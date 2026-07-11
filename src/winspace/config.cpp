// Config parser + semantic input types — core (pure, no <windows.h>).
//
// Seam 2: parse(text) -> (config, diagnostics). Owns the vocabulary the parser
// produces — Mod, Key, Dispatcher, Bind — none of them Win32 constants; the I/O
// adapter (task 05) maps Mod->MOD_* and Key->VK_*. The test TU links no WM
// libraries, so any stray OS call reachable from here is a link error.
//
// The grammar is a strict subset of the eventual Hyprland DSL (issue 09 only
// adds directives, it never reshapes this parser):
//   * `#` comments (whole-line or trailing)
//   * `$name = tokens` variable definitions, referenced as `$name`
//   * `bind = MODS, KEY, dispatcher, args`  (dispatcher in { workspace, quit,
//     focus, movetoworkspace, movetoworkspacesilent })
//
// A malformed line yields a diagnostic and parsing continues. "Keep last good
// config" retention and live reload are out of scope (issue 09).
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "winspace/reducer.cpp"  // WindowRule, Field — defined once in the core

namespace winspace {

// ── semantic types owned by core (never Win32 constants) ────────────────────

// Modifier bitflags, OR-combined into a Bind's modifier set. The four Hyprland
// modifiers; grown additively if needed.
enum class Mod : uint8_t {
    None = 0,
    Super = 1,
    Alt = 2,
    Ctrl = 4,
    Shift = 8,
};

constexpr Mod operator|(Mod a, Mod b) {
    return static_cast<Mod>(std::to_underlying(a) | std::to_underlying(b));
}
constexpr Mod& operator|=(Mod& a, Mod b) {
    a = a | b;
    return a;
}
constexpr bool contains(Mod set, Mod flag) {
    return (std::to_underlying(set) & std::to_underlying(flag)) != 0;
}

// Every key a Bind can name; the I/O adapter (task 05) maps each to a VK_*. Each
// contiguous run (digits, letters, function keys) lets parse_key map by offset.
enum class Key : uint8_t {
    // Digits 0-9 — a digit char maps by offset from N0.
    N0, N1, N2, N3, N4, N5, N6, N7, N8, N9,
    // Letters A-Z — a letter char maps by offset from A.
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    // Function keys F1-F24 — "F<n>" maps by offset from F1.
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    // Navigation and editing (named in the config; see parse_key for aliases).
    Left, Right, Up, Down,
    Home, End, PageUp, PageDown,
    Insert, Delete,
    // Whitespace and control.
    Return, Space, Tab, Escape, Backspace,
};

// The direction a `focus` bind steers keyboard focus. A peer of Mod/Key/
// Dispatcher, but nested in a namespace so it stays a DISTINCT type from
// reducer::Direction (both would otherwise be winspace::Direction and collide in
// the app/test TUs that include both headers). hotkeys.cpp's toEvent translates
// this config::Direction → reducer::Direction at the seam.
namespace config {
enum class Direction : uint8_t { Left, Right, Up, Down };
}

// Dispatchers recognized this slice. An unknown name is diagnosed here, not
// deferred to registration (task 05).
enum class Dispatcher : uint8_t {
    Workspace,
    Quit,
    Focus,
    MoveToWorkspace,        // movetoworkspace N — move + follow (switch to N)
    MoveToWorkspaceSilent,  // movetoworkspacesilent N — move, stay on current
};

// A parsed bind line. `arg` carries the workspace number for Workspace and the two
// MoveToWorkspace forms (unused, 0, otherwise); `dir` carries the direction for
// Focus (unused, Left, otherwise). A bind's key and its direction are independent fields.
struct Bind {
    Mod mods = Mod::None;
    Key key{};
    Dispatcher dispatcher{};
    int arg = 0;
    config::Direction dir{};
};

constexpr bool operator==(const Bind& a, const Bind& b) {
    return a.mods == b.mods && a.key == b.key &&
           a.dispatcher == b.dispatcher && a.arg == b.arg && a.dir == b.dir;
}

// A per-line problem. `line` is 1-based. Valid binds on other lines still parse.
struct Diagnostic {
    int line = 0;
    std::string message;
};

struct Config {
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
    std::vector<ExecEntry> execs;
};

// Both halves of the return tuple exist from day one.
struct ParseResult {
    Config config;
    std::vector<Diagnostic> diagnostics;
};

// ── parsing internals ───────────────────────────────────────────────────────

namespace detail {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline bool is_ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline bool is_hspace(char c) { return c == ' ' || c == '\t'; }

// Rebuild a string_view over a contiguous subrange (e.g. a split piece); its
// endpoints are const char* into the original text.
inline std::string_view as_view(std::ranges::contiguous_range auto&& r) {
    return std::string_view(std::ranges::begin(r), std::ranges::end(r));
}

inline std::string_view trim(std::string_view s) {
    const auto is_sp = [](char c) { return is_space(c); };
    // Drop leading, then trailing, whitespace (the latter off the reversed
    // remainder). reverse_iterator::base() unwraps back to const char* bounds —
    // back.end() at the first kept char, back.begin() one past the last — so an
    // all-whitespace input collapses to an empty range, never an inverted one.
    auto front = s | std::views::drop_while(is_sp);
    auto back = front | std::views::reverse | std::views::drop_while(is_sp);
    return std::string_view(back.end().base(), back.begin().base());
}

// Whitespace-separated tokens (the MODS field, e.g. "SUPER ALT"). chunk_by groups
// runs of one character class, collapsing spaces/tabs; keep the non-space chunks.
inline std::vector<std::string_view> split_ws(std::string_view s) {
    const auto same_class = [](char a, char b) { return is_hspace(a) == is_hspace(b); };
    std::vector<std::string_view> out;
    for (const auto chunk : s | std::views::chunk_by(same_class)) {
        if (!is_hspace(chunk.front())) out.push_back(as_view(chunk));
    }
    return out;
}

// Comma-separated, each field trimmed. A trailing comma yields a trailing empty
// field (diagnosed downstream where an argument is required).
inline std::vector<std::string_view> split_comma(std::string_view s) {
    std::vector<std::string_view> out;
    for (const auto field : s | std::views::split(',')) out.push_back(trim(as_view(field)));
    return out;
}

inline std::optional<Mod> parse_mod(std::string_view t) {
    if (t == "SUPER") return Mod::Super;
    if (t == "ALT") return Mod::Alt;
    if (t == "CTRL") return Mod::Ctrl;
    if (t == "SHIFT") return Mod::Shift;
    return std::nullopt;
}

inline std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

inline bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

inline std::optional<Key> parse_key(std::string_view t) {
    // A single digit or letter (case-insensitive) maps by offset.
    if (t.size() == 1) {
        const char c = t[0];
        if (c >= '0' && c <= '9')
            return static_cast<Key>(std::to_underlying(Key::N0) + (c - '0'));
        if (c >= 'A' && c <= 'Z')
            return static_cast<Key>(std::to_underlying(Key::A) + (c - 'A'));
        if (c >= 'a' && c <= 'z')
            return static_cast<Key>(std::to_underlying(Key::A) + (c - 'a'));
        return std::nullopt;
    }

    // Function keys F1..F24 (multi-char starting with F/f + a number). The upper
    // bound is derived from the enum, so it tracks the F1..F24 run automatically.
    if ((t[0] == 'F' || t[0] == 'f') && t.size() >= 2) {
        constexpr int first_fn = 1;
        constexpr int last_fn =
            first_fn + (std::to_underlying(Key::F24) - std::to_underlying(Key::F1));
        int n = 0;
        const auto [ptr, ec] = std::from_chars(t.data() + 1, t.data() + t.size(), n);
        if (ec == std::errc{} && ptr == t.data() + t.size() && n >= first_fn && n <= last_fn)
            return static_cast<Key>(std::to_underlying(Key::F1) + (n - first_fn));
        return std::nullopt;
    }

    // Named navigation / editing / control keys, with the common aliases.
    static constexpr std::pair<std::string_view, Key> named[] = {
        {"left", Key::Left},       {"right", Key::Right},
        {"up", Key::Up},           {"down", Key::Down},
        {"home", Key::Home},       {"end", Key::End},
        {"pageup", Key::PageUp},   {"pgup", Key::PageUp},
        {"pagedown", Key::PageDown}, {"pgdn", Key::PageDown},
        {"insert", Key::Insert},   {"ins", Key::Insert},
        {"delete", Key::Delete},   {"del", Key::Delete},
        {"return", Key::Return},   {"enter", Key::Return},
        {"space", Key::Space},     {"tab", Key::Tab},
        {"escape", Key::Escape},   {"esc", Key::Escape},
        {"backspace", Key::Backspace},
    };
    for (const auto& [name, key] : named)
        if (iequals(t, name)) return key;
    return std::nullopt;
}

inline std::optional<Dispatcher> parse_dispatcher(std::string_view t) {
    if (t == "workspace") return Dispatcher::Workspace;
    if (t == "quit") return Dispatcher::Quit;
    if (t == "focus") return Dispatcher::Focus;
    if (t == "movetoworkspace") return Dispatcher::MoveToWorkspace;
    if (t == "movetoworkspacesilent") return Dispatcher::MoveToWorkspaceSilent;
    return std::nullopt;
}

// The four canonical direction words, case-insensitive, no aliases (users bind
// whatever KEY they like to these — h/j/k/l are keys, not direction words).
inline std::optional<config::Direction> parse_direction(std::string_view t) {
    if (iequals(t, "left")) return config::Direction::Left;
    if (iequals(t, "right")) return config::Direction::Right;
    if (iequals(t, "up")) return config::Direction::Up;
    if (iequals(t, "down")) return config::Direction::Down;
    return std::nullopt;
}

// Substitute every `$name` reference from `vars`. On an undefined reference,
// returns nullopt and writes the offending name to `missing`.
inline std::optional<std::string> expand_vars(
    std::string_view s,
    const std::unordered_map<std::string, std::string>& vars,
    std::string& missing) {
    // Walk the input into pieces, then join them: a `$name` reference (a `$`
    // followed by identifier characters) becomes the variable's value, and each
    // literal run in between passes through unchanged. Pieces are views into `s`
    // or `vars`, which both outlive this call.
    std::vector<std::string_view> pieces;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '$') {
            const std::string_view rest = s.substr(i + 1);
            const size_t len = static_cast<size_t>(
                std::ranges::distance(rest | std::views::take_while(is_ident)));
            const std::string name(rest.substr(0, len));
            const auto it = vars.find(name);
            if (it == vars.end()) {
                missing = name;
                return std::nullopt;
            }
            pieces.push_back(it->second);
            i += 1 + len;
        } else {
            const std::string_view rest = s.substr(i);
            const size_t len = static_cast<size_t>(std::ranges::distance(
                rest | std::views::take_while([](char c) { return c != '$'; })));
            pieces.push_back(rest.substr(0, len));
            i += len;
        }
    }
    return pieces | std::views::join | std::ranges::to<std::string>();
}

}  // namespace detail

// ── the seam ──────────────────────────────────────────────────────────────

inline ParseResult parse(std::string_view text) {
    using namespace detail;

    ParseResult result;
    std::unordered_map<std::string, std::string> vars;

    int line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string_view raw =
            (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++line_no;

        // Strip a `#` comment (line or trailing), then trim.
        if (const size_t h = raw.find('#'); h != std::string_view::npos)
            raw = raw.substr(0, h);
        const std::string_view line = trim(raw);
        if (line.empty()) continue;

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            result.diagnostics.push_back({line_no, "expected 'name = value'"});
            continue;
        }
        const std::string_view lhs = trim(line.substr(0, eq));
        const std::string_view rhs = trim(line.substr(eq + 1));

        // `$name = tokens` — variable definition. Values are expanded at
        // definition time, so a reference to an earlier variable is resolved
        // once and stored flat.
        if (!lhs.empty() && lhs.front() == '$') {
            const std::string name(lhs.substr(1));
            if (name.empty()) {
                result.diagnostics.push_back({line_no, "empty variable name"});
                continue;
            }
            std::string missing;
            auto expanded = expand_vars(rhs, vars, missing);
            if (!expanded) {
                result.diagnostics.push_back({line_no, "undefined variable $" + missing});
                continue;
            }
            vars[name] = std::move(*expanded);
            continue;
        }

        // `bind = MODS, KEY, dispatcher, args`
        if (lhs == "bind") {
            std::string missing;
            auto expanded = expand_vars(rhs, vars, missing);
            if (!expanded) {
                result.diagnostics.push_back({line_no, "undefined variable $" + missing});
                continue;
            }
            const auto fields = split_comma(*expanded);
            if (fields.size() < 3) {
                result.diagnostics.push_back(
                    {line_no, "bind needs at least MODS, KEY, dispatcher"});
                continue;
            }

            Mod mods = Mod::None;
            bool mods_ok = true;
            for (const auto tok : split_ws(fields[0])) {
                const auto m = parse_mod(tok);
                if (!m) {
                    result.diagnostics.push_back(
                        {line_no, "unknown modifier '" + std::string(tok) + "'"});
                    mods_ok = false;
                    break;
                }
                mods |= *m;
            }
            if (!mods_ok) continue;

            const auto key = parse_key(fields[1]);
            if (!key) {
                result.diagnostics.push_back(
                    {line_no, "unknown key '" + std::string(fields[1]) + "'"});
                continue;
            }

            const auto disp = parse_dispatcher(fields[2]);
            if (!disp) {
                result.diagnostics.push_back(
                    {line_no, "unknown dispatcher '" + std::string(fields[2]) + "'"});
                continue;
            }

            Bind bind;
            bind.mods = mods;
            bind.key = *key;
            bind.dispatcher = *disp;

            // workspace and both movetoworkspace forms take a Logical workspace
            // number in the same arg slot, parsed identically.
            const bool needsWorkspaceArg = *disp == Dispatcher::Workspace ||
                                           *disp == Dispatcher::MoveToWorkspace ||
                                           *disp == Dispatcher::MoveToWorkspaceSilent;
            if (needsWorkspaceArg) {
                if (fields.size() < 4 || fields[3].empty()) {
                    result.diagnostics.push_back(
                        {line_no, "workspace dispatcher needs a workspace number"});
                    continue;
                }
                const std::string_view arg = fields[3];
                int n = 0;
                const auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
                if (ec != std::errc{} || ptr != arg.data() + arg.size()) {
                    result.diagnostics.push_back(
                        {line_no, "workspace argument must be an integer: '" +
                                      std::string(arg) + "'"});
                    continue;
                }
                bind.arg = n;
            } else if (*disp == Dispatcher::Focus) {
                if (fields.size() < 4 || fields[3].empty()) {
                    result.diagnostics.push_back(
                        {line_no, "focus dispatcher needs a direction (left|right|up|down)"});
                    continue;
                }
                const auto dir = parse_direction(fields[3]);
                if (!dir) {
                    result.diagnostics.push_back(
                        {line_no, "unknown direction '" + std::string(fields[3]) + "'"});
                    continue;
                }
                bind.dir = *dir;
            }
            // quit takes no argument; any extra field is ignored.

            result.config.binds.push_back(bind);
            continue;
        }

        // `windowrule = workspace N, <field>:<pattern>` — pin a matching app to a
        // Workspace on Appeared (PRD 06). NO $var expansion: a title regex may end
        // in `$` (the end-anchor), which expand_vars would misread as a reference.
        if (lhs == "windowrule") {
            // Split the RHS on the FIRST comma (action vs. match spec) so a title
            // regex may contain commas. rhs is already trimmed, so the spec's
            // trailing whitespace is gone; the field half is trimmed below.
            const size_t comma = rhs.find(',');
            if (comma == std::string_view::npos) {
                result.diagnostics.push_back(
                    {line_no, "windowrule needs 'workspace N, <field>:<pattern>'"});
                continue;
            }
            const std::string_view action = trim(rhs.substr(0, comma));
            const std::string_view spec = rhs.substr(comma + 1);

            // Action must be `workspace N`; `ignore` and other Hyprland forms are
            // unsupported this slice (diagnosed, not aborting the file).
            const auto atoks = split_ws(action);
            if (atoks.empty() || atoks[0] != "workspace") {
                result.diagnostics.push_back(
                    {line_no, "unsupported windowrule action '" + std::string(action) +
                                  "' (only 'workspace N')"});
                continue;
            }
            if (atoks.size() < 2) {
                result.diagnostics.push_back(
                    {line_no, "windowrule workspace needs a number"});
                continue;
            }
            int n = 0;
            const std::string_view narg = atoks[1];
            const auto [ptr, ec] = std::from_chars(narg.data(), narg.data() + narg.size(), n);
            if (ec != std::errc{} || ptr != narg.data() + narg.size()) {
                result.diagnostics.push_back(
                    {line_no, "windowrule workspace must be an integer: '" +
                                  std::string(narg) + "'"});
                continue;
            }

            // Split the spec on the FIRST colon (field vs. verbatim pattern) so a
            // title regex may contain colons.
            const size_t colon = spec.find(':');
            if (colon == std::string_view::npos) {
                result.diagnostics.push_back(
                    {line_no, "windowrule match needs '<field>:<pattern>'"});
                continue;
            }
            const std::string_view fieldStr = trim(spec.substr(0, colon));
            const std::string_view pattern = spec.substr(colon + 1);  // verbatim tail

            WindowRule rule;
            rule.workspace = n;
            if (iequals(fieldStr, "exe")) rule.field = Field::Exe;
            else if (iequals(fieldStr, "class")) rule.field = Field::Class;
            else if (iequals(fieldStr, "title")) rule.field = Field::Title;
            else {
                result.diagnostics.push_back(
                    {line_no, "unknown windowrule field '" + std::string(fieldStr) +
                                  "' (expected exe|class|title)"});
                continue;
            }

            if (rule.field == Field::Title) {
                // Title pattern is verbatim; compiled here so a std::regex_error
                // becomes a Diagnostic (not a load-time throw). Case-sensitive.
                if (pattern.empty()) {
                    result.diagnostics.push_back(
                        {line_no, "windowrule title pattern is empty"});
                    continue;
                }
                try {
                    rule.regex = std::regex(std::string(pattern));
                } catch (const std::regex_error& e) {
                    result.diagnostics.push_back(
                        {line_no, "invalid title regex: " + std::string(e.what())});
                    continue;
                }
                rule.pattern = std::string(pattern);
            } else {
                // exe/class stored lowercased + whitespace-trimmed.
                const std::string_view trimmed = trim(pattern);
                if (trimmed.empty()) {
                    result.diagnostics.push_back(
                        {line_no, "windowrule pattern is empty"});
                    continue;
                }
                rule.pattern = to_lower(trimmed);
            }

            result.config.rules.push_back(std::move(rule));
            continue;
        }

        // `exec = <cmd>` / `exec-once = <cmd>` — declare an app to launch (PRD 08,
        // ADR-0011). The command is the VERBATIM tail after `=` (may hold spaces,
        // commas, quotes, `$`), stored unparsed into one source-ordered list so the
        // launch order across interleaved exec / exec-once lines is preserved. NO
        // $var expansion — winspace vars are modifier names, and a literal `$` in a
        // path or arg must pass through (mirrors windowrule's verbatim-tail rule).
        // `rhs` is the already-trimmed remainder; an empty tail is diagnosed and
        // skipped. Placement is a paired `windowrule`, never the launcher's job.
        if (lhs == "exec" || lhs == "exec-once") {
            if (rhs.empty()) {
                result.diagnostics.push_back(
                    {line_no, std::string(lhs) + " needs a command"});
                continue;
            }
            result.config.execs.push_back({std::string(rhs), lhs == "exec-once"});
            continue;
        }

        result.diagnostics.push_back(
            {line_no, "unknown directive '" + std::string(lhs) + "'"});
    }

    return result;
}

}  // namespace winspace
