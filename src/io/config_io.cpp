// Config I/O helper — I/O adapter (owns <windows.h> / <filesystem>).
//
// The single home for "find the config file, read it, parse it" — the path
// resolution, file read, and parse() call that BOTH the process spine's initial
// load (io/app.cpp) and the Worker's live reload (io/worker.cpp, ADR-0012) drive.
// Extracted here (a prefactor) so neither owner duplicates the path/read/
// parse logic, and so worker.cpp — which is #included before app.cpp in the unity
// TU — can reach it.
//
// The two callers layer DIFFERENT fallback policies on top of the same read+parse
// (the asymmetry ADR-0012 records):
//   * startup   — seed the built-in default on first run, degrade per-line, and if
//                 the file is unreadable fall back to the built-in default;
//   * reload    — keep the last-good running config on ANY diagnostic or an
//                 unreadable file (atomic), applying only a clean parse.
// Both are expressed against readAndParseConfig() below; the policy lives in the
// caller, the mechanism lives here.
#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "io/error.cpp"          // lg:: levels + narrow()
#include "winspace/config.cpp"   // parse, Config, ParseResult, Bind
#include "winspace/reducer.cpp"  // WindowRule, ExecEntry

namespace winspace::io {

// The config winspace seeds on first run and falls back to when the on-disk file
// is unreadable. It is the Hyprland-subset grammar winspace supports; a user who
// edits the seeded file grows it from here.
inline constexpr std::string_view k_defaultConfig =
    "# winspace config — edit and save, then hit your `reload` bind (or restart).\n"
    "# $mod = SUPER binds the Windows key. Win+<key> only registers when the\n"
    "# NoWinKeys policy is on (HKCU\\...\\Policies\\Explorer\\NoWinKeys=1); without\n"
    "# it Windows reserves these chords and winspace skips-and-logs each bind.\n"
    "$mod = SUPER\n"
    "bind = $mod, 1, workspace, 1\n"
    "bind = $mod, 2, workspace, 2\n"
    "bind = $mod, 3, workspace, 3\n"
    "bind = $mod, 4, workspace, 4\n"
    "bind = $mod, 5, workspace, 5\n"
    "bind = $mod, Q, quit\n"
    "# Re-read this file live. Edit, save, hit the bind — hotkeys, window rules, and\n"
    "# launch entries all re-apply with no restart. A file with any error is rejected\n"
    "# whole and the last good config stays live (so a typo never breaks your setup).\n"
    "bind = $mod SHIFT, R, reload\n"
    "# Spatial focus: vim-style Alt + h/j/k/l steer the keyboard to the nearest window\n"
    "# in a direction. Alt (not $mod) because Windows reserves the whole Win+h/j/k/l set\n"
    "# even under NoWinKeys (Win+J is Explorer's, Win+K/L are system-reserved), while\n"
    "# Alt+h/j/k/l registers cleanly. The bound key and the direction are independent\n"
    "# fields, so rebind freely (e.g. to the arrow keys).\n"
    "bind = ALT, H, focus, left\n"
    "bind = ALT, J, focus, down\n"
    "bind = ALT, K, focus, up\n"
    "bind = ALT, L, focus, right\n"
    "# Move the focused window to a workspace (Super+Shift+<n>, the hyprland idiom).\n"
    "# `movetoworkspacesilent` moves it but leaves you where you are; the plain\n"
    "# `movetoworkspace` would also follow the window (switch the active desktop to N).\n"
    "# A cross-desktop move is cloaked (DWM) around the move so it never flashes here.\n"
    "bind = $mod SHIFT, 1, movetoworkspacesilent, 1\n"
    "bind = $mod SHIFT, 2, movetoworkspacesilent, 2\n"
    "bind = $mod SHIFT, 3, movetoworkspacesilent, 3\n"
    "bind = $mod SHIFT, 4, movetoworkspacesilent, 4\n"
    "bind = $mod SHIFT, 5, movetoworkspacesilent, 5\n"
    "# Launch apps at startup. `exec-once` runs once when winspace starts; `exec`\n"
    "# runs at startup and again on every config reload. Each line is a verbatim\n"
    "# command (exe + args, quoted paths for spaces), started detached — it keeps\n"
    "# running after winspace exits. The launcher only STARTS apps; it never places\n"
    "# them. To put a launched app on a workspace, pair it with a `windowrule` that\n"
    "# matches the window by exe. Example: start Firefox and pin it to workspace 2.\n"
    "# exec-once = firefox\n"
    "# windowrule = workspace 2, exe:firefox.exe\n"
    "# Exclude a dock or always-on-top widget from directional focus (it is never a\n"
    "# focus target, but stays Alt-Tab reachable and is never moved or sized).\n"
    "# windowrule = ignore, class:Shell_TrayWnd\n"
    "# Start winspace with your session (registered by the logon task).\n"
    "# start_at_login = false\n";

// Read an environment variable with the size-then-fill two-call pattern.
// GetEnvironmentVariableW returns the length INCLUDING the null when probing with
// a null buffer, EXCLUDING it when filling — so allocate n and fill n. Empty
// optional means the variable is unset (or unreadable): there is no home to anchor
// the config path to.
inline std::optional<std::wstring> envVar(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n - 1, L'\0');  // n counts the terminator; string owns size()+1
    GetEnvironmentVariableW(name, value.data(), n);
    return value;
}

// The known location: %USERPROFILE%\.config\winspace\winspace.conf — the Win32
// home for the Hyprland-style ~/.config/<app> layout. Empty optional when
// %USERPROFILE% is unset, which leaves winspace on its built-in defaults.
inline std::optional<std::filesystem::path> configPath() {
    const std::optional<std::wstring> home = envVar(L"USERPROFILE");
    if (!home) return std::nullopt;
    return std::filesystem::path(*home) / L".config" / L"winspace" / L"winspace.conf";
}

// Slurp a file's bytes verbatim (the config is UTF-8, which the parser consumes as
// bytes). Empty optional on any open failure.
inline std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return std::move(buffer).str();
}

// First-run only: if the config file does not yet exist, create the directory chain
// and seed it with the built-in default. Best effort — a missing home or an
// unwritable disk logs a diagnostic and returns, leaving readAndParseConfig() to
// report the file as unreadable (the caller then falls back to the default text).
// Reload never calls this: a reload of a deleted file keeps the last good config.
inline void seedDefaultConfigIfMissing() {
    const std::optional<std::filesystem::path> path = configPath();
    if (!path) {
        lg::warn("config: %USERPROFILE% is unset; using built-in defaults");
        return;
    }
    const std::string shown = narrow(path->wstring());

    std::error_code ec;
    if (std::filesystem::exists(*path, ec)) return;

    std::filesystem::create_directories(path->parent_path(), ec);
    if (ec) {
        lg::warn("config: cannot create {}: {}; using built-in defaults",
                 narrow(path->parent_path().wstring()), ec.message());
        return;
    }
    std::ofstream out(*path, std::ios::binary);
    out << k_defaultConfig;
    if (!out) {
        lg::warn("config: cannot write default {}; using built-in defaults", shown);
        return;
    }
    lg::info("config: seeded default config at {}", shown);
}

// The outcome of resolving + reading + parsing the on-disk config — the shared
// mechanism both the spine and the Worker drive. `read` is false when the path
// could not be resolved (unset %USERPROFILE%) or the file could not be opened (no
// file yet, unreadable); the parse is then default-empty. Callers layer their own
// fallback: startup seeds/keeps the built-in default, reload keeps the last-good
// running config. On a successful read, `parsed` is exactly parse(fileBytes).
// (Distinct from LoadedConfig below: this is the RAW read+parse outcome; that is
// the consumable binds/rules/execs the threads borrow.)
struct ConfigReadResult {
    bool read = false;
    ParseResult parsed;
};

// Resolve the path, read the file, parse it. The one place the path/read/parse
// chain lives; startup and reload both call it and differ only in the fallback
// policy they apply to the result.
inline ConfigReadResult readAndParseConfig() {
    const std::optional<std::filesystem::path> path = configPath();
    if (!path) return {};
    std::optional<std::string> text = readFile(*path);
    if (!text) return {};
    return {true, parse(*text)};
}

// Both halves of one parse plus the flat setting: the Binds the Hotkey thread
// registers, the WindowRules and Launch entries the Worker seeds into State, and
// the start_at_login flag. Owned by whoever loaded it for the lifetime of the
// threads that borrow it.
struct LoadedConfig {
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
    std::vector<ExecEntry> execs;
    bool startAtLogin = false;
};

// Move a parsed Config into the LoadedConfig the threads consume.
inline LoadedConfig toLoaded(Config&& c) {
    return {std::move(c.binds), std::move(c.rules), std::move(c.execs), c.startAtLogin};
}

}  // namespace winspace::io
