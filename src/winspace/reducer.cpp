// The central seam: a pure Reducer — core (pure, no <windows.h>).
//
// Seam 1: reduce(state, event) -> (newState, effects). The Reducer is the only
// place behavioral logic lives; it consumes an Event, returns fresh State by
// value, and emits Effects as plain data. It reasons purely in Logical
// workspace numbers — it never sees a Virtual Desktop GUID (the logical->GUID
// map, adoption, and create-on-demand all live in the bridge, task 06).
//
// The test TU links no WM libraries, so any stray OS call reachable from here
// is a link error — the linker enforces purity, not discipline. Behavior is
// tested through the emitted Effects, never by inspecting State internals.
#pragma once

#include <cstdint>
#include <map>
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
// reasons in these and never applies the drop-shadow delta or DPI (the Worker
// does, at execute time).
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// The Probe result: one window's live attributes, read reactively when a hook
// fires and carried into the core on an Appeared Event. Just `id`, the monitor
// it sits on, and the eligibility booleans — the Eligibility gate (02.02) is
// the pure AND of these facts.
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

// One monitor's work area (the desktop minus taskbar), keyed by its identity.
// A whole-topology list of these arrives on MonitorsChanged.
struct MonitorInfo {
    MonitorId id{};
    Rect rcWork{};
};

// Events — what the outside world asks the Reducer to do. Produced by the I/O
// adapters (a WM_HOTKEY becomes one of these) and fed in as plain data.
struct WorkspaceSwitch {
    int n = 0;  // target Logical workspace number
};
struct Quit {};
// A window crossed the SHOW / UNCLOAKED edge; carries the Probe's facts.
struct Appeared {
    WindowAttrs attrs{};
};
// A window crossed the DESTROY / HIDE / CLOAKED edge; the identity is enough.
struct Vanished {
    WindowId id{};
};
// The monitor topology changed; carries the whole new set (a wholesale replace,
// never a delta — see 02.03).
struct MonitorsChanged {
    std::vector<MonitorInfo> monitors;
};

using Event = std::variant<WorkspaceSwitch, Quit, Appeared, Vanished, MonitorsChanged>;

// Effects — what the Reducer asks the outside world to do. Executed by the
// Worker thread against the COM bridge; the Reducer itself performs no I/O.
struct SwitchToWorkspace {
    int logical = 0;  // Logical workspace number — resolved to a GUID in the bridge
};
struct Exit {};
// Move a window to a logical rect. The Worker frame-compensates and applies it;
// the Reducer only names the target geometry.
struct PositionWindow {
    WindowId id{};
    Rect rect{};
};

using Effect = std::variant<SwitchToWorkspace, Exit, PositionWindow>;

constexpr bool operator==(const WorkspaceSwitch& a, const WorkspaceSwitch& b) {
    return a.n == b.n;
}
constexpr bool operator==(const Quit&, const Quit&) { return true; }
constexpr bool operator==(const SwitchToWorkspace& a, const SwitchToWorkspace& b) {
    return a.logical == b.logical;
}
constexpr bool operator==(const Exit&, const Exit&) { return true; }

// Value equality for the new plain-data types. Rect first — WindowAttrs and
// MonitorInfo compare through it.
constexpr bool operator==(const Rect& a, const Rect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
constexpr bool operator==(const WindowAttrs& a, const WindowAttrs& b) {
    return a.id == b.id && a.monitor == b.monitor && a.topLevel == b.topLevel &&
           a.visible == b.visible && a.thickFrame == b.thickFrame && a.caption == b.caption &&
           a.toolWindow == b.toolWindow && a.cloaked == b.cloaked && a.fullscreen == b.fullscreen;
}
constexpr bool operator==(const MonitorInfo& a, const MonitorInfo& b) {
    return a.id == b.id && a.rcWork == b.rcWork;
}
constexpr bool operator==(const Appeared& a, const Appeared& b) { return a.attrs == b.attrs; }
constexpr bool operator==(const Vanished& a, const Vanished& b) { return a.id == b.id; }
constexpr bool operator==(const MonitorsChanged& a, const MonitorsChanged& b) {
    return a.monitors == b.monitors;
}
constexpr bool operator==(const PositionWindow& a, const PositionWindow& b) {
    return a.id == b.id && a.rect == b.rect;
}

// All the Reducer owns. Tiny by design: pass-by-value and return-by-value keep
// reduce pure and trivially testable.
struct State {
    int current_workspace = 1;
    bool running = true;
    // The monitor topology the layout resolves rcWork against; replaced whole on
    // MonitorsChanged (02.03).
    std::map<MonitorId, Rect> monitors;
    // The Tileable windows as a priority-ordered list, front = most-recently
    // focused; the layout acts on front() (02.02). Still a value: copied whole
    // in and out of reduce.
    std::vector<WindowId> focus_order;
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

// ── the seam ────────────────────────────────────────────────────────────────

inline ReduceResult reduce(const State& s, const Event& e) {
    return std::visit(
        detail::overload{
            [&](const WorkspaceSwitch& ws) -> ReduceResult {
                State next = s;
                next.current_workspace = ws.n;
                return {next, {Effect{SwitchToWorkspace{ws.n}}}};
            },
            [&](const Quit&) -> ReduceResult {
                State next = s;
                next.running = false;
                return {next, {Effect{Exit{}}}};
            },
            // Window-tracking Events are part of the vocabulary now, but their
            // behavior (eligibility + Focus order + layout, monitor fold) lands
            // in 02.02 / 02.03. Until then these are inert: State passes through
            // unchanged and no Effect fires. They exist here so the visitor is
            // exhaustive — a real handler simply replaces the stub.
            [&](const Appeared&) -> ReduceResult { return {s, {}}; },
            [&](const Vanished&) -> ReduceResult { return {s, {}}; },
            [&](const MonitorsChanged&) -> ReduceResult { return {s, {}}; },
        },
        e);
}

}  // namespace winspace
