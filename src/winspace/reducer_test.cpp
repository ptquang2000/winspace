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
    const State s{.currentWorkspace = 1, .running = true};

    const auto r = reduce(s, WorkspaceSwitch{5});

    // The Effect carries the requested logical number...
    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{5});
    // ...and the same number is what currentWorkspace becomes.
    REQUIRE(r.state.currentWorkspace == 5);
    REQUIRE(r.state.running);
}

TEST_CASE("the switch target is the Event's number, not a delta from the current workspace", "[reducer]") {
    // Already on 4; switching to 2 lands on 2, not 4-2 or 4+2.
    const auto r = reduce(State{.currentWorkspace = 4, .running = true}, WorkspaceSwitch{2});

    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{2});
    REQUIRE(r.state.currentWorkspace == 2);
}

TEST_CASE("switching to the workspace already current still emits the Effect", "[reducer]") {
    // Idempotent State, but the Effect must still fire — the bridge decides
    // whether a same-desktop SwitchDesktop is a no-op, not the Reducer.
    const auto r = reduce(State{.currentWorkspace = 3, .running = true}, WorkspaceSwitch{3});

    REQUIRE(single_effect<SwitchToWorkspace>(r) == SwitchToWorkspace{3});
    REQUIRE(r.state.currentWorkspace == 3);
}

TEST_CASE("a Quit Event emits Exit and clears running", "[reducer]") {
    const auto r = reduce(State{.currentWorkspace = 2, .running = true}, Quit{});

    // Exit is the observable Effect; running=false is exactly what it reveals.
    REQUIRE(single_effect<Exit>(r) == Exit{});
    REQUIRE_FALSE(r.state.running);
    // Quit doesn't move the workspace.
    REQUIRE(r.state.currentWorkspace == 2);
}

TEST_CASE("reduce is pure — same inputs yield same Effects and leave the input State untouched", "[reducer]") {
    const State s{.currentWorkspace = 1, .running = true};

    const auto a = reduce(s, WorkspaceSwitch{7});
    const auto b = reduce(s, WorkspaceSwitch{7});

    // Deterministic: identical inputs, identical Effects.
    REQUIRE(a.effects == b.effects);
    REQUIRE(single_effect<SwitchToWorkspace>(a) == SwitchToWorkspace{7});

    // No mutation of the argument: s is still usable and unchanged.
    REQUIRE(s.currentWorkspace == 1);
    REQUIRE(s.running);
}

// ── window tracking: eligibility, focus order, layout ────────────────────────

namespace {

// A fully-Tileable Probe result: every eligibility fact points the right way.
// Individual tests flip exactly one field to prove that field alone decides.
WindowAttrs tileable(WindowId id, MonitorId monitor) {
    return WindowAttrs{.id = id,
                       .monitor = monitor,
                       .topLevel = true,
                       .visible = true,
                       .thickFrame = true,
                       .caption = true,
                       .toolWindow = false,
                       .cloaked = false,
                       .fullscreen = false};
}

// One monitor whose work area is the rect we expect a fill to land in.
constexpr MonitorId kMon{1};
constexpr Rect kWork{0, 0, 1920, 1040};

State with_monitor() {
    State s{};
    s.monitors[kMon] = kWork;
    return s;
}

}  // namespace

TEST_CASE("isTileable is the AND of the probed facts — each condition alone flips the verdict", "[reducer]") {
    const WindowAttrs ok = tileable(WindowId{1}, kMon);
    REQUIRE(isTileable(ok));

    // Each required-true fact, cleared, drops it out.
    auto no_top = ok; no_top.topLevel = false; REQUIRE_FALSE(isTileable(no_top));
    auto no_vis = ok; no_vis.visible = false; REQUIRE_FALSE(isTileable(no_vis));
    auto no_thick = ok; no_thick.thickFrame = false; REQUIRE_FALSE(isTileable(no_thick));
    auto no_cap = ok; no_cap.caption = false; REQUIRE_FALSE(isTileable(no_cap));

    // Each must-be-false fact, set, drops it out.
    auto tool = ok; tool.toolWindow = true; REQUIRE_FALSE(isTileable(tool));
    auto cloaked = ok; cloaked.cloaked = true; REQUIRE_FALSE(isTileable(cloaked));
    auto fs = ok; fs.fullscreen = true; REQUIRE_FALSE(isTileable(fs));
}

TEST_CASE("a single eligible Appeared fills the window to its monitor's rcWork", "[reducer]") {
    const auto r = reduce(with_monitor(), Appeared{tileable(WindowId{1}, kMon)});

    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});
}

TEST_CASE("an ineligible Appeared emits no Effect and does not track the window", "[reducer]") {
    auto attrs = tileable(WindowId{7}, kMon);
    attrs.toolWindow = true;  // a palette / tooltip — ignored

    const auto r = reduce(with_monitor(), Appeared{attrs});

    REQUIRE(r.effects.empty());
    REQUIRE(r.state.focusOrder.empty());
}

TEST_CASE("a second eligible Appeared fills over the first — head is the newest", "[reducer]") {
    auto r = reduce(with_monitor(), Appeared{tileable(WindowId{1}, kMon)});
    r = reduce(r.state, Appeared{tileable(WindowId{2}, kMon)});

    // The newest window owns the head, so it gets the fill.
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{2}, kWork});
}

TEST_CASE("Vanished of the head re-fills the survivor; the final Vanished emits nothing", "[reducer]") {
    auto r = reduce(with_monitor(), Appeared{tileable(WindowId{1}, kMon)});
    r = reduce(r.state, Appeared{tileable(WindowId{2}, kMon)});

    // Kill the head (2): the survivor (1) reclaims the fill.
    r = reduce(r.state, Vanished{WindowId{2}});
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});

    // Kill the last one: nothing left to place.
    r = reduce(r.state, Vanished{WindowId{1}});
    REQUIRE(r.effects.empty());
}

TEST_CASE("two Appeared for the same WindowId keep one entry and emit one fill", "[reducer]") {
    auto r = reduce(with_monitor(), Appeared{tileable(WindowId{1}, kMon)});
    r = reduce(r.state, Appeared{tileable(WindowId{1}, kMon)});

    // Idempotent: move-to-front, never duplicate.
    REQUIRE(r.state.focusOrder.size() == 1);
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});
}

TEST_CASE("Vanished of an untracked window is a no-op that still holds the current fill", "[reducer]") {
    auto r = reduce(with_monitor(), Appeared{tileable(WindowId{1}, kMon)});

    // A Vanished for a window we never tracked leaves the head untouched.
    r = reduce(r.state, Vanished{WindowId{99}});
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});
}

// ── monitor model: MonitorsChanged fold + rcWork lookup ──────────────────────

TEST_CASE("MonitorsChanged emits no Effect of its own", "[reducer]") {
    const auto r = reduce(State{}, MonitorsChanged{{{kMon, kWork}}});

    REQUIRE(r.effects.empty());
}

TEST_CASE("after a MonitorsChanged snapshot, an Appeared fills to that monitor's rcWork", "[reducer]") {
    // No monitors known yet — the snapshot is what makes the fill resolvable.
    auto r = reduce(State{}, MonitorsChanged{{{kMon, kWork}}});
    r = reduce(r.state, Appeared{tileable(WindowId{1}, kMon)});

    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});
}

TEST_CASE("an Appeared on a MonitorId absent from the topology emits no PositionWindow", "[reducer]") {
    // The snapshot knows kMon; the window claims a different, unknown monitor.
    auto r = reduce(State{}, MonitorsChanged{{{kMon, kWork}}});
    r = reduce(r.state, Appeared{tileable(WindowId{1}, MonitorId{99})});

    // Defensive: a window on an unknown monitor is left alone (no fill), but
    // still tracked so a later snapshot can place it.
    REQUIRE(r.effects.empty());
    REQUIRE(r.state.focusOrder.size() == 1);
}

TEST_CASE("MonitorsChanged replaces the topology wholesale — the later snapshot wins entirely", "[reducer]") {
    constexpr MonitorId kOther{2};
    constexpr Rect kOtherWork{0, 0, 2560, 1400};

    // First snapshot knows kMon; a window fills there.
    auto r = reduce(State{}, MonitorsChanged{{{kMon, kWork}}});
    r = reduce(r.state, Appeared{tileable(WindowId{1}, kMon)});
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{1}, kWork});

    // A second snapshot drops kMon entirely and introduces kOther. The head
    // still sits on the now-vanished kMon, so it no longer resolves.
    r = reduce(r.state, MonitorsChanged{{{kOther, kOtherWork}}});
    REQUIRE(r.effects.empty());

    // A window on the new monitor fills to the new work area.
    r = reduce(r.state, Appeared{tileable(WindowId{2}, kOther)});
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{2}, kOtherWork});
}
