// The central seam: a pure Reducer — core (pure, no <windows.h>).
//
// Seam 1: reduce(state, event) -> (newState, effects). The Reducer is the only
// place behavioral logic lives; it consumes an Event, returns fresh State by
// value, and emits Effects as plain data. It reasons purely in Logical
// workspace numbers — it never sees a Virtual Desktop GUID (the logical->GUID
// map, adoption, and create-on-demand all live in the bridge, task 06).
//
// winspace owns no window geometry (ADR-0007): no Effect ever moves or sizes a
// window. What remains here on the window side is the Eligibility substrate —
// strong identities and the pure `isEligible` gate — which the Spatial-focus
// slice (issue 05) builds on and PRD 06's rules reuse. The tiling machinery
// (layout, PositionWindow, focus order, the monitor model, and the
// Appeared/Vanished lifecycle stream) was removed with tiling.
//
// The test TU links no WM libraries, so any stray OS call reachable from here
// is a link error — the linker enforces purity, not discipline. Behavior is
// tested through the emitted Effects, never by inspecting State internals.
#pragma once

#include <cstdint>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

namespace winspace {

// ── the vocabulary: Event, Effect, State (aggregates over a variant) ────────

// ── window & monitor identities and plain-data facts ────────────────────────

// Strong, opaque identities minted by the I/O adapters (an HWND / HMONITOR
// stamped into an integer). The core never does arithmetic on them — it only
// stores, compares, and hands them back on Effects.
enum class WindowId : uint64_t {};
enum class MonitorId : uint64_t {};

// A logical rectangle in virtual-screen coordinates. Plain data; the Reducer
// reasons in these (issue 05 resolves directional focus over them). The
// drop-shadow delta and DPI never enter the core.
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// The Probe result: one window's live attributes, read reactively when needed
// (on a `focus` keypress, issue 05). `id`, the monitor it sits on, its live
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

// movetoworkspace / movetoworkspacesilent (issue 06): move the FOREGROUND window
// to a Logical workspace. `follow` = the plain form (the active desktop also
// becomes N); the silent form stays put. Ungated by Eligibility — the user aimed
// at the foreground window explicitly. Unlike focus (ADR-0008) there is no pure
// decision to make, so no probe round-trip: the Worker resolves the foreground
// window inline at execute time (degrade-and-log if there is none).
struct MoveToWorkspace {
    int logical = 0;    // target Logical workspace number
    bool follow = false;
};

using Event = std::variant<WorkspaceSwitch, Quit, FocusMove, FocusResolve, MoveToWorkspace>;

// Effects — what the Reducer asks the outside world to do. Executed by the
// Worker thread against the COM bridge; the Reducer itself performs no I/O. No
// Effect writes window geometry (ADR-0007) — `focus` reads rects, never moves a
// window.
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

// Move the foreground window to a Logical workspace (resolved to a GUID in the
// bridge, ADR-0010; the target is materialized on demand WITHOUT switching). The
// Worker wraps the DWM cloak/uncloak around the bridge call. Assigns a Workspace
// only — never geometry (ADR-0007). The plain (follow) form additionally emits a
// SwitchToWorkspace; the silent form does not.
struct MoveForegroundWindowToWorkspace {
    int logical = 0;
};

using Effect = std::variant<SwitchToWorkspace, Exit, ResolveFocus, SetForegroundWindow,
                            MoveForegroundWindowToWorkspace>;

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

// Value equality for the plain-data window types (used by the eligibility tests
// and, later, issue 05's directional-focus tests).
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
// Spatial focus (issue 05) is stateless, resolving from rects probed live at
// keypress, so nothing about windows is persisted between events.
struct State {
    int currentWorkspace = 1;
    bool running = true;
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
// (issue 05) filters candidates through this; PRD 06's rules match against the
// same set. (`thickFrame` is a tiling-era hold-over and may be loosened for
// focus candidacy later — see ADR-0007 / DESIGN §4.)
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
                                            const std::optional<WindowAttrs>& origin) {
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
        if (c.id == origin->id) continue;  // the Origin is never its own target
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
                if (const auto target = resolveFocus(fr.dir, fr.candidates, fr.origin))
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
        },
        e);
}

}  // namespace winspace
