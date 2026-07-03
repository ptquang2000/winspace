// Tests for the pure Reducer (Seam 1). Included by the test TU only, so it
// links no WM import libraries — the Reducer under test is provably pure.
//
// These tests exercise external behavior, not implementation details: they feed
// Events and assert on the emitted Effects. State is checked only for what an
// Effect already reveals (the switch target, the cleared running flag), never
// on internal structure.
#pragma once

#include "catch2/catch_amalgamated.hpp"
#include "winspace/reducer.cpp"

using namespace winspace;

// Pull the single emitted Effect of the expected alternative, or fail loudly.
template <class T>
const T& single_effect(const ReduceResult& r) {
    REQUIRE(r.effects.size() == 1);
    REQUIRE(std::holds_alternative<T>(r.effects[0]));
    return std::get<T>(r.effects[0]);
}

TEST_CASE("a WorkspaceSwitch Event emits exactly SwitchToWorkspace with that number", "[reducer]") {
    const State s{.current_workspace = 1, .running = true};

    const auto r = reduce(s, WorkspaceSwitch{5});

    // The Effect carries the requested logical number...
    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{5});
    // ...and the same number is what current_workspace becomes.
    REQUIRE(r.state.current_workspace == 5);
    REQUIRE(r.state.running);
}

TEST_CASE("the switch target is the Event's number, not a delta from the current workspace", "[reducer]") {
    // Already on 4; switching to 2 lands on 2, not 4-2 or 4+2.
    const auto r = reduce(State{.current_workspace = 4, .running = true}, WorkspaceSwitch{2});

    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{2});
    REQUIRE(r.state.current_workspace == 2);
}

TEST_CASE("switching to the workspace already current still emits the Effect", "[reducer]") {
    // Idempotent State, but the Effect must still fire — the bridge decides
    // whether a same-desktop SwitchDesktop is a no-op, not the Reducer.
    const auto r = reduce(State{.current_workspace = 3, .running = true}, WorkspaceSwitch{3});

    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{3});
    REQUIRE(r.state.current_workspace == 3);
}

TEST_CASE("a Quit Event emits Exit and clears running", "[reducer]") {
    const auto r = reduce(State{.current_workspace = 2, .running = true}, Quit{});

    // Exit is the observable Effect; running=false is exactly what it reveals.
    REQUIRE(single_effect<Exit>(r) == Exit{});
    REQUIRE_FALSE(r.state.running);
    // Quit doesn't move the workspace.
    REQUIRE(r.state.current_workspace == 2);
}

TEST_CASE("reduce is pure — same inputs yield same Effects and leave the input State untouched", "[reducer]") {
    const State s{.current_workspace = 1, .running = true};

    const auto a = reduce(s, WorkspaceSwitch{7});
    const auto b = reduce(s, WorkspaceSwitch{7});

    // Deterministic: identical inputs, identical Effects.
    REQUIRE(a.effects == b.effects);
    REQUIRE(single_effect<SwitchToWorkspace>(a) == SwitchToWorkspace{7});

    // No mutation of the argument: s is still usable and unchanged.
    REQUIRE(s.current_workspace == 1);
    REQUIRE(s.running);
}
