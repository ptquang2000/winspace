// winspace core — the pure heart of winspace (NO <windows.h>, NO COM, NO OS calls).
//
// One of the project's two production translation-unit sources. This file holds
// the entire pure core: the Reducer (Seam 1) and the config parser (Seam 2),
// each preserved below as a banner-delimited section carrying its original
// doc-header. It is #included by win32.cpp (the app TU) and by winspace_test.cpp
// (the test TU) — and by nothing else.
//
// PURITY IS LINKER-ENFORCED: winspace_test.cpp compiles this file and links ZERO
// WM import libraries, so any stray OS call reachable from here becomes a *link*
// error, not a runtime surprise. Keep this file free of <windows.h> and COM.
//
// Section order is dependency order: `reducer` defines the vocabulary
// (WindowRule, Field, Event, Effect, State, detail::overload) that the `config`
// parser section below consumes.
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// [core section] reducer
// ─────────────────────────────────────────────────────────────────────────────

// The central seam: a pure Reducer — core (pure, no <windows.h>).
//
// Seam 1: reduce(state, event) -> (newState, effects). The Reducer is the only
// place behavioral logic lives; it consumes an Event, returns fresh State by
// value, and emits Effects as plain data. It reasons purely in Logical
// workspace numbers — it never sees a Virtual Desktop GUID (the logical->GUID
// map, adoption, and create-on-demand all live in the bridge).
//
// winspace owns no window geometry (ADR-0007): no Effect ever moves or sizes a
// window. What remains here on the window side is the Eligibility substrate —
// strong identities and the pure `isEligible` gate — which the Spatial-focus
// slice builds on and the window rules reuse. The tiling machinery
// (layout, PositionWindow, focus order, the monitor model, and the
// Appeared/Vanished lifecycle stream) was removed with tiling.
//
// The test TU links no WM libraries, so any stray OS call reachable from here
// is a link error — the linker enforces purity, not discipline. Behavior is
// tested through the emitted Effects, never by inspecting State internals.


namespace winspace {

// ── the vocabulary: Event, Effect, State (aggregates over a variant) ────────

// ── window & monitor identities and plain-data facts ────────────────────────

// Strong, opaque identities minted by the I/O adapters (an HWND / HMONITOR
// stamped into an integer). The core never does arithmetic on them — it only
// stores, compares, and hands them back on Effects.
enum class WindowId : uint64_t {};
enum class MonitorId : uint64_t {};

// A logical rectangle in virtual-screen coordinates. Plain data; the Reducer
// reasons in these (directional focus resolves over them). The
// drop-shadow delta and DPI never enter the core.
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// The Probe result: one window's live attributes, read reactively when needed
// (on a `focus` keypress). `id`, the monitor it sits on, its live
// `rect` in virtual-screen coordinates (the directional resolution reasons over
// these), and the eligibility booleans — the Eligibility gate is the pure AND of
// those facts. The rect is probed together with the eligibility facts in one read.
struct WindowAttrs {
    WindowId id{};
    MonitorId monitor{};
    Rect rect{};
    bool topLevel = false;
    bool visible = false;
    bool thickFrame = false;
    bool caption = false;
    bool toolWindow = false;
    bool cloaked = false;
    bool fullscreen = false;
};

// The string half of a window Probe — the identity a WindowRule matches against
// (ADR-0006's Probe/policy split, string side). Gathered only on the `Appeared`
// path (rules need it; the focus sweep never does, so strings stay off that hot
// path). Plain UTF-8 std::string, narrowed at the adapter so the core stays
// wchar_t-free. Distinct from WindowAttrs, which carries the Eligibility facts.
struct WindowIdentity {
    WindowId id{};
    std::string exe;
    std::string windowClass;
    std::string title;
};

// One Display and the COUNT of Eligible windows it carries on the current
// Workspace — the occupancy fact the Distribute round-trip probes (ADR-0020; was a
// bare `bool occupied` under ADR-0021's Spread). A count, not a flag, so
// "least-occupied" can still balance when windows outnumber displays (two windows
// on a display read `count == 2`, not merely "occupied"). Built by the Worker
// (which enumerates Displays and counts windows), consumed by the pure Reducer via
// pickDistributeTarget. The subject window is excluded upstream by the Worker, so
// its own opening Display never counts itself (exclude-self).
struct DisplayOccupancy {
    MonitorId id{};
    int count = 0;
};

// One probed window: the (attrs, identity) pair — the same pair an Appeared
// carries, batched per window for the `tile` sweep (ADR-0016). Unlike the focus
// sweep (WindowAttrs only), tile must also match rules, so it gathers the identity
// strings too. TileResolve carries a vector of these back to the pure Reducer.
struct ProbedWindow {
    WindowAttrs attrs;
    WindowIdentity identity;
};

// The one match field a WindowRule names. exe/class compare exact and
// ASCII-case-insensitive; title is a std::regex_search (substring, case-sensitive).
enum class Field { Exe, Class, Title };

// The action a matching WindowRule applies (ADR-0009 extension, ADR-0020 reversal):
// Place pins the window to a target Workspace on Appeared (and its Slot, if any);
// Ignore widens to "don't touch at all" — the window is neither a Spatial-focus
// target nor auto-placed (ADR-0020). Both actions are explicit user intent that
// opts a window OUT of Distribute; an unmatched window is Distributed instead. The
// old `Spread` action is gone — distribution is now automatic for every eligible
// window, not a per-rule opt-in.
enum class RuleAction { Place, Ignore };

// A Slot: a symbolic named fraction of a Display work area (ADR-0016), the
// optional geometry target of a Place WindowRule. It is NOT a stored rect —
// winspace computes the rect live from the target Display's work area at write
// time (rectForSlot) and forgets it, so nothing can go stale (ADR-0007's
// stale-rect rejection answered by construction). Named in config as kebab-case
// tokens (left-half, top-left-quarter, maximized, …). `maximized` is special-cased
// at the adapter to OS SW_MAXIMIZE; the rest become a SetWindowPos to their rect.
enum class Slot {
    LeftHalf,
    RightHalf,
    TopHalf,
    BottomHalf,
    TopLeftQuarter,
    TopRightQuarter,
    BottomLeftQuarter,
    BottomRightQuarter,
    Maximized,
};

// A parsed `windowrule = <action>, <field>:<pattern>`. Lives in the core next to WindowAttrs; the parser (Seam 2)
// compiles these and the reducer matches against them. For Exe/Class, `pattern` is
// the (lowercased, trimmed) literal to compare; for Title, `regex` is the compiled
// pattern and `pattern` keeps the source text (diagnostics only). `workspace` is
// the target Logical number for a Place rule (unused for Ignore). `slot` is the
// optional geometry target of a Place rule (ADR-0016) — nullopt is exactly the
// pre-ADR-0016 behavior (Workspace pin only, no geometry), so all existing configs
// are unaffected; meaningless on an Ignore rule (the parser diagnoses `slot` there).
// std::regex makes this copyable-but-not-cheap, which is why State holds the rule
// list behind a shared_ptr (O(1) per-event State copies).
struct WindowRule {
    Field field{};
    RuleAction action = RuleAction::Place;
    int workspace = 0;
    std::string pattern;
    std::regex regex;
    std::optional<Slot> slot;
};

// A parsed `exec = <cmd>` / `exec-once = <cmd>` launch entry (ADR-0011).
// `command` is the verbatim command line (unexpanded — no $var); `once` is true
// for exec-once (start only, not on reload). Launch-only: no target Workspace,
// no PID — placement is a paired `windowrule`. Lives in the core next to
// WindowRule; the parser (Seam 2) collects these in source order and the reducer
// turns each into a LaunchApp Effect. State holds the list behind a shared_ptr for
// O(1) per-event copies, mirroring `rules` (ADR-0009).
struct ExecEntry {
    std::string command;
    bool once = false;
};

constexpr bool operator==(const ExecEntry& a, const ExecEntry& b) {
    return a.command == b.command && a.once == b.once;
}

// The four directions a `focus` press can steer keyboard focus. Nested in a
// namespace so it stays a DISTINCT type from config::Direction — both would
// otherwise be winspace::Direction and collide in the app/test TUs that include
// both headers. hotkeys.cpp's toEvent translates config::Direction →
// reducer::Direction at the seam, mirroring the existing Dispatcher → Event and
// Mod → MOD_* translations (ADR-0008).
namespace reducer {
enum class Direction { Left, Right, Up, Down };
}

// Events — what the outside world asks the Reducer to do. Produced by the I/O
// adapters (a WM_HOTKEY becomes one of these) and fed in as plain data.
struct WorkspaceSwitch {
    int n = 0;  // target Logical workspace number
};
struct Quit {};

// Spatial focus is a two-phase probe round-trip (ADR-0008): an Effect cannot hand
// data back to the pure Reducer, so the Probed rects re-enter as a second Event.
// FocusMove is what the Hotkey thread posts (it knows only which key fired);
// FocusResolve is what the Worker posts back to itself after the Probe sweep.
struct FocusMove {
    reducer::Direction dir{};
};
struct FocusResolve {
    reducer::Direction dir{};
    std::vector<WindowAttrs> candidates;    // the full probed set, unfiltered
    std::optional<WindowAttrs> origin;      // the foreground window; nullopt => no-op
};

// The `tile` dispatcher (ADR-0016) — a two-phase Probe sweep mirroring Spatial
// focus (FocusMove → ResolveFocus → FocusResolve). Tile is what the Hotkey thread
// posts (the bind fired); the Worker runs the sweep and posts TileResolve back to
// itself with the probed windows. Unlike the focus sweep, tile gathers identity
// too (it must match rules), so its payload is ProbedWindows, not bare WindowAttrs.
struct Tile {};
struct TileResolve {
    std::vector<ProbedWindow> windows;    // the full probed set, unfiltered
    std::vector<MonitorId> displays;      // every live Display, for count-balancing
};

// movetoworkspace / movetoworkspacesilent: move the FOREGROUND window
// to a Logical workspace. `follow` = the plain form (the active desktop also
// becomes N); the silent form stays put. Ungated by Eligibility — the user aimed
// at the foreground window explicitly. Unlike focus (ADR-0008) there is no pure
// decision to make, so no probe round-trip: the Worker resolves the foreground
// window inline at execute time (degrade-and-log if there is none).
struct MoveToWorkspace {
    int logical = 0;    // target Logical workspace number
    bool follow = false;
};

// The window-lifecycle edges delivered by the SetWinEventHook adapter,
// reintroduced with window rules. Appeared = EVENT_OBJECT_SHOW / _UNCLOAKED — it
// carries a Probed WindowAttrs (the Eligibility facts) AND a WindowIdentity (the
// strings a rule matches); the adapter hands over facts, never a verdict, so the
// reducer runs isEligible itself (ADR-0006). Vanished = EVENT_OBJECT_DESTROY /
// _HIDE / _CLOAKED — just a WindowId (no probe of a possibly-dead HWND).
struct Appeared {
    WindowAttrs attrs;
    WindowIdentity identity;
};
struct Vanished {
    WindowId id{};
};

// Phase two of the Distribute round-trip (ADR-0020, was Spread's SpreadResolve
// under ADR-0021), mirroring focus's FocusResolve (ADR-0008): an Effect cannot hand
// data back to the pure Reducer, so the probed Display occupancy re-enters as a
// second Event. Posted by the Worker after it executes ResolveDistribute —
// `subject` is the window to place, `subjectMonitor` is the Display it currently
// sits on (so the tie-break can prefer keeping it put), and `displays` lists every
// Display with its window count (the subject already excluded from the counts). The
// Reducer runs pickDistributeTarget and emits exactly one PositionWindow.
struct DistributeResolve {
    WindowId subject{};
    MonitorId subjectMonitor{};
    std::vector<DisplayOccupancy> displays;
};

// Launcher lifecycle Events. Started = the spine posted it once the
// Worker HWND exists; it makes the Reducer emit a LaunchApp for EVERY exec entry.
// Reloaded = config was reloaded; it emits
// LaunchApp only for the `exec` (once == false) entries. exec-once idempotency is
// thus stateless — purely which Event fires, never a remembered flag.
struct Started {};
struct Reloaded {};

// The `reload` hotkey trigger (ADR-0012). Posted by the Hotkey thread
// when a bound `reload` dispatcher fires. The Reducer is pure and cannot read a
// file, so it only ASKS: reduce(state, Reload{}) emits a single ReloadConfig{}
// Effect and touches no State. Distinct from Reloaded{} (above), the POST-parse
// Event the Worker posts to itself once the new file has parsed cleanly.
struct Reload {};

using Event = std::variant<WorkspaceSwitch, Quit, FocusMove, FocusResolve, MoveToWorkspace,
                           Appeared, Vanished, DistributeResolve, Started, Reloaded, Reload, Tile,
                           TileResolve>;

// Effects — what the Reducer asks the outside world to do. Executed by the
// Worker thread against the COM bridge; the Reducer itself performs no I/O. One
// Effect writes window geometry — the bounded exception to ADR-0007's ban:
// PositionWindow, the unified Slot writer (ADR-0020 folded the old SpreadWindow
// into it). It carries an optional target Display and is emitted by a Slot-bearing
// Place rule, by Distribute (auto-placement onto the least-occupied Display), and
// by `tile`; every other Effect leaves geometry untouched (`focus` reads rects,
// never moves a window).
struct SwitchToWorkspace {
    int logical = 0;  // Logical workspace number — resolved to a GUID in the bridge
};
struct Exit {};

// ResolveFocus asks the Worker to run the Probe sweep and post FocusResolve back;
// SetForegroundWindow asks it to bring the resolved target to the foreground.
struct ResolveFocus {
    reducer::Direction dir{};
};
struct SetForegroundWindow {
    WindowId id{};
};

// ResolveTile asks the Worker to run the tile Probe sweep (EnumWindows, gathering
// attrs AND identity per window) and post TileResolve back — the tile counterpart
// of ResolveFocus (ADR-0016). Carries nothing: the sweep needs no argument.
struct ResolveTile {};

// Move the foreground window to a Logical workspace (resolved to a GUID in the
// bridge, ADR-0010; the target is materialized on demand WITHOUT switching). The
// Worker wraps the DWM cloak/uncloak around the bridge call. Assigns a Workspace
// only — never geometry (ADR-0007). The plain (follow) form additionally emits a
// SwitchToWorkspace; the silent form does not.
struct MoveForegroundWindowToWorkspace {
    int logical = 0;
};

// Move a SPECIFIC window (usually a background one that just Appeared) to a
// Logical workspace — the WindowRule move. Distinct from
// MoveForegroundWindowToWorkspace, whose Worker arm resolves GetForegroundWindow()
// inline; a rule names its target by id. Both drive the same id-addressable bridge
// method moveWindowToWorkspace(WindowId, int); there is no cloak wrapping (the
// internal MoveViewToDesktop never paints on the current desktop; ADR-0010 revised).
struct MoveWindowToWorkspace {
    WindowId id{};
    int logical = 0;
};

// Phase one of the Distribute round-trip (ADR-0020, was ResolveSpread under
// ADR-0021), mirroring ResolveFocus: ask the Worker to probe Display occupancy and
// post DistributeResolve back. Emitted when an Eligible Appeared matches NO rule;
// the Reducer decides no geometry yet (the pure function cannot enumerate
// Displays), it only asks. `subject` is the window to place, carried through the
// round-trip so the eventual PositionWindow names it.
struct ResolveDistribute {
    WindowId subject{};
};

// Position a SPECIFIC window into a symbolic Slot (ADR-0016), optionally on a target
// Display (ADR-0020) — the sole geometry-writing Effect, a bounded reopening of
// ADR-0007's geometry ban. The Reducer only ever names the Slot and a target; it
// never computes or sees a rect. `target == nullopt` means the window's CURRENT
// monitor (a Slot rule placing on its own Display, the ADR-0016 behavior, and the
// Distribute tie-break "keep it put"); a set target is the Display Distribute chose.
// The adapter (positionWindow) resolves the destination monitor's work area, applies
// the pure rectForSlot, compensates the visible frame, and writes with SetWindowPos
// (or OS SW_MAXIMIZE for the Maximized Slot, moving onto the target Display first).
// Emitted on three paths — a Slot-bearing Place rule on Appeared (target nullopt,
// before the MoveWindowToWorkspace), Distribute's DistributeResolve (target = the
// chosen Display, Slot Maximized), and the `tile` sweep — never continuously.
struct PositionWindow {
    WindowId id{};
    std::optional<MonitorId> target;
    Slot slot{};
};

// Launch a detached child process (ADR-0011). The Worker runs `command`
// through one CreateProcessW and closes both handles — launch-only, no PID kept,
// no Workspace assigned (placement is a paired `windowrule`). `command` is the
// verbatim config tail.
struct LaunchApp {
    std::string command;
};

// Re-read and re-apply the config file live (ADR-0012). Emitted by the
// Reducer in response to a Reload{} Event; executed by the Worker on its own
// thread — it re-resolves the config path, re-reads and re-parses the file, and
// (only on a clean parse) reseeds config-derived State, hands the new Binds to the
// Hotkey thread, and posts Reloaded{} to itself. Carries no data: the Reducer
// reads no file, so everything the reload needs is discovered by the Worker.
struct ReloadConfig {};

// Make the OS logon task match `enabled` (ADR-0013). DECLARATIVE, not a
// transition command: the Reducer always emits SyncAutostart{state.startAtLogin}
// and never decides register-vs-remove — the Worker's adapter does create-or-update
// when enabled, delete-if-exists when not (counting an absent task as success). One
// SyncAutostart falls out of Started (initial start) and Reloaded (so toggling
// start_at_login + reload registers/removes the task live), mirroring LaunchApp.
struct SyncAutostart {
    bool enabled = false;
};

using Effect = std::variant<SwitchToWorkspace, Exit, ResolveFocus, SetForegroundWindow,
                            MoveForegroundWindowToWorkspace, MoveWindowToWorkspace, ResolveDistribute,
                            PositionWindow, ResolveTile, LaunchApp, ReloadConfig,
                            SyncAutostart>;

constexpr bool operator==(const WorkspaceSwitch& a, const WorkspaceSwitch& b) {
    return a.n == b.n;
}
constexpr bool operator==(const Quit&, const Quit&) { return true; }
constexpr bool operator==(const SwitchToWorkspace& a, const SwitchToWorkspace& b) {
    return a.logical == b.logical;
}
constexpr bool operator==(const Exit&, const Exit&) { return true; }
constexpr bool operator==(const ResolveFocus& a, const ResolveFocus& b) {
    return a.dir == b.dir;
}
constexpr bool operator==(const ResolveTile&, const ResolveTile&) { return true; }
constexpr bool operator==(const SetForegroundWindow& a, const SetForegroundWindow& b) {
    return a.id == b.id;
}
constexpr bool operator==(const MoveToWorkspace& a, const MoveToWorkspace& b) {
    return a.logical == b.logical && a.follow == b.follow;
}
constexpr bool operator==(const MoveForegroundWindowToWorkspace& a,
                          const MoveForegroundWindowToWorkspace& b) {
    return a.logical == b.logical;
}
constexpr bool operator==(const MoveWindowToWorkspace& a, const MoveWindowToWorkspace& b) {
    return a.id == b.id && a.logical == b.logical;
}
constexpr bool operator==(const ResolveDistribute& a, const ResolveDistribute& b) {
    return a.subject == b.subject;
}
constexpr bool operator==(const DisplayOccupancy& a, const DisplayOccupancy& b) {
    return a.id == b.id && a.count == b.count;
}
constexpr bool operator==(const PositionWindow& a, const PositionWindow& b) {
    return a.id == b.id && a.target == b.target && a.slot == b.slot;
}
inline bool operator==(const LaunchApp& a, const LaunchApp& b) {
    return a.command == b.command;
}
constexpr bool operator==(const ReloadConfig&, const ReloadConfig&) { return true; }
constexpr bool operator==(const SyncAutostart& a, const SyncAutostart& b) {
    return a.enabled == b.enabled;
}

// Value equality for the plain-data window types (used by the eligibility tests
// and the directional-focus tests).
constexpr bool operator==(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
constexpr bool operator==(const WindowAttrs& a, const WindowAttrs& b) {
    return a.id == b.id && a.monitor == b.monitor && a.rect == b.rect &&
           a.topLevel == b.topLevel && a.visible == b.visible && a.thickFrame == b.thickFrame &&
           a.caption == b.caption && a.toolWindow == b.toolWindow && a.cloaked == b.cloaked &&
           a.fullscreen == b.fullscreen;
}

// All the Reducer owns. Tiny by design: pass-by-value and return-by-value keep
// reduce pure and trivially testable. No window/geometry state lives here —
// Spatial focus is stateless, resolving from rects probed live at
// keypress, so nothing about windows is persisted between events.
struct State {
    int currentWorkspace = 1;
    bool running = true;

    // Window-rule state — the bounded, deliberate reintroduction of window state
    // ADR-0009 records against ADR-0007's otherwise-stateless window side. `rules`
    // is an immutable shared handle (seeded once at Worker construction), so the
    // per-event State copy stays O(1) instead of deep-copying N compiled regexes
    // (a deviation from ADR-0009's literal vector). `placed` is the place-once
    // set: an id enters on its first Eligible Appeared (matched or not) and is
    // erased on Vanished, so a user-moved window is never yanked back.
    std::shared_ptr<const std::vector<WindowRule>> rules;
    std::unordered_set<WindowId> placed;

    // The Ignore-set (ADR-0009 extension, widened by ADR-0020): the bounded set of
    // ids a matching Ignore WindowRule has excluded. Mirrors `placed` — an id enters
    // on its first Eligible Appeared whose first match is an Ignore rule (and enters
    // `placed` too, since it has been evaluated), and is erased on Vanished.
    // `resolveFocus` drops any Candidate in this set, so an Ignored window is never a
    // focus target; and because the Appeared handler emits no geometry for an Ignore
    // match, it is never auto-placed either — Ignore now means "don't touch at all"
    // (still Eligible, never moved or sized, still Alt-Tab reachable).
    std::unordered_set<WindowId> ignored;

    // Launch entries, seeded once at Worker construction behind the same
    // O(1)-copy shared handle as `rules`. Read only by the Started/Reloaded
    // handlers, which turn each into a LaunchApp; never mutated by a reduce. No
    // per-window launch state exists — exec-once idempotency is stateless.
    std::shared_ptr<const std::vector<ExecEntry>> execs;

    // The parsed `start_at_login` flag, seeded at Worker construction
    // beside `rules`/`execs` and reseeded on reload. State only carries it — no Effect derives from it here;
    // the logon task reads it and
    // emits its own Effect. Every reduce preserves it unchanged.
    bool startAtLogin = false;
};

struct ReduceResult {
    State state;
    std::vector<Effect> effects;
};

// ── the overloaded visitor ──────────────────────────────────────────────────

namespace detail {

// The classic overload set: one lambda per alternative, combined so std::visit
// dispatches to the matching handler. Adding an Event alternative without a
// matching overload is a compile error — the visitor is exhaustive by
// construction.
template <class... Fs>
struct overload : Fs... {
    using Fs::operator()...;
};
template <class... Fs>
overload(Fs...) -> overload<Fs...>;

}  // namespace detail

// ── eligibility: the pure window gate (substrate for focus + rules) ──────────

// The Eligibility gate: a window is Eligible iff every probed fact agrees. A
// real top-level, visible, resizable, captioned application window that is
// neither a tool window, cloaked, nor fullscreen. The Spatial-focus sweep
// filters candidates through this; the window rules match against the
// same set. (`thickFrame` is a tiling-era hold-over and may be loosened for
// focus candidacy later — see ADR-0007.)
constexpr bool isEligible(const WindowAttrs& a) {
    return a.topLevel && a.visible && a.thickFrame && a.caption && !a.toolWindow && !a.cloaked &&
           !a.fullscreen;
}

// ── directional focus resolution: the pure rule (ADR-0008) ───────────────────

namespace focus_detail {

// A window's center, kept as the DOUBLED sum of opposite edges so it stays exact
// integer arithmetic (no /2 rounding) and overflow-safe as int64. Distances and
// forward tests are all comparisons, so the ×2 scale cancels out.
struct Center {
    long long x = 0;
    long long y = 0;
};

constexpr Center center(const Rect& r) {
    return {static_cast<long long>(r.left) + r.right, static_cast<long long>(r.top) + r.bottom};
}

// Is the Candidate's center STRICTLY ahead of the Origin's on the Direction's
// axis? Win32 y grows down, so `down` is +y and `up` is −y.
constexpr bool ahead(reducer::Direction dir, const Center& c, const Center& o) {
    switch (dir) {
        case reducer::Direction::Left: return c.x < o.x;
        case reducer::Direction::Right: return c.x > o.x;
        case reducer::Direction::Up: return c.y < o.y;
        case reducer::Direction::Down: return c.y > o.y;
    }
    return false;
}

// In-band iff the Candidate's cross-axis span overlaps the Origin's — a window in
// the same row/column beats a diagonal one. Left/Right travel is horizontal, so
// the band is the vertical (top..bottom) overlap; Up/Down travel is vertical, so
// the band is the horizontal (left..right) overlap.
constexpr bool inBand(reducer::Direction dir, const Rect& c, const Rect& o) {
    const bool horizontal =
        dir == reducer::Direction::Left || dir == reducer::Direction::Right;
    if (horizontal) return c.top < o.bottom && o.top < c.bottom;
    return c.left < o.right && o.left < c.right;
}

// Squared center-to-center distance (on the doubled centers — a monotonic proxy
// for the true distance, so it orders Candidates identically without a sqrt).
constexpr long long distSq(const Center& a, const Center& b) {
    const long long dx = a.x - b.x;
    const long long dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace focus_detail

// The whole directional-focus brain, pure and stateless (ADR-0008): filter
// Candidates by isEligible, exclude the Origin by WindowId, keep only those whose
// center is strictly ahead in the Direction, then pick the argmin of the
// lexicographic tuple (inBand ? 0 : 1, euclideanCenterDistance, windowId) —
// same-band before diagonal, nearest by center distance, lowest id to break ties.
// Nothing ahead, or no Origin, → no target (nullopt). Monitor-crossing needs no
// special case: the rects are already in one virtual-screen coordinate space.
inline std::optional<WindowId> resolveFocus(reducer::Direction dir,
                                            const std::vector<WindowAttrs>& candidates,
                                            const std::optional<WindowAttrs>& origin,
                                            const std::unordered_set<WindowId>& ignored) {
    using namespace focus_detail;
    if (!origin) return std::nullopt;
    const Center oc = center(origin->rect);

    struct Score {
        int band;         // 0 in-band, 1 diagonal
        long long dist;   // squared center distance
        WindowId id;      // deterministic final tie-break
    };
    std::optional<Score> best;
    std::optional<WindowId> chosen;

    for (const WindowAttrs& c : candidates) {
        if (!isEligible(c)) continue;
        if (c.id == origin->id) continue;   // the Origin is never its own target
        if (ignored.contains(c.id)) continue;  // an Ignore-rule window is no focus target
        const Center cc = center(c.rect);
        if (!ahead(dir, cc, oc)) continue;
        const Score s{inBand(dir, c.rect, origin->rect) ? 0 : 1, distSq(cc, oc), c.id};
        if (!best || std::tie(s.band, s.dist, s.id) <
                         std::tie(best->band, best->dist, best->id)) {
            best = s;
            chosen = c.id;
        }
    }
    return chosen;
}

// ── window-rule matcher: the pure rule ───────────────────────────────────────

namespace rule_detail {

// ASCII per-byte case-insensitive equality — the same fold as config's iequals,
// re-expressed here so the core stays self-contained. exe/class match exactly
// under this fold; no Unicode case-folding (out of scope).
inline bool asciiIEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

inline bool matchesField(const WindowRule& r, const WindowIdentity& id) {
    switch (r.field) {
        case Field::Exe: return asciiIEquals(r.pattern, id.exe);
        case Field::Class: return asciiIEquals(r.pattern, id.windowClass);
        case Field::Title: return std::regex_search(id.title, r.regex);
    }
    return false;
}

}  // namespace rule_detail

// The matcher: three sequential passes over the rule list — all `exe` rules
// (config order), then all `class`, then all `title` — first match wins, and the
// winning rule is returned (its `action` decides Place vs Ignore, its `workspace`
// the Place target). The fixed field precedence exe→class→title makes overlapping
// rules resolve the same way every time; config order breaks within-field ties. A
// window matching both an Ignore and a Place rule thus resolves by that same
// first-match order. nullptr means nothing matched. Pure: a function of
// (rules, identity) only.
inline const WindowRule* matchRule(const std::vector<WindowRule>& rules,
                                   const WindowIdentity& id) {
    for (const Field pass : {Field::Exe, Field::Class, Field::Title})
        for (const WindowRule& r : rules)
            if (r.field == pass && rule_detail::matchesField(r, id)) return &r;
    return nullptr;
}

// ── slot geometry: the pure rect arithmetic (ADR-0016) ───────────────────────

// Compute the rect a Slot names within a Display work area — the fiddliest part
// of the feature, deliberately pure so it is unit-tested with no Win32 (the
// internal seam ADR-0016 calls out). `workArea` is the target monitor's work area
// (taskbar already excluded — the adapter reads it via GetMonitorInfo). Halves
// split at the integer midpoint and quarters meet on that same shared midpoint, so
// the four quarters tile the area with no gap and no overlapping interior; a stray
// odd pixel lands consistently on the right/bottom half. `maximized` returns the
// full work area (the adapter still prefers OS SW_MAXIMIZE for it — this rect is
// the fallback and the value the unit tests assert). No DPI or frame delta enters
// here; the adapter compensates the visible frame after calling this.
constexpr Rect rectForSlot(Slot slot, const Rect& workArea) {
    const int midX = workArea.left + (workArea.right - workArea.left) / 2;
    const int midY = workArea.top + (workArea.bottom - workArea.top) / 2;
    switch (slot) {
        case Slot::LeftHalf:
            return {workArea.left, workArea.top, midX, workArea.bottom};
        case Slot::RightHalf:
            return {midX, workArea.top, workArea.right, workArea.bottom};
        case Slot::TopHalf:
            return {workArea.left, workArea.top, workArea.right, midY};
        case Slot::BottomHalf:
            return {workArea.left, midY, workArea.right, workArea.bottom};
        case Slot::TopLeftQuarter:
            return {workArea.left, workArea.top, midX, midY};
        case Slot::TopRightQuarter:
            return {midX, workArea.top, workArea.right, midY};
        case Slot::BottomLeftQuarter:
            return {workArea.left, midY, midX, workArea.bottom};
        case Slot::BottomRightQuarter:
            return {midX, midY, workArea.right, workArea.bottom};
        case Slot::Maximized:
            return workArea;
    }
    return workArea;  // unreachable for a valid Slot
}

// ── distribute target: the pure balancing decision (ADR-0020) ────────────────

// Pick the least-occupied Display to Distribute a window onto — the deep-module
// core of Distribute, pure and unit-tested like rectForSlot with no Win32.
// `counts` is every live Display with its Eligible-window count (the subject
// already excluded upstream); `current` is the Display the subject sits on now.
//
// "Least-occupied" = the minimum count. The tie-break prefers keeping the window
// PUT: if `current` is among the displays and ties for the minimum, return nullopt
// — the caller emits PositionWindow{target=nullopt} (current monitor), avoiding a
// pointless cross-monitor jump (story 13). Otherwise return the FIRST minimum-count
// Display in enumeration order — deterministic when displays tie and the subject is
// not on any of them (story 14). An empty `counts` (no Displays enumerated) yields
// nullopt (degrade to the current monitor). Always returns a placement decision;
// there is no overflow no-op (story 15) — with every Display carrying windows it
// still names the least-occupied.
inline std::optional<MonitorId> pickDistributeTarget(const std::vector<DisplayOccupancy>& counts,
                                                     MonitorId current) {
    if (counts.empty()) return std::nullopt;
    const auto min = std::ranges::min_element(
        counts, {}, [](const DisplayOccupancy& d) { return d.count; });
    // The subject's own Display ties for least-occupied → keep it put (nullopt).
    if (const auto cur = std::ranges::find(counts, current, &DisplayOccupancy::id);
        cur != counts.end() && cur->count == min->count)
        return std::nullopt;
    return min->id;
}

// ── the seam ────────────────────────────────────────────────────────────────

inline ReduceResult reduce(const State& s, const Event& e) {
    return std::visit(
        detail::overload{
            [&](const WorkspaceSwitch& ws) -> ReduceResult {
                State next = s;
                next.currentWorkspace = ws.n;
                return {next, {Effect{SwitchToWorkspace{ws.n}}}};
            },
            [&](const Quit&) -> ReduceResult {
                State next = s;
                next.running = false;
                return {next, {Effect{Exit{}}}};
            },
            // Phase one: ask the Worker to run the Probe sweep. State is untouched —
            // spatial focus persists nothing (ADR-0007).
            [&](const FocusMove& fm) -> ReduceResult {
                return {s, {Effect{ResolveFocus{fm.dir}}}};
            },
            // Phase two: the probed rects have re-entered as an Event. Run the pure
            // resolution; emit a single SetForegroundWindow, or nothing.
            [&](const FocusResolve& fr) -> ReduceResult {
                if (const auto target = resolveFocus(fr.dir, fr.candidates, fr.origin, s.ignored))
                    return {s, {Effect{SetForegroundWindow{*target}}}};
                return {s, {}};
            },
            // Always move the foreground window to N; the plain (follow) form ALSO
            // switches the active desktop to N, the silent form does not. The move
            // Effect is emitted FIRST so the Worker reads the still-current active
            // desktop for its cloak decision before any follow-on switch lands.
            [&](const MoveToWorkspace& m) -> ReduceResult {
                std::vector<Effect> effects{Effect{MoveForegroundWindowToWorkspace{m.logical}}};
                if (!m.follow) return {s, std::move(effects)};
                State next = s;
                next.currentWorkspace = m.logical;
                effects.push_back(Effect{SwitchToWorkspace{m.logical}});
                return {next, std::move(effects)};
            },
            // A window appeared (SHOW / UNCLOAKED). Place-once (ADR-0009): if the id
            // is already `placed`, do nothing. If the window is not Eligible, emit
            // AND insert nothing — a later Eligible edge re-evaluates it (the UWP
            // cloaked-SHOW→Eligible-UNCLOAKED case). If Eligible, run the matcher,
            // emit the move on a match, and insert the id REGARDLESS of match — so an
            // Eligible unmatched window is never re-matched on later uncloaks.
            [&](const Appeared& a) -> ReduceResult {
                if (s.placed.contains(a.attrs.id)) return {s, {}};
                if (!isEligible(a.attrs)) return {s, {}};
                State next = s;
                next.placed.insert(a.attrs.id);
                std::vector<Effect> effects;
                // The window has been evaluated, so it is now `placed` regardless of
                // match. A rule match is explicit user intent that opts the window OUT
                // of Distribute (ADR-0020): an Ignore rule records the id in `ignored`
                // and does NOTHING else — Ignore now widens to "don't touch at all",
                // covering both focus and geometry; a Place rule emits the Slot (if
                // any) then the Workspace move. NO rule match → Distribute: emit
                // ResolveDistribute to start the least-occupied-Display probe
                // round-trip — no geometry yet, the pure Reducer cannot enumerate
                // Displays.
                const WindowRule* rule = s.rules ? matchRule(*s.rules, a.identity) : nullptr;
                if (!rule) {
                    effects.push_back(Effect{ResolveDistribute{a.attrs.id}});
                } else {
                    switch (rule->action) {
                        case RuleAction::Ignore:
                            next.ignored.insert(a.attrs.id);
                            break;
                        case RuleAction::Place:
                            // A Slot-bearing Place rule (ADR-0016) positions the window
                            // FIRST — while it is still visible on the current Desktop,
                            // on its OWN monitor (target nullopt) — then the workspace
                            // move cloaks-and-moves it. Geometry is per-HWND and
                            // Virtual-Desktop-independent, so it survives the move; this
                            // ordering is the lower-risk choice and a named acceptance
                            // check.
                            if (rule->slot)
                                effects.push_back(Effect{
                                    PositionWindow{a.attrs.id, std::nullopt, *rule->slot}});
                            effects.push_back(
                                Effect{MoveWindowToWorkspace{a.attrs.id, rule->workspace}});
                            break;
                    }
                }
                return {next, std::move(effects)};
            },
            // A window vanished (DESTROY / HIDE / CLOAKED). Erase the id: this bounds
            // `placed` to live windows and, because Windows recycles HWNDs, stops a
            // reused handle from being wrongly treated as already-placed. A fresh
            // Appeared for the same id afterward re-pins (a new lifetime).
            [&](const Vanished& v) -> ReduceResult {
                State next = s;
                next.placed.erase(v.id);
                next.ignored.erase(v.id);
                return {next, {}};
            },
            // Phase two of the Distribute round-trip (ADR-0020): the probed Display
            // counts have re-entered as an Event. Run pickDistributeTarget over them
            // (tie-break preferring the subject's own Display) and emit EXACTLY ONE
            // PositionWindow onto the chosen Display, always maximized — there is no
            // overflow no-op (story 15): even when every Display carries windows the
            // window still lands maximized on the least-occupied one (target nullopt
            // when it should stay put). State is untouched: place-once was already
            // recorded when the subject Appeared, so this phase persists nothing
            // (mirroring FocusResolve). The subject's own Display count excludes itself
            // upstream (exclude-self), so it never defeats its own placement.
            [&](const DistributeResolve& dr) -> ReduceResult {
                const std::optional<MonitorId> target =
                    pickDistributeTarget(dr.displays, dr.subjectMonitor);
                return {s, {Effect{PositionWindow{dr.subject, target, Slot::Maximized}}}};
            },
            // Startup: emit a LaunchApp for EVERY exec entry, in config
            // order (exec-once + exec alike), then one SyncAutostart carrying the
            // current start_at_login flag — the autostart task is synced
            // at start just as the launchers fire. State is untouched — exec-once
            // idempotency is stateless, falling out of which Event fires, and the
            // flag is declarative (the Worker's adapter, not the Reducer, decides
            // register-vs-remove).
            [&](const Started&) -> ReduceResult {
                std::vector<Effect> effects;
                if (s.execs)
                    for (const ExecEntry& e : *s.execs)
                        effects.push_back(Effect{LaunchApp{e.command}});
                effects.push_back(Effect{SyncAutostart{s.startAtLogin}});
                return {s, std::move(effects)};
            },
            // Reload: emit LaunchApp only for
            // the `exec` (once == false) entries, in config order — exec-once is
            // skipped so a reload never spawns a second copy of an already-open app —
            // then one SyncAutostart with the freshly-reseeded flag, so
            // toggling start_at_login and reloading registers/removes the task live.
            [&](const Reloaded&) -> ReduceResult {
                std::vector<Effect> effects;
                if (s.execs)
                    for (const ExecEntry& e : *s.execs)
                        if (!e.once) effects.push_back(Effect{LaunchApp{e.command}});
                effects.push_back(Effect{SyncAutostart{s.startAtLogin}});
                return {s, std::move(effects)};
            },
            // The `reload` hotkey fired (ADR-0012). The Reducer is pure —
            // it cannot read the file — so it only ASKS: emit one ReloadConfig{}
            // Effect and touch no State. The Worker executes the re-read/re-parse and
            // (on a clean parse) reseeds State, re-registers hotkeys, and posts
            // Reloaded{} to itself. Not to be confused with Reloaded{} above.
            [&](const Reload&) -> ReduceResult {
                return {s, {Effect{ReloadConfig{}}}};
            },
            // Phase one of the tile round-trip (ADR-0016), mirroring FocusMove: ask
            // the Worker to run the Probe sweep. State is untouched — tile is an
            // explicit, stateless re-tile that persists nothing.
            [&](const Tile&) -> ReduceResult {
                return {s, {Effect{ResolveTile{}}}};
            },
            // Phase two of the tile round-trip (ADR-0020): the probed windows AND the
            // Display list have re-entered as an Event. tile is the full-pipeline
            // rebalance button — it re-runs Distribute across every Eligible window on
            // the current Workspace, re-placeable (it consults no `placed` set) and the
            // only path that may move already-placed windows back into an even spread.
            // Explicit user intent still wins: a Slot-bearing Place rule places to its
            // Slot on its own monitor (target nullopt); an Ignore or workspace-only
            // Place window is counted as occupancy but never moved (tile never teleports
            // a window between Workspaces or resizes an Ignored one). Every OTHER
            // Eligible window is FREE — balanced across all Displays (including empty
            // ones) via pickDistributeTarget over a local, mutating count vector, each
            // landing maximized. Stateless: State is left unchanged. "Current Workspace
            // only" falls out for free — windows on other Virtual Desktops are
            // DWM-cloaked → Ineligible → skipped.
            [&](const TileResolve& tr) -> ReduceResult {
                std::vector<Effect> effects;
                // Baseline occupancy: every live Display at count 0. Fixed windows add
                // to it; free windows are then balanced against it.
                auto counts = tr.displays |
                              std::views::transform([](MonitorId d) {
                                  return DisplayOccupancy{d, 0};
                              }) |
                              std::ranges::to<std::vector>();
                const auto bump = [&](MonitorId m) {
                    if (const auto d = std::ranges::find(counts, m, &DisplayOccupancy::id);
                        d != counts.end())
                        ++d->count;
                };

                // Pass one: place Slot rules, count all matched (fixed) windows as
                // occupancy, and collect the free windows (id + current monitor kept
                // paired in the probed attrs) for balancing.
                std::vector<WindowAttrs> freeWindows;
                for (const ProbedWindow& w : tr.windows) {
                    if (!isEligible(w.attrs)) continue;
                    const WindowRule* rule = s.rules ? matchRule(*s.rules, w.identity) : nullptr;
                    if (rule) {
                        if (rule->action == RuleAction::Place && rule->slot)
                            effects.push_back(
                                Effect{PositionWindow{w.attrs.id, std::nullopt, *rule->slot}});
                        bump(w.attrs.monitor);  // Ignore / Place stay put but occupy
                    } else {
                        freeWindows.push_back(w.attrs);
                    }
                }

                // Pass two: balance each free window onto the least-occupied Display,
                // mutating the count vector so successive free windows spread out.
                for (const WindowAttrs& w : freeWindows) {
                    const std::optional<MonitorId> target = pickDistributeTarget(counts, w.monitor);
                    effects.push_back(Effect{PositionWindow{w.id, target, Slot::Maximized}});
                    bump(target.value_or(w.monitor));
                }
                return {s, std::move(effects)};
            },
        },
        e);
}

}  // namespace winspace

// ─────────────────────────────────────────────────────────────────────────────
// [core section] config
// ─────────────────────────────────────────────────────────────────────────────

// Config parser + semantic input types — core (pure, no <windows.h>).
//
// Seam 2: parse(text) -> (config, diagnostics). Owns the vocabulary the parser
// produces — Mod, Key, Dispatcher, Bind — none of them Win32 constants; the I/O
// adapter maps Mod->MOD_* and Key->VK_*. The test TU links no WM
// libraries, so any stray OS call reachable from here is a link error.
//
// The grammar is a strict subset of the eventual Hyprland DSL:
//   * `#` comments (whole-line or trailing)
//   * `$name = tokens` variable definitions, referenced as `$name`
//   * `bind = MODS, KEY, dispatcher, args`  (dispatcher in { workspace, quit,
//     focus, movetoworkspace, movetoworkspacesilent, reload })
//   * `windowrule = <action>, <field>:<pattern>`  (action in { workspace N, ignore, spread })
//   * `exec` / `exec-once = <verbatim command>`
//   * `start_at_login = <bool>`
// A name removed when tiling was dropped (ADR-0007) — a dispatcher such as
// movewindow, or a setting such as min_tile_width — earns a targeted "removed with
// tiling" Diagnostic rather than the generic unknown-name one.
//
// A malformed line yields a diagnostic and parsing continues. The parse() seam is
// pure and never reads a file; "keep last good config" retention and the live
// reload orchestration live in the I/O layer (io/config_io.cpp + the Worker).



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

// Every key a Bind can name; the I/O adapter maps each to a VK_*. Each
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

// Dispatchers recognized. An unknown name is diagnosed here, not
// deferred to registration.
enum class Dispatcher : uint8_t {
    Workspace,
    Quit,
    Focus,
    MoveToWorkspace,        // movetoworkspace N — move + follow (switch to N)
    MoveToWorkspaceSilent,  // movetoworkspacesilent N — move, stay on current
    Reload,                 // reload — re-read + re-apply the config file
    Tile,                   // tile — re-place every open window into its Slot (ADR-0016)
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
    // The flat `start_at_login` setting; false when absent or set to
    // `false`. Carried into State; the logon task consumes it.
    bool startAtLogin = false;
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
    if (t == "reload") return Dispatcher::Reload;
    if (t == "tile") return Dispatcher::Tile;
    return std::nullopt;
}

// Names removed when tiling was dropped (ADR-0007) — dispatchers a `bind` might
// still target and settings a ported Hyprland config might still carry. A match
// earns a TARGETED "removed with tiling" Diagnostic (so a porting user reads it as
// scoped, not mistyped); every other unknown name keeps the generic message, so a
// real typo is still caught. One combined list: the message is identical and no
// removed dispatcher name collides with a removed setting name.
inline bool is_removed_tiling_name(std::string_view name) {
    static constexpr std::string_view removed[] = {
        // dispatchers
        "movewindow", "maximize", "resizeactive", "togglefloat", "movetomonitor",
        // settings
        "min_tile_width", "min_tile_height"};
    return std::ranges::contains(removed, name);
}

inline std::string removed_tiling_message(std::string_view name) {
    return "'" + std::string(name) +
           "' was removed with tiling (winspace owns no window geometry)";
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

// The nine Slot vocabulary tokens (ADR-0016), matched case-insensitively like the
// direction words. This table is the SINGLE source of truth: parse_slot resolves
// the `slot <name>` suffix through it, and slot_vocabulary() renders it for the
// diagnostic message — so adding a Slot can never leave the parser and the error
// text out of sync.
inline constexpr std::pair<std::string_view, Slot> k_slot_names[] = {
    {"left-half", Slot::LeftHalf},
    {"right-half", Slot::RightHalf},
    {"top-half", Slot::TopHalf},
    {"bottom-half", Slot::BottomHalf},
    {"top-left-quarter", Slot::TopLeftQuarter},
    {"top-right-quarter", Slot::TopRightQuarter},
    {"bottom-left-quarter", Slot::BottomLeftQuarter},
    {"bottom-right-quarter", Slot::BottomRightQuarter},
    {"maximized", Slot::Maximized},
};

inline std::optional<Slot> parse_slot(std::string_view t) {
    const auto it = std::ranges::find_if(
        k_slot_names, [&](std::string_view name) { return iequals(t, name); },
        &std::pair<std::string_view, Slot>::first);
    if (it == std::ranges::end(k_slot_names)) return std::nullopt;
    return it->second;
}

// The vocabulary rendered as `a|b|c…` for a per-line Diagnostic (cold path — an
// unknown slot name only). Built from k_slot_names so it never drifts from what
// parse_slot actually accepts.
inline std::string slot_vocabulary() {
    std::string out;
    for (const auto& [name, slot] : k_slot_names) {
        if (!out.empty()) out += '|';
        out += name;
    }
    return out;
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
                    {line_no, is_removed_tiling_name(fields[2])
                                  ? removed_tiling_message(fields[2])
                                  : "unknown dispatcher '" + std::string(fields[2]) + "'"});
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

        // `windowrule = <action>, <field>:<pattern>` — where <action> is
        // `workspace N` (Place a matching app on a Workspace, and its Slot if given)
        // or `ignore` (leave the window entirely alone — no focus, no placement).
        // The old `spread` action is gone (ADR-0020): distribution is now automatic
        // for every unmatched eligible window, so a config still carrying `spread`
        // earns a targeted diagnostic rather than a rule. NO $var expansion: a title
        // regex may end in `$` (the end-anchor), which expand_vars would misread as a
        // reference.
        if (lhs == "windowrule") {
            // Split the RHS on the FIRST comma (action vs. match spec) so a title
            // regex may contain commas. rhs is already trimmed, so the spec's
            // trailing whitespace is gone; the field half is trimmed below.
            const size_t comma = rhs.find(',');
            if (comma == std::string_view::npos) {
                result.diagnostics.push_back(
                    {line_no, "windowrule needs '<action>, <field>:<pattern>'"});
                continue;
            }
            const std::string_view action = trim(rhs.substr(0, comma));
            const std::string_view spec = rhs.substr(comma + 1);

            // Action is `workspace N` (Place) or `ignore`; the retired `spread`
            // token earns a targeted diagnostic (distribution is automatic now,
            // ADR-0020); any other Hyprland form is unsupported (diagnosed, not
            // aborting the file).
            const auto atoks = split_ws(action);
            WindowRule rule;
            if (!atoks.empty() && atoks[0] == "workspace") {
                rule.action = RuleAction::Place;
                if (atoks.size() < 2) {
                    result.diagnostics.push_back(
                        {line_no, "windowrule workspace needs a number"});
                    continue;
                }
                int n = 0;
                const std::string_view narg = atoks[1];
                const auto [ptr, ec] =
                    std::from_chars(narg.data(), narg.data() + narg.size(), n);
                if (ec != std::errc{} || ptr != narg.data() + narg.size()) {
                    result.diagnostics.push_back(
                        {line_no, "windowrule workspace must be an integer: '" +
                                      std::string(narg) + "'"});
                    continue;
                }
                rule.workspace = n;

                // Optional `slot <name>` suffix (ADR-0016), living inside the
                // `workspace N` action clause. Absent → nullopt → exactly the
                // pre-ADR-0016 Workspace-pin behavior. A present-but-malformed slot
                // is a per-line Diagnostic that skips only this rule (per-line
                // degrade, consistent with the rest of the parser).
                if (atoks.size() >= 3) {
                    if (atoks[2] != "slot") {
                        result.diagnostics.push_back(
                            {line_no, "unexpected token '" + std::string(atoks[2]) +
                                          "' in windowrule workspace action (expected 'slot')"});
                        continue;
                    }
                    if (atoks.size() < 4) {
                        result.diagnostics.push_back(
                            {line_no, "windowrule slot needs a name (" + slot_vocabulary() + ")"});
                        continue;
                    }
                    const auto slot = parse_slot(atoks[3]);
                    if (!slot) {
                        result.diagnostics.push_back(
                            {line_no, "unknown slot '" + std::string(atoks[3]) +
                                          "' (expected " + slot_vocabulary() + ")"});
                        continue;
                    }
                    rule.slot = *slot;
                }
            } else if (!atoks.empty() && atoks[0] == "ignore") {
                rule.action = RuleAction::Ignore;  // no workspace number
                // `slot` is Place-only — a Slot on an Ignore rule is a meaningless
                // combination, flagged rather than silently dropped (ADR-0016).
                if (atoks.size() > 1 && atoks[1] == "slot") {
                    result.diagnostics.push_back(
                        {line_no,
                         "slot is not valid on an 'ignore' windowrule (slot places a "
                         "window; ignore only excludes it from focus)"});
                    continue;
                }
            } else if (!atoks.empty() && atoks[0] == "spread") {
                // Retired with ADR-0020: distribution is automatic for every
                // unmatched eligible window, so `spread` is no longer a rule action.
                // A targeted message so a ported config reads as scoped, not mistyped.
                result.diagnostics.push_back(
                    {line_no,
                     "'spread' was removed — distribution is now automatic for every "
                     "eligible window (drop this rule; unmatched windows are spread and "
                     "maximized on the least-occupied display)"});
                continue;
            } else {
                result.diagnostics.push_back(
                    {line_no, "unsupported windowrule action '" + std::string(action) +
                                  "' (only 'workspace N' or 'ignore')"});
                continue;
            }

            // Split the spec on the FIRST colon (field vs. verbatim pattern) so a
            // title regex may contain colons. Shared by both actions — an `ignore`
            // rule names its match field exactly like a `workspace` rule.
            const size_t colon = spec.find(':');
            if (colon == std::string_view::npos) {
                result.diagnostics.push_back(
                    {line_no, "windowrule match needs '<field>:<pattern>'"});
                continue;
            }
            const std::string_view fieldStr = trim(spec.substr(0, colon));
            const std::string_view pattern = spec.substr(colon + 1);  // verbatim tail

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

        // `exec = <cmd>` / `exec-once = <cmd>` — declare an app to launch (ADR-0011). The command is the VERBATIM tail after `=` (may hold spaces,
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

        // `start_at_login = <bool>` — the lone flat setting. true/false,
        // case-insensitive; anything else is a Diagnostic. Absent behaves as false.
        // No section{} grammar and no workspace rules (both geometry-only, ADR-0007).
        if (lhs == "start_at_login") {
            if (iequals(rhs, "true")) {
                result.config.startAtLogin = true;
            } else if (iequals(rhs, "false")) {
                result.config.startAtLogin = false;
            } else {
                result.diagnostics.push_back(
                    {line_no, "start_at_login must be true or false: '" +
                                  std::string(rhs) + "'"});
            }
            continue;
        }

        // A genuinely unknown directive — but if it names something removed with
        // tiling (a dropped setting like min_tile_width, or a dispatcher used as a
        // bare directive), give the targeted message so a ported config reads as
        // scoped, not mistyped.
        result.diagnostics.push_back(
            {line_no, is_removed_tiling_name(lhs)
                          ? removed_tiling_message(lhs)
                          : "unknown directive '" + std::string(lhs) + "'"});
    }

    return result;
}

}  // namespace winspace

// ─────────────────────────────────────────────────────────────────────────────
// [core section] command
// ─────────────────────────────────────────────────────────────────────────────

// Pure command-intent parsing (ADR-0019) — core (pure, no <windows.h>).
//
// winspace.exe has one no-arg mode (the WM) and two headless subcommands the
// Scoop package invokes around its file swap. wWinMain narrows the wide command
// line to UTF-8 argument tokens (the program name dropped) and hands them to this
// pure mapper BEFORE any window or thread exists, so command routing is a
// unit-testable decision with zero I/O — the same altitude as the Reducer, and
// the only NEW place pure logic is added by this feature.
//
// Deliberately small and total: no args -> Run (the WM, unchanged); exactly
// "install" or "uninstall" as the sole token -> that headless command; anything
// else -> Other (an unknown verb, or a known verb with stray arguments). The
// spine treats Other as a usage error rather than silently starting the WM, so a
// typo (`winspace instal`) never seizes the session hooks.

namespace winspace {

enum class Command { Run, Install, Uninstall, Other };

constexpr bool operator==(Command a, Command b) {
    return static_cast<int>(a) == static_cast<int>(b);
}

// Map already-narrowed argument tokens (argv without the program name) to a
// Command. Total and allocation-free; the I/O layer owns the wide->narrow
// narrowing, so the core stays wchar_t-free.
inline Command parseCommand(const std::vector<std::string>& args) {
    if (args.empty()) return Command::Run;
    if (args.size() == 1) {
        if (args[0] == "install") return Command::Install;
        if (args[0] == "uninstall") return Command::Uninstall;
    }
    return Command::Other;
}

}  // namespace winspace
