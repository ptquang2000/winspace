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

#include <variant>
#include <vector>

namespace winspace {

// ── the vocabulary: Event, Effect, State (aggregates over a variant) ────────

// Events — what the outside world asks the Reducer to do. Produced by the I/O
// adapters (a WM_HOTKEY becomes one of these) and fed in as plain data.
struct WorkspaceSwitch {
    int n = 0;  // target Logical workspace number
};
struct Quit {};

using Event = std::variant<WorkspaceSwitch, Quit>;

// Effects — what the Reducer asks the outside world to do. Executed by the
// Worker thread against the COM bridge; the Reducer itself performs no I/O.
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

// All the Reducer owns. Tiny by design: pass-by-value and return-by-value keep
// reduce pure and trivially testable.
struct State {
    int current_workspace = 1;
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
        },
        e);
}

}  // namespace winspace
