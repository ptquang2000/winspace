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
// (on a `focus` keypress, issue 05). Just `id`, the monitor it sits on, and the
// eligibility booleans — the Eligibility gate is the pure AND of these facts.
struct WindowAttrs {
    WindowId id{};
    MonitorId monitor{};
    bool topLevel = false;
    bool visible = false;
    bool thickFrame = false;
    bool caption = false;
    bool toolWindow = false;
    bool cloaked = false;
    bool fullscreen = false;
};

// Events — what the outside world asks the Reducer to do. Produced by the I/O
// adapters (a WM_HOTKEY becomes one of these) and fed in as plain data.
struct WorkspaceSwitch {
    int n = 0;  // target Logical workspace number
};
struct Quit {};

using Event = std::variant<WorkspaceSwitch, Quit>;

// Effects — what the Reducer asks the outside world to do. Executed by the
// Worker thread against the COM bridge; the Reducer itself performs no I/O. No
// Effect writes window geometry (ADR-0007).
struct SwitchToWorkspace {
    int logical = 0;  // Logical workspace number — resolved to a GUID in the bridge
};
struct Exit {};

using Effect = std::variant<SwitchToWorkspace, Exit>;

constexpr bool operator==(const WorkspaceSwitch& a, const WorkspaceSwitch& b) {
    return a.n == b.n;
}
constexpr bool operator==(const Quit&, const Quit&) { return true; }
constexpr bool operator==(const SwitchToWorkspace& a, const SwitchToWorkspace& b) {
    return a.logical == b.logical;
}
constexpr bool operator==(const Exit&, const Exit&) { return true; }

// Value equality for the plain-data window types (used by the eligibility tests
// and, later, issue 05's directional-focus tests).
constexpr bool operator==(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
constexpr bool operator==(const WindowAttrs& a, const WindowAttrs& b) {
    return a.id == b.id && a.monitor == b.monitor && a.topLevel == b.topLevel &&
           a.visible == b.visible && a.thickFrame == b.thickFrame && a.caption == b.caption &&
           a.toolWindow == b.toolWindow && a.cloaked == b.cloaked && a.fullscreen == b.fullscreen;
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
        },
        e);
}

}  // namespace winspace
