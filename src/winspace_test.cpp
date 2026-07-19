// winspace tests — the test translation-unit source (winspace_tests.exe).
//
// Compiles the pure core (winspace.cpp) plus vendored Catch2 into one TU, built
// /SUBSYSTEM:CONSOLE, and links NONE of the WM import libraries. That last fact
// is the point: any accidental OS call reachable from core becomes a *link*
// error here, not a runtime surprise in the app. The linker enforces "core
// touches no Windows API" — discipline is not required.
//
// Catch2 is amalgamated and #included (header + implementation) so this stays a
// single translation unit. The push(0)/pop wrapper silences third-party warnings
// so our /W4 /WX applies only to winspace code. The reducer (Seam 1) and config
// (Seam 2) tests follow, each a banner-delimited section.
#pragma warning(push, 0)
#include "catch2/catch_amalgamated.hpp"
#include "catch2/catch_amalgamated.cpp"  // brings the library + default main()
#pragma warning(pop)

#include "winspace.cpp"  // the pure core under test

using namespace winspace;

TEST_CASE("build harness compiles, links, and runs a pure test", "[harness]") {
    REQUIRE(1 + 1 == 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// [reducer tests] Seam 1
// ─────────────────────────────────────────────────────────────────────────────

// Tests for the pure Reducer (Seam 1). Included by the test TU only, so it
// links no WM import libraries — the Reducer under test is provably pure.
//
// These tests exercise external behavior, not implementation details: they feed
// Events and assert on the emitted Effects. State is checked only for what an
// Effect already reveals (the switch target, the cleared running flag), never
// on internal structure.



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

// ── move-to-workspace: the move Effect, and follow vs. silent ────────────────

TEST_CASE("movetoworkspace (follow) emits the move then the switch", "[reducer]") {
    const auto r = reduce(State{.currentWorkspace = 1, .running = true},
                          MoveToWorkspace{3, true});

    // Two Effects, in order: move the foreground window, THEN follow it to N.
    REQUIRE(r.effects.size() == 2);
    REQUIRE(std::holds_alternative<MoveForegroundWindowToWorkspace>(r.effects[0]));
    REQUIRE(std::get<MoveForegroundWindowToWorkspace>(r.effects[0]) ==
            MoveForegroundWindowToWorkspace{3});
    REQUIRE(std::holds_alternative<SwitchToWorkspace>(r.effects[1]));
    REQUIRE(std::get<SwitchToWorkspace>(r.effects[1]) == SwitchToWorkspace{3});
    // Following advances the active workspace, exactly as a bare WorkspaceSwitch would.
    REQUIRE(r.state.currentWorkspace == 3);
}

TEST_CASE("movetoworkspacesilent emits only the move and stays on the current workspace", "[reducer]") {
    const auto r = reduce(State{.currentWorkspace = 2, .running = true},
                          MoveToWorkspace{5, false});

    // Exactly one Effect — the move; no switch.
    REQUIRE(single_effect<MoveForegroundWindowToWorkspace>(r) ==
            MoveForegroundWindowToWorkspace{5});
    // Silent leaves the active workspace where it was.
    REQUIRE(r.state.currentWorkspace == 2);
}

TEST_CASE("a silent move to the workspace already current still emits the move", "[reducer]") {
    // Same-desktop is not the Reducer's call — it emits the move unconditionally and
    // lets the Worker decide the cloak is unnecessary (target == current).
    const auto r = reduce(State{.currentWorkspace = 3, .running = true},
                          MoveToWorkspace{3, false});

    REQUIRE(single_effect<MoveForegroundWindowToWorkspace>(r) ==
            MoveForegroundWindowToWorkspace{3});
    REQUIRE(r.state.currentWorkspace == 3);
}

// ── eligibility: the pure window gate (substrate for focus + rules) ──────────

namespace {

// A fully-Eligible Probe result: every eligibility fact points the right way.
// Individual tests flip exactly one field to prove that field alone decides.
WindowAttrs eligible(WindowId id, MonitorId monitor) {
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

constexpr MonitorId kMon{1};

}  // namespace

TEST_CASE("isEligible is the AND of the probed facts — each condition alone flips the verdict", "[reducer]") {
    const WindowAttrs ok = eligible(WindowId{1}, kMon);
    REQUIRE(isEligible(ok));

    // Each required-true fact, cleared, drops it out.
    auto no_top = ok; no_top.topLevel = false; REQUIRE_FALSE(isEligible(no_top));
    auto no_vis = ok; no_vis.visible = false; REQUIRE_FALSE(isEligible(no_vis));
    auto no_thick = ok; no_thick.thickFrame = false; REQUIRE_FALSE(isEligible(no_thick));
    auto no_cap = ok; no_cap.caption = false; REQUIRE_FALSE(isEligible(no_cap));

    // Each must-be-false fact, set, drops it out.
    auto tool = ok; tool.toolWindow = true; REQUIRE_FALSE(isEligible(tool));
    auto cloaked = ok; cloaked.cloaked = true; REQUIRE_FALSE(isEligible(cloaked));
    auto fs = ok; fs.fullscreen = true; REQUIRE_FALSE(isEligible(fs));
}

// ── spatial directional focus: the pure resolution rule (ADR-0008) ───────────
//
// These feed a FocusMove / FocusResolve Event and assert on the emitted Effect,
// exactly as the workspace tests do. Rects are in virtual-screen coordinates
// (Win32: x grows right, y grows down). Cross-monitor resolution is covered here
// (the VM is single-display) — it is just a farther rect in the shared space.

namespace {

// A fully-Eligible window at a given rect. Individual tests flip one field to
// make a Candidate Ineligible, or move the rect to place it in the layout.
WindowAttrs win(WindowId id, Rect rect) {
    WindowAttrs a = eligible(id, kMon);
    a.rect = rect;
    return a;
}

using reducer::Direction;

}  // namespace

TEST_CASE("FocusMove emits exactly ResolveFocus for that direction and leaves State untouched", "[reducer][focus]") {
    const State s{.currentWorkspace = 2, .running = true};

    const auto r = reduce(s, FocusMove{Direction::Right});

    // Phase one asks the Worker to probe; it decides nothing yet and touches no State.
    REQUIRE(single_effect<ResolveFocus>(r) == ResolveFocus{Direction::Right});
    REQUIRE(r.state.currentWorkspace == 2);
    REQUIRE(r.state.running);
}

TEST_CASE("a neighbor in the direction is focused; the opposite direction with nothing there is a no-op", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto right = win(WindowId{2}, {200, 0, 300, 100});
    const std::vector<WindowAttrs> candidates{origin, right};

    const auto go = reduce(State{}, FocusResolve{Direction::Right, candidates, origin});
    REQUIRE(single_effect<winspace::SetForegroundWindow>(go) == winspace::SetForegroundWindow{WindowId{2}});

    // Nothing to the left → no Effect (focus never wraps).
    const auto nope = reduce(State{}, FocusResolve{Direction::Left, candidates, origin});
    REQUIRE(nope.effects.empty());
}

TEST_CASE("an aligned (in-band) window beats a closer diagonal one", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    // Aligned: same row as the Origin, directly right, but farther by center.
    const auto aligned = win(WindowId{2}, {300, 0, 400, 100});
    // Diagonal: below-and-right, closer by center distance, but off-band.
    const auto diagonal = win(WindowId{3}, {200, 200, 300, 300});

    const auto r =
        reduce(State{}, FocusResolve{Direction::Right, {origin, aligned, diagonal}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{2}});
}

TEST_CASE("a Candidate overlapping the Origin but center-forward is still reachable", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    // Overlaps the Origin's x-span (left 40 < origin right 100) but its center
    // (x=140) is past the Origin's (x=50), so an edge-ahead test would drop it and
    // center-ahead keeps it.
    const auto overlap = win(WindowId{2}, {40, 0, 240, 100});

    const auto r = reduce(State{}, FocusResolve{Direction::Right, {origin, overlap}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{2}});
}

TEST_CASE("the geometrically nearest window is skipped when Ineligible; the next Eligible is chosen", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    auto nearer = win(WindowId{2}, {150, 0, 250, 100});
    nearer.toolWindow = true;  // Ineligible — the gate must skip it inside the Reducer
    const auto farther = win(WindowId{3}, {300, 0, 400, 100});

    const auto r =
        reduce(State{}, FocusResolve{Direction::Right, {origin, nearer, farther}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{3}});
}

TEST_CASE("the Origin is never its own target", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto right = win(WindowId{2}, {200, 0, 300, 100});
    // The Origin is in the probed set (the sweep always sees the foreground window).
    const auto r = reduce(State{}, FocusResolve{Direction::Right, {origin, right}, origin});

    const auto& chosen = single_effect<winspace::SetForegroundWindow>(r);
    REQUIRE(chosen == winspace::SetForegroundWindow{WindowId{2}});
    REQUIRE_FALSE(chosen == winspace::SetForegroundWindow{WindowId{1}});
}

TEST_CASE("an Ineligible Origin is still a valid reference point", "[reducer][focus]") {
    auto origin = win(WindowId{1}, {0, 0, 100, 100});
    origin.toolWindow = true;  // the Origin's own Eligibility is never checked
    const auto right = win(WindowId{2}, {200, 0, 300, 100});

    const auto r = reduce(State{}, FocusResolve{Direction::Right, {origin, right}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{2}});
}

TEST_CASE("no Eligible Candidate ahead in the direction is a no-op", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto behind = win(WindowId{2}, {-200, 0, -100, 100});  // to the left

    const auto r = reduce(State{}, FocusResolve{Direction::Right, {origin, behind}, origin});

    REQUIRE(r.effects.empty());
}

TEST_CASE("no foreground Origin is a no-op", "[reducer][focus]") {
    const auto a = win(WindowId{2}, {200, 0, 300, 100});
    const auto b = win(WindowId{3}, {-200, 0, -100, 100});

    const auto r = reduce(State{}, FocusResolve{Direction::Right, {a, b}, std::nullopt});

    REQUIRE(r.effects.empty());
}

TEST_CASE("traversal crosses monitors via virtual-screen coordinates, no boundary special case", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});  // display 1
    // A window on the second display is just a far-right rect in the shared space.
    auto onSecondDisplay = win(WindowId{2}, {2000, 0, 2100, 100});
    onSecondDisplay.monitor = MonitorId{2};

    const auto r =
        reduce(State{}, FocusResolve{Direction::Right, {origin, onSecondDisplay}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{2}});
}

TEST_CASE("two Candidates identical in band and distance break the tie by lowest WindowId", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    // Same rect ⇒ same band and same center distance; only the id differs. The
    // higher id is listed FIRST to prove the choice is not enumeration order.
    const auto high = win(WindowId{5}, {200, 0, 300, 100});
    const auto low = win(WindowId{3}, {200, 0, 300, 100});

    const auto r = reduce(State{}, FocusResolve{Direction::Right, {high, low}, origin});

    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) == winspace::SetForegroundWindow{WindowId{3}});
}

TEST_CASE("y-down convention: focus down picks the +y Candidate, focus up the -y", "[reducer][focus]") {
    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto below = win(WindowId{2}, {0, 200, 100, 300});    // larger y (down)
    const auto above = win(WindowId{3}, {0, -200, 100, -100});  // smaller y (up)
    const std::vector<WindowAttrs> candidates{origin, below, above};

    const auto down = reduce(State{}, FocusResolve{Direction::Down, candidates, origin});
    REQUIRE(single_effect<winspace::SetForegroundWindow>(down) == winspace::SetForegroundWindow{WindowId{2}});

    const auto up = reduce(State{}, FocusResolve{Direction::Up, candidates, origin});
    REQUIRE(single_effect<winspace::SetForegroundWindow>(up) == winspace::SetForegroundWindow{WindowId{3}});
}

// ── window rules: match + place-once ─────────────────────────────────────────
//
// These feed synthetic Appeared / Vanished Events with hand-built WindowAttrs +
// WindowIdentity and assert on the emitted Effects, never on State internals —
// the `placed` set is observed only through what a later Appeared does or doesn't
// emit. No WM library is linked here, so the matcher's purity is linker-enforced.

namespace {

std::shared_ptr<const std::vector<WindowRule>> ruleList(std::vector<WindowRule> rs) {
    return std::make_shared<const std::vector<WindowRule>>(std::move(rs));
}

WindowRule exeRule(int ws, std::string exe) {
    return WindowRule{Field::Exe, RuleAction::Place, ws, std::move(exe), std::regex{}};
}
WindowRule classRule(int ws, std::string cls) {
    return WindowRule{Field::Class, RuleAction::Place, ws, std::move(cls), std::regex{}};
}
WindowRule titleRule(int ws, const std::string& pattern) {
    return WindowRule{Field::Title, RuleAction::Place, ws, pattern, std::regex(pattern)};
}

// An Ignore-action rule. workspace is unused for Ignore, so 0.
WindowRule ignoreExeRule(std::string exe) {
    return WindowRule{Field::Exe, RuleAction::Ignore, 0, std::move(exe), std::regex{}};
}
WindowRule ignoreTitleRule(const std::string& pattern) {
    return WindowRule{Field::Title, RuleAction::Ignore, 0, pattern, std::regex(pattern)};
}

State stateWith(std::vector<WindowRule> rs) {
    State s;
    s.rules = ruleList(std::move(rs));
    return s;
}

// An Appeared with a fully-Eligible WindowAttrs (from `eligible` above) and the
// given identity strings; the id is shared by both halves, as the adapter mints it.
Appeared appeared(WindowId id, std::string exe, std::string cls, std::string title) {
    return Appeared{eligible(id, kMon),
                    WindowIdentity{id, std::move(exe), std::move(cls), std::move(title)}};
}

}  // namespace

TEST_CASE("an Appeared matching a rule emits exactly MoveWindowToWorkspace{thatId, N}", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto r = reduce(s, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));

    REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{7}, 2});
}

TEST_CASE("a repeat Appeared for an already-placed id emits nothing", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto first = reduce(s, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(single_effect<MoveWindowToWorkspace>(first) == MoveWindowToWorkspace{WindowId{7}, 2});

    // Same id again, reduced against the state the first Appeared produced.
    const auto second = reduce(first.state, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(second.effects.empty());
}

TEST_CASE("field precedence is exe over class over title, independent of config order", "[reducer][rules]") {
    // One window matches all three fields; the rules are listed title, class, exe
    // (reverse precedence) to prove precedence is by FIELD, not vector position.
    const State s = stateWith(
        {titleRule(30, "Term"), classRule(40, "ConsoleWindowClass"), exeRule(20, "wt.exe")});

    const auto r = reduce(s, appeared(WindowId{1}, "wt.exe", "ConsoleWindowClass", "Terminal"));

    REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{1}, 20});
}

TEST_CASE("within a field, config order breaks ties — first match wins", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "app.exe"), exeRule(9, "app.exe")});

    const auto r = reduce(s, appeared(WindowId{1}, "app.exe", "cls", "title"));

    REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{1}, 2});
}

TEST_CASE("exe matching is ASCII case-insensitive and exact (not substring)", "[reducer][rules]") {
    const State s = stateWith({exeRule(5, "slack.exe")});

    const auto hit = reduce(s, appeared(WindowId{1}, "SLACK.EXE", "cls", "t"));
    REQUIRE(single_effect<MoveWindowToWorkspace>(hit) == MoveWindowToWorkspace{WindowId{1}, 5});

    // A near-miss matches no rule, so it is Distributed rather than left alone.
    const auto miss = reduce(s, appeared(WindowId{2}, "slack.exe.bak", "cls", "t"));
    REQUIRE(single_effect<ResolveDistribute>(miss) == ResolveDistribute{WindowId{2}});
}

TEST_CASE("an ineligible Appeared emits nothing and is not placed; a later eligible edge pins", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    // First edge: cloaked → Ineligible (the UWP cloaked-SHOW case). Emits nothing
    // AND must not burn place-once.
    Appeared cloaked = appeared(WindowId{7}, "slack.exe", "cls", "Slack");
    cloaked.attrs.cloaked = true;
    const auto first = reduce(s, cloaked);
    REQUIRE(first.effects.empty());

    // Second edge for the SAME id, now Eligible (UNCLOAKED) — the move fires here.
    const auto second = reduce(first.state, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(single_effect<MoveWindowToWorkspace>(second) == MoveWindowToWorkspace{WindowId{7}, 2});
}

TEST_CASE("an eligible non-matching Appeared is Distributed and never re-evaluated", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    // No rule matches, so the window is Distributed (place-once still burns).
    const auto first = reduce(s, appeared(WindowId{7}, "notepad.exe", "cls", "t"));
    REQUIRE(single_effect<ResolveDistribute>(first) == ResolveDistribute{WindowId{7}});

    // Even a now-matching identity for the same id emits nothing — it is placed.
    const auto second = reduce(first.state, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(second.effects.empty());
}

TEST_CASE("Vanished erases the id so a fresh Appeared re-pins", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto placed = reduce(s, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(single_effect<MoveWindowToWorkspace>(placed) == MoveWindowToWorkspace{WindowId{7}, 2});

    const auto gone = reduce(placed.state, Vanished{WindowId{7}});
    REQUIRE(gone.effects.empty());

    const auto again = reduce(gone.state, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));
    REQUIRE(single_effect<MoveWindowToWorkspace>(again) == MoveWindowToWorkspace{WindowId{7}, 2});
}

TEST_CASE("title matching is substring and case-sensitive", "[reducer][rules]") {
    // title:Slack matches "Slack | general" (substring, unanchored).
    {
        const auto r = reduce(stateWith({titleRule(3, "Slack")}),
                              appeared(WindowId{1}, "e", "c", "Slack | general"));
        REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{1}, 3});
    }
    // title:^Slack$ does NOT match "Slack | general" (anchored whole-title) — so the
    // window matches no rule and is Distributed.
    {
        const auto r = reduce(stateWith({titleRule(3, "^Slack$")}),
                              appeared(WindowId{2}, "e", "c", "Slack | general"));
        REQUIRE(single_effect<ResolveDistribute>(r) == ResolveDistribute{WindowId{2}});
    }
    // title:slack does NOT match "Slack" (case-sensitive) — Distributed.
    {
        const auto r = reduce(stateWith({titleRule(3, "slack")}),
                              appeared(WindowId{3}, "e", "c", "Slack"));
        REQUIRE(single_effect<ResolveDistribute>(r) == ResolveDistribute{WindowId{3}});
    }
}

TEST_CASE("an Appeared with no rules seeded still Distributes and places the id", "[reducer][rules]") {
    // A default State has a null rules handle; matching is skipped safely, so the
    // window matches nothing and is Distributed — and place-once still records the id.
    const auto first = reduce(State{}, appeared(WindowId{1}, "slack.exe", "cls", "Slack"));
    REQUIRE(single_effect<ResolveDistribute>(first) == ResolveDistribute{WindowId{1}});
    const auto second = reduce(first.state, appeared(WindowId{1}, "slack.exe", "cls", "Slack"));
    REQUIRE(second.effects.empty());
}

// ── windowrule slot placement on Appeared (ADR-0016) ─────────────────────────

namespace {
// An exe Place rule carrying a Slot geometry target.
WindowRule exeSlotRule(int ws, std::string exe, Slot slot) {
    return WindowRule{Field::Exe, RuleAction::Place, ws, std::move(exe), std::regex{}, slot};
}
}  // namespace

TEST_CASE("an Appeared matching a Slot-bearing Place rule emits PositionWindow then the move", "[reducer][rules][slot]") {
    const State s = stateWith({exeSlotRule(2, "code.exe", Slot::RightHalf)});

    const auto r = reduce(s, appeared(WindowId{7}, "code.exe", "cls", "Code"));

    // Two Effects, in this exact order: position while still on the current
    // Desktop, THEN cloak-and-move to the workspace (ADR-0016 emission order).
    REQUIRE(r.effects.size() == 2);
    REQUIRE(std::holds_alternative<PositionWindow>(r.effects[0]));
    REQUIRE(std::get<PositionWindow>(r.effects[0]) ==
            PositionWindow{WindowId{7}, std::nullopt, Slot::RightHalf});
    REQUIRE(std::holds_alternative<MoveWindowToWorkspace>(r.effects[1]));
    REQUIRE(std::get<MoveWindowToWorkspace>(r.effects[1]) == MoveWindowToWorkspace{WindowId{7}, 2});
}

TEST_CASE("a matched workspace-only rule (no slot) still emits only the move", "[reducer][rules][slot]") {
    // Regression: a rule without a slot behaves exactly as before ADR-0016.
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto r = reduce(s, appeared(WindowId{7}, "slack.exe", "cls", "Slack"));

    REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{7}, 2});
}

TEST_CASE("a Slot-bearing Place rule places the id once — a later edge is a no-op", "[reducer][rules][slot]") {
    const State s = stateWith({exeSlotRule(2, "code.exe", Slot::Maximized)});

    const auto first = reduce(s, appeared(WindowId{7}, "code.exe", "cls", "Code"));
    REQUIRE(first.effects.size() == 2);

    // Place-once: the same id again emits nothing (no re-position, no re-move).
    const auto second = reduce(first.state, appeared(WindowId{7}, "code.exe", "cls", "Code"));
    REQUIRE(second.effects.empty());
}

// ── tile dispatcher: the full-pipeline rebalance (ADR-0016, ADR-0020) ─────────

namespace {
// A probed window for the tile sweep: a fully-Eligible attrs (from `eligible`)
// paired with the given identity, sharing the id as the adapter mints it. `probed`
// puts it on the default monitor; `probedOn` names the monitor for balancing tests.
ProbedWindow probedOn(WindowId id, std::string exe, std::string cls, std::string title,
                      MonitorId monitor) {
    return ProbedWindow{eligible(id, monitor),
                        WindowIdentity{id, std::move(exe), std::move(cls), std::move(title)}};
}
ProbedWindow probed(WindowId id, std::string exe, std::string cls, std::string title) {
    return probedOn(id, std::move(exe), std::move(cls), std::move(title), kMon);
}
}  // namespace

TEST_CASE("a Tile Event emits exactly ResolveTile and touches no State", "[reducer][tile]") {
    const State s = stateWith({exeSlotRule(2, "code.exe", Slot::LeftHalf)});

    const auto r = reduce(s, Tile{});

    REQUIRE(single_effect<ResolveTile>(r) == ResolveTile{});
    // Stateless: the placed/ignored sets and workspace are untouched.
    REQUIRE(r.state.placed.empty());
    REQUIRE(r.state.currentWorkspace == s.currentWorkspace);
}

TEST_CASE("TileResolve places each Eligible Slot-matched window to its Slot on its own monitor", "[reducer][tile]") {
    const State s = stateWith({exeSlotRule(2, "code.exe", Slot::LeftHalf),
                               exeSlotRule(3, "slack.exe", Slot::RightHalf)});

    // Three windows: two Eligible + Slot-matched, one matched-but-Ineligible
    // (cloaked → on another Workspace, skipped for free).
    ProbedWindow code = probed(WindowId{1}, "code.exe", "cls", "Code");
    ProbedWindow slack = probed(WindowId{2}, "slack.exe", "cls", "Slack");
    ProbedWindow cloakedCode = probed(WindowId{4}, "code.exe", "cls", "Code");
    cloakedCode.attrs.cloaked = true;  // Ineligible — on another Virtual Desktop

    const auto r = reduce(s, TileResolve{{code, slack, cloakedCode}, {kMon}});

    // A Slot rule places to its Slot on its own monitor (target nullopt); the cloaked
    // window is skipped. No MoveWindowToWorkspace ever (no cross-Workspace teleport).
    REQUIRE(r.effects.size() == 2);
    REQUIRE(std::get<PositionWindow>(r.effects[0]) ==
            PositionWindow{WindowId{1}, std::nullopt, Slot::LeftHalf});
    REQUIRE(std::get<PositionWindow>(r.effects[1]) ==
            PositionWindow{WindowId{2}, std::nullopt, Slot::RightHalf});

    // Stateless: no id entered `placed` (tile is deliberately re-placeable).
    REQUIRE(r.state.placed.empty());
}

TEST_CASE("TileResolve never moves an Ignore-matched window", "[reducer][tile]") {
    // Ignore widens to "don't touch at all": counted as occupancy but never placed.
    const State s = stateWith({ignoreExeRule("dock.exe")});

    const auto r = reduce(s, TileResolve{{probed(WindowId{1}, "dock.exe", "cls", "Dock")}, {kMon}});

    REQUIRE(r.effects.empty());
}

TEST_CASE("TileResolve leaves a workspace-only Place window put (counted, not moved)", "[reducer][tile]") {
    // A workspace-only Place rule pins Workspace, not geometry — tile never teleports
    // a window between Workspaces, so it emits no Effect for it (but still counts it).
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto r = reduce(s, TileResolve{{probed(WindowId{1}, "slack.exe", "cls", "Slack")}, {kMon}});

    REQUIRE(r.effects.empty());
}

TEST_CASE("TileResolve Distributes each free window, maximized, on a single display", "[reducer][tile]") {
    // No rules → every window is free. One display → each maximizes in place (target
    // nullopt, since its own display always ties for least-occupied).
    const auto r = reduce(State{}, TileResolve{{probed(WindowId{1}, "code.exe", "cls", "Code")}, {kMon}});
    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{1}, std::nullopt, Slot::Maximized});
}

TEST_CASE("TileResolve balances free windows across displays, including empty ones", "[reducer][tile]") {
    const MonitorId monA{1}, monB{2};
    // Two free windows both currently on monA; tile spreads them across both displays.
    ProbedWindow w1 = probedOn(WindowId{1}, "a.exe", "cls", "A", monA);
    ProbedWindow w2 = probedOn(WindowId{2}, "b.exe", "cls", "B", monA);

    const auto r = reduce(State{}, TileResolve{{w1, w2}, {monA, monB}});

    REQUIRE(r.effects.size() == 2);
    // w1: monA ties the min (both 0) → stays put (nullopt); monA's count becomes 1.
    REQUIRE(std::get<PositionWindow>(r.effects[0]) ==
            PositionWindow{WindowId{1}, std::nullopt, Slot::Maximized});
    // w2: monA is now 1, monB still 0 → monB is least-occupied → moved there.
    REQUIRE(std::get<PositionWindow>(r.effects[1]) ==
            PositionWindow{WindowId{2}, monB, Slot::Maximized});
}

TEST_CASE("TileResolve counts a fixed window's occupancy so a free window balances away from it", "[reducer][tile]") {
    const MonitorId monA{1}, monB{2};
    const State s = stateWith({exeRule(5, "pinned.exe")});  // workspace-only Place
    // A pinned window on monA (counted, not moved) and a free window also on monA.
    ProbedWindow pinned = probedOn(WindowId{1}, "pinned.exe", "cls", "P", monA);
    ProbedWindow freeW = probedOn(WindowId{2}, "free.exe", "cls", "F", monA);

    const auto r = reduce(s, TileResolve{{pinned, freeW}, {monA, monB}});

    // Only the free window is placed; monA already carries the pinned window (count 1)
    // so the empty monB wins and the free window moves there.
    REQUIRE(single_effect<PositionWindow>(r) == PositionWindow{WindowId{2}, monB, Slot::Maximized});
}

TEST_CASE("TileResolve with no displays enumerated still places free windows in place", "[reducer][tile]") {
    // Degenerate: no Displays reported → pickDistributeTarget yields nullopt, so each
    // free window maximizes on its current monitor rather than being dropped.
    const auto r = reduce(State{}, TileResolve{{probed(WindowId{1}, "code.exe", "cls", "Code")}, {}});
    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{1}, std::nullopt, Slot::Maximized});
}

// ── windowrule ignore action + the ignored set ───────────────────────────────

TEST_CASE("an Appeared matching an Ignore rule emits no move — the id is recorded, not placed elsewhere", "[reducer][rules]") {
    // Ignore never moves the window (winspace acts on it in no way), yet the window
    // is Eligible and evaluated, so a repeat Appeared is not re-matched.
    const State s = stateWith({ignoreExeRule("dock.exe")});

    const auto first = reduce(s, appeared(WindowId{2}, "dock.exe", "cls", "Dock"));
    REQUIRE(first.effects.empty());

    const auto second = reduce(first.state, appeared(WindowId{2}, "dock.exe", "cls", "Dock"));
    REQUIRE(second.effects.empty());
}

TEST_CASE("resolveFocus never returns an ignored window and still selects the next-best candidate", "[reducer][rules][focus]") {
    const State base = stateWith({ignoreExeRule("dock.exe")});
    // The dock Appears and is recorded in `ignored` (no move Effect emitted).
    const auto afterIgnore = reduce(base, appeared(WindowId{2}, "dock.exe", "cls", "Dock"));
    REQUIRE(afterIgnore.effects.empty());

    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto dock = win(WindowId{2}, {150, 0, 250, 100});   // nearer — but ignored
    const auto other = win(WindowId{3}, {300, 0, 400, 100});  // farther — Eligible target

    const auto r = reduce(afterIgnore.state,
                          FocusResolve{Direction::Right, {origin, dock, other}, origin});

    // The nearer window is Eligible yet skipped by id; focus lands on the next-best.
    REQUIRE(single_effect<winspace::SetForegroundWindow>(r) ==
            winspace::SetForegroundWindow{WindowId{3}});
}

TEST_CASE("a window matching both an Ignore and a Place rule resolves by exe→class→title precedence", "[reducer][rules]") {
    // exe:ignore vs title:place — exe precedes title, so Ignore wins → no move.
    {
        const State s = stateWith({titleRule(3, "Term"), ignoreExeRule("wt.exe")});
        const auto r = reduce(s, appeared(WindowId{1}, "wt.exe", "cls", "Terminal"));
        REQUIRE(r.effects.empty());
    }
    // title:ignore vs exe:place — exe precedes title, so Place wins → move emitted.
    {
        const State s = stateWith({ignoreTitleRule("Term"), exeRule(3, "wt.exe")});
        const auto r = reduce(s, appeared(WindowId{2}, "wt.exe", "cls", "Terminal"));
        REQUIRE(single_effect<MoveWindowToWorkspace>(r) == MoveWindowToWorkspace{WindowId{2}, 3});
    }
}

TEST_CASE("Vanished clears an ignored id so the window becomes a focus target again", "[reducer][rules][focus]") {
    const State base = stateWith({ignoreExeRule("dock.exe")});
    const auto ignored = reduce(base, appeared(WindowId{2}, "dock.exe", "cls", "Dock"));

    const auto origin = win(WindowId{1}, {0, 0, 100, 100});
    const auto dock = win(WindowId{2}, {150, 0, 250, 100});

    // While ignored, focus skips it and (nothing else ahead) is a no-op.
    const auto skip = reduce(ignored.state, FocusResolve{Direction::Right, {origin, dock}, origin});
    REQUIRE(skip.effects.empty());

    // After Vanished the id is cleared, so a candidate with that id is focusable again.
    const auto gone = reduce(ignored.state, Vanished{WindowId{2}});
    const auto after = reduce(gone.state, FocusResolve{Direction::Right, {origin, dock}, origin});
    REQUIRE(single_effect<winspace::SetForegroundWindow>(after) ==
            winspace::SetForegroundWindow{WindowId{2}});
}

// ── Distribute: auto-placement round-trip + pickDistributeTarget (ADR-0020) ───
//
// Distribute is a two-phase Probe round-trip mirroring Spatial focus (ADR-0008): an
// Eligible Appeared matching NO rule emits a ResolveDistribute request (no geometry
// yet — the pure Reducer cannot enumerate Displays), and the probed Display counts
// re-enter as a DistributeResolve Event the Reducer turns into a single
// PositionWindow (always — there is no overflow no-op). These feed synthetic Events
// and assert on the emitted Effect; the balancing brain (pickDistributeTarget) is
// unit-tested directly below, like rectForSlot, with no Win32.

TEST_CASE("an unmatched Eligible Appeared emits exactly ResolveDistribute{thatId} and no geometry", "[reducer][distribute]") {
    // No rules seeded → nothing matches → Distribute. Phase one only asks the Worker
    // to probe occupancy; it decides no target and writes no geometry yet.
    const auto r = reduce(State{}, appeared(WindowId{7}, "term.exe", "cls", "Terminal"));
    REQUIRE(single_effect<ResolveDistribute>(r) == ResolveDistribute{WindowId{7}});
}

TEST_CASE("a Distributed window is place-once — a repeat Appeared for the placed id emits nothing", "[reducer][distribute]") {
    const auto first = reduce(State{}, appeared(WindowId{7}, "term.exe", "cls", "Terminal"));
    REQUIRE(single_effect<ResolveDistribute>(first) == ResolveDistribute{WindowId{7}});

    // Same id again (an uncloak), reduced against the state the first Appeared left:
    // it is already `placed`, so Distribute never re-probes — a dragged window is not
    // yanked back (story 8).
    const auto second = reduce(first.state, appeared(WindowId{7}, "term.exe", "cls", "Terminal"));
    REQUIRE(second.effects.empty());
}

TEST_CASE("an Ineligible unmatched Appeared probes nothing; a later Eligible edge Distributes", "[reducer][distribute]") {
    // First edge: cloaked → Ineligible. Emits nothing AND must not burn place-once.
    Appeared cloaked = appeared(WindowId{7}, "term.exe", "cls", "Terminal");
    cloaked.attrs.cloaked = true;
    const auto first = reduce(State{}, cloaked);
    REQUIRE(first.effects.empty());

    // Second edge for the SAME id, now Eligible (UNCLOAKED) — the probe fires here.
    const auto second = reduce(first.state, appeared(WindowId{7}, "term.exe", "cls", "Terminal"));
    REQUIRE(single_effect<ResolveDistribute>(second) == ResolveDistribute{WindowId{7}});
}

TEST_CASE("DistributeResolve maximizes the subject on the least-occupied Display", "[reducer][distribute]") {
    // Subject sits on the busiest Display; the emptiest one wins.
    const DistributeResolve dr{WindowId{7}, MonitorId{10},
                               {{MonitorId{10}, 2}, {MonitorId{20}, 0}, {MonitorId{30}, 1}}};

    const auto r = reduce(State{}, dr);

    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{7}, MonitorId{20}, Slot::Maximized});
}

TEST_CASE("DistributeResolve always places — no overflow no-op even when every Display carries windows", "[reducer][distribute]") {
    // Every Display occupied; the subject is on the busier one. It still lands on the
    // least-occupied Display (overlap accepted), never left where the OS opened it.
    const DistributeResolve dr{WindowId{7}, MonitorId{10},
                               {{MonitorId{10}, 2}, {MonitorId{20}, 1}}};

    const auto r = reduce(State{}, dr);

    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{7}, MonitorId{20}, Slot::Maximized});
}

TEST_CASE("DistributeResolve keeps the subject put when its Display ties for least-occupied", "[reducer][distribute]") {
    // The subject already sits on a least-occupied Display → target nullopt (its
    // current monitor), only maximized, never jumped to another monitor (story 13).
    const DistributeResolve dr{WindowId{7}, MonitorId{20},
                               {{MonitorId{10}, 1}, {MonitorId{20}, 1}}};

    const auto r = reduce(State{}, dr);

    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{7}, std::nullopt, Slot::Maximized});
}

TEST_CASE("DistributeResolve leaves State untouched (place-once was recorded on Appeared)", "[reducer][distribute]") {
    State s;
    s.currentWorkspace = 3;
    s.placed = {WindowId{7}};  // already placed when the subject Appeared

    const DistributeResolve dr{WindowId{7}, MonitorId{10}, {{MonitorId{10}, 1}, {MonitorId{20}, 0}}};

    const auto r = reduce(s, dr);

    REQUIRE(single_effect<PositionWindow>(r) ==
            PositionWindow{WindowId{7}, MonitorId{20}, Slot::Maximized});
    REQUIRE(r.state.currentWorkspace == 3);
    REQUIRE(r.state.placed == std::unordered_set<WindowId>{WindowId{7}});
}

// ── pickDistributeTarget: the pure balancing decision (ADR-0020) ──────────────

TEST_CASE("pickDistributeTarget picks the least-occupied Display", "[reducer][distribute]") {
    const std::vector<DisplayOccupancy> counts{
        {MonitorId{10}, 2}, {MonitorId{20}, 0}, {MonitorId{30}, 1}};
    // The subject is on the busiest Display, so no tie keeps it put — the emptiest wins.
    REQUIRE(pickDistributeTarget(counts, MonitorId{10}) == MonitorId{20});
}

TEST_CASE("pickDistributeTarget prefers the current Display when it ties for least-occupied", "[reducer][distribute]") {
    const std::vector<DisplayOccupancy> counts{{MonitorId{10}, 1}, {MonitorId{20}, 1}};
    // current ties the minimum → nullopt (keep it put, no pointless cross-monitor jump).
    REQUIRE(pickDistributeTarget(counts, MonitorId{20}) == std::nullopt);
}

TEST_CASE("pickDistributeTarget breaks a tie by first-in-order when current is not least-occupied", "[reducer][distribute]") {
    const std::vector<DisplayOccupancy> counts{
        {MonitorId{10}, 0}, {MonitorId{20}, 2}, {MonitorId{30}, 0}};
    // current (mon20) is the busiest; two Displays tie at 0 → the FIRST in order wins.
    REQUIRE(pickDistributeTarget(counts, MonitorId{20}) == MonitorId{10});
}

TEST_CASE("pickDistributeTarget with the current Display absent picks the first least-occupied", "[reducer][distribute]") {
    const std::vector<DisplayOccupancy> counts{{MonitorId{10}, 0}, {MonitorId{20}, 0}};
    // current (mon99) is on none of the enumerated Displays → first least-occupied
    // in order (story 14).
    REQUIRE(pickDistributeTarget(counts, MonitorId{99}) == MonitorId{10});
}

TEST_CASE("pickDistributeTarget on a single Display keeps the window put", "[reducer][distribute]") {
    const std::vector<DisplayOccupancy> counts{{MonitorId{10}, 3}};
    // One display always ties itself for least-occupied → nullopt (maximize in place).
    REQUIRE(pickDistributeTarget(counts, MonitorId{10}) == std::nullopt);
}

TEST_CASE("pickDistributeTarget with no Displays yields nullopt", "[reducer][distribute]") {
    REQUIRE(pickDistributeTarget({}, MonitorId{10}) == std::nullopt);
}

// ── launcher: Started / Reloaded → LaunchApp ─────────────────────────────────
//
// A State seeded with a synthetic exec list; assert the LaunchApp Effects. Started
// launches every entry in config order; Reloaded launches only the `exec`
// (once == false) entries. exec-once idempotency is observed purely through which
// Event fires — there is no launch flag in State to inspect.

namespace {

State stateWithExecs(std::vector<ExecEntry> es) {
    State s;
    s.execs = std::make_shared<const std::vector<ExecEntry>>(std::move(es));
    return s;
}

// The command carried by the i-th emitted LaunchApp, or fail loudly if that Effect
// is not a LaunchApp.
std::string launched(const ReduceResult& r, size_t i) {
    REQUIRE(i < r.effects.size());
    REQUIRE(std::holds_alternative<LaunchApp>(r.effects[i]));
    return std::get<LaunchApp>(r.effects[i]).command;
}

}  // namespace

TEST_CASE("Started launches every exec entry, exec-once and exec alike, in config order", "[reducer][launcher]") {
    const State s = stateWithExecs({{"firefox", true},        // exec-once
                                    {"kitty", false},         // exec
                                    {"code --new-window", true}});

    const auto r = reduce(s, Started{});

    // The three launches in config order, then the trailing SyncAutostart.
    REQUIRE(r.effects.size() == 4);
    REQUIRE(launched(r, 0) == "firefox");
    REQUIRE(launched(r, 1) == "kitty");
    REQUIRE(launched(r, 2) == "code --new-window");
    REQUIRE(std::holds_alternative<SyncAutostart>(r.effects[3]));
    // Startup persists nothing — idempotency is stateless.
    REQUIRE(r.state.running);
}

TEST_CASE("Reloaded launches only the exec entries, skipping exec-once, in config order", "[reducer][launcher]") {
    const State s = stateWithExecs({{"firefox", true},   // exec-once — skipped on reload
                                    {"kitty", false},    // exec — relaunched
                                    {"agent", true},     // exec-once — skipped
                                    {"tray", false}});   // exec — relaunched

    const auto r = reduce(s, Reloaded{});

    // The two `exec` relaunches, then the trailing SyncAutostart.
    REQUIRE(r.effects.size() == 3);
    REQUIRE(launched(r, 0) == "kitty");
    REQUIRE(launched(r, 1) == "tray");
    REQUIRE(std::holds_alternative<SyncAutostart>(r.effects[2]));
}

TEST_CASE("Started and Reloaded with no exec entries still emit exactly one SyncAutostart", "[reducer][launcher]") {
    // A default State has a null execs handle; both handlers skip it safely and are
    // left with only the SyncAutostart the flag always produces.
    REQUIRE(single_effect<SyncAutostart>(reduce(State{}, Started{})) == SyncAutostart{false});
    REQUIRE(single_effect<SyncAutostart>(reduce(State{}, Reloaded{})) == SyncAutostart{false});

    // An all-exec-once list yields only the SyncAutostart on reload, and every
    // launch plus the SyncAutostart on start.
    const State onceOnly = stateWithExecs({{"a", true}, {"b", true}});
    REQUIRE(single_effect<SyncAutostart>(reduce(onceOnly, Reloaded{})) == SyncAutostart{false});
    REQUIRE(reduce(onceOnly, Started{}).effects.size() == 3);  // 2 launches + SyncAutostart
}

// ── autostart: Started / Reloaded → SyncAutostart{startAtLogin} ──────────────
//
// Both lifecycle Events emit exactly one SyncAutostart carrying State.startAtLogin
// verbatim (declarative — the Reducer never decides register-vs-remove), and neither
// changes the flag. Asserted for both flag values; the Worker's ITaskService adapter
// (create-or-update / delete-if-exists) is a VM Smoke seam, not a unit test.

namespace {

// The bool carried by the single emitted SyncAutostart, failing loudly if the last
// Effect is not one. Started/Reloaded emit it last (after any LaunchApp).
bool syncedAutostart(const ReduceResult& r) {
    REQUIRE_FALSE(r.effects.empty());
    const Effect& last = r.effects.back();
    REQUIRE(std::holds_alternative<SyncAutostart>(last));
    return std::get<SyncAutostart>(last).enabled;
}

}  // namespace

TEST_CASE("Started emits exactly one SyncAutostart with the current flag and leaves State unchanged", "[reducer][autostart]") {
    for (const bool flag : {false, true}) {
        State s;
        s.startAtLogin = flag;

        const auto r = reduce(s, Started{});

        // Exactly one SyncAutostart (no execs here), carrying the flag verbatim.
        REQUIRE(r.effects.size() == 1);
        REQUIRE(single_effect<SyncAutostart>(r) == SyncAutostart{flag});
        REQUIRE(syncedAutostart(r) == flag);
        // The flag rides through unchanged — Started derives an Effect, not a mutation.
        REQUIRE(r.state.startAtLogin == flag);
    }
}

TEST_CASE("Reloaded emits exactly one SyncAutostart with the current flag and leaves State unchanged", "[reducer][autostart]") {
    for (const bool flag : {false, true}) {
        State s;
        s.startAtLogin = flag;

        const auto r = reduce(s, Reloaded{});

        REQUIRE(r.effects.size() == 1);
        REQUIRE(single_effect<SyncAutostart>(r) == SyncAutostart{flag});
        REQUIRE(syncedAutostart(r) == flag);
        REQUIRE(r.state.startAtLogin == flag);
    }
}

TEST_CASE("the launch command is passed through verbatim, including a literal $", "[reducer][launcher]") {
    // No $var expansion anywhere on the launch path — the Reducer emits the stored
    // string byte-for-byte.
    const State s = stateWithExecs({{"C:\\Program Files\\App\\app.exe --flag $HOME", true}});

    const auto r = reduce(s, Started{});

    REQUIRE(launched(r, 0) == "C:\\Program Files\\App\\app.exe --flag $HOME");
}

// ── reload trigger: Reload → ReloadConfig (ADR-0012) ─────────────────────────
//
// The Reducer is pure and cannot read a file, so a Reload{} only ASKS: it emits a
// single ReloadConfig{} Effect and mutates no State. The Worker does the re-read /
// re-parse / fan-out; that I/O is verified by the VM Smoke seam, not here.

TEST_CASE("a Reload Event emits exactly one ReloadConfig and changes no State", "[reducer][reload]") {
    State s{.currentWorkspace = 3, .running = true};
    s.startAtLogin = true;
    s.placed.insert(WindowId{7});

    const auto r = reduce(s, Reload{});

    REQUIRE(single_effect<ReloadConfig>(r) == ReloadConfig{});
    // Nothing about State moves — the Reducer neither reads a file nor reseeds here.
    REQUIRE(r.state.currentWorkspace == 3);
    REQUIRE(r.state.running);
    REQUIRE(r.state.startAtLogin);
    REQUIRE(r.state.placed.contains(WindowId{7}));
}

// ── start_at_login carried through State ────────────────────────────────────
//
// The flag is seeded at Worker construction and reseeded on reload; the Reducer
// only carries it — no Event derives an Effect from it, and every reduce preserves
// it unchanged (the logon task is what acts on it).

TEST_CASE("startAtLogin round-trips through State and no Event emits an Effect for it", "[reducer]") {
    State on;
    on.startAtLogin = true;

    // An Event that DOES change State still leaves startAtLogin untouched, and the
    // only Effect is that Event's own — never one derived from the flag.
    const auto sw = reduce(on, WorkspaceSwitch{4});
    REQUIRE(sw.state.startAtLogin);
    REQUIRE(single_effect<SwitchToWorkspace>(sw) == SwitchToWorkspace{4});

    // A default State carries false through unchanged.
    REQUIRE_FALSE(reduce(State{}, WorkspaceSwitch{1}).state.startAtLogin);
}

// ── slot geometry: rectForSlot pure arithmetic (ADR-0016) ────────────────────
//
// The highest-value unit surface: the fraction arithmetic tested with no Win32.
// A representative work area whose width and height are BOTH even, so the halves
// split exactly and the quarters meet on a clean midpoint (odd-pixel handling is
// its own case below). Origin is offset (100, 50) to prove the math respects a
// work area that does not start at (0, 0) — e.g. a taskbar-inset or second monitor.

namespace {
// left=100 top=50 right=1300 bottom=850 → 1200 × 800, mid at (700, 450).
constexpr Rect kWork{100, 50, 1300, 850};
constexpr int kMidX = 700;
constexpr int kMidY = 450;
}  // namespace

TEST_CASE("rectForSlot: halves split the work area on the midpoint", "[reducer][slot]") {
    REQUIRE(rectForSlot(Slot::LeftHalf, kWork) == Rect{100, 50, kMidX, 850});
    REQUIRE(rectForSlot(Slot::RightHalf, kWork) == Rect{kMidX, 50, 1300, 850});
    REQUIRE(rectForSlot(Slot::TopHalf, kWork) == Rect{100, 50, 1300, kMidY});
    REQUIRE(rectForSlot(Slot::BottomHalf, kWork) == Rect{100, kMidY, 1300, 850});
}

TEST_CASE("rectForSlot: the four quarters tile with no gap and no overlap", "[reducer][slot]") {
    const Rect tl = rectForSlot(Slot::TopLeftQuarter, kWork);
    const Rect tr = rectForSlot(Slot::TopRightQuarter, kWork);
    const Rect bl = rectForSlot(Slot::BottomLeftQuarter, kWork);
    const Rect br = rectForSlot(Slot::BottomRightQuarter, kWork);

    REQUIRE(tl == Rect{100, 50, kMidX, kMidY});
    REQUIRE(tr == Rect{kMidX, 50, 1300, kMidY});
    REQUIRE(bl == Rect{100, kMidY, kMidX, 850});
    REQUIRE(br == Rect{kMidX, kMidY, 1300, 850});

    // Adjacent quarters share their common edge exactly — the seam between them is
    // a zero-area line, so together they cover the whole work area with no gap and
    // no overlapping interior.
    REQUIRE(tl.right == tr.left);       // vertical seam, top row
    REQUIRE(bl.right == br.left);       // vertical seam, bottom row
    REQUIRE(tl.bottom == bl.top);       // horizontal seam, left column
    REQUIRE(tr.bottom == br.top);       // horizontal seam, right column
    // Outer edges reach the work-area bounds.
    REQUIRE(tl.left == kWork.left);
    REQUIRE(tr.right == kWork.right);
    REQUIRE(bl.bottom == kWork.bottom);
    REQUIRE(tl.top == kWork.top);
}

TEST_CASE("rectForSlot: maximized covers the full work area", "[reducer][slot]") {
    REQUIRE(rectForSlot(Slot::Maximized, kWork) == kWork);
}

TEST_CASE("rectForSlot: an odd-sized work area lands the extra pixel consistently", "[reducer][slot]") {
    // 1001 × 801 from the origin — an odd span on both axes. The midpoint uses
    // integer division, so the left/top halves get the floor and the right/bottom
    // halves absorb the extra pixel; the quarters still meet on the same midpoints.
    constexpr Rect odd{0, 0, 1001, 801};
    const int mx = 500;  // 0 + 1001/2
    const int my = 400;  // 0 + 801/2
    REQUIRE(rectForSlot(Slot::LeftHalf, odd) == Rect{0, 0, mx, 801});
    REQUIRE(rectForSlot(Slot::RightHalf, odd) == Rect{mx, 0, 1001, 801});
    REQUIRE(rectForSlot(Slot::TopHalf, odd) == Rect{0, 0, 1001, my});
    REQUIRE(rectForSlot(Slot::BottomHalf, odd) == Rect{0, my, 1001, 801});
    // Quarters still share the seam — no gap opens on the odd axis.
    REQUIRE(rectForSlot(Slot::TopLeftQuarter, odd).right ==
            rectForSlot(Slot::TopRightQuarter, odd).left);
    REQUIRE(rectForSlot(Slot::TopLeftQuarter, odd).bottom ==
            rectForSlot(Slot::BottomLeftQuarter, odd).top);
}

// ─────────────────────────────────────────────────────────────────────────────
// [config tests] Seam 2
// ─────────────────────────────────────────────────────────────────────────────

// Tests for the pure config parser (Seam 2). Included by the test TU only, so
// it links no WM import libraries — the parser under test is provably pure.



TEST_CASE("well-formed bind with $mod substitution parses to the expected Bind", "[config]") {
    const auto result = parse(
        "$mod = SUPER ALT\n"
        "bind = $mod, 1, workspace, 1\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super | Mod::Alt, Key::N1, Dispatcher::Workspace, 1});
}

TEST_CASE("the workspace argument is independent of the bound key", "[config]") {
    // Super+1 dispatches to workspace 2 — arg is not derived from the key.
    const auto result = parse("bind = SUPER, 1, workspace, 2\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::N1, Dispatcher::Workspace, 2});
}

TEST_CASE("comments and blank lines are ignored", "[config]") {
    const auto result = parse(
        "# a full-line comment\n"
        "\n"
        "   \n"
        "$mod = SUPER   # trailing comment after a variable def\n"
        "bind = $mod, 3, workspace, 3   # trailing comment after a bind\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::N3, Dispatcher::Workspace, 3});
}

TEST_CASE("the quit dispatcher is recognized and takes no argument", "[config]") {
    const auto result = parse(
        "$mod = SUPER ALT\n"
        "bind = $mod, Q, quit\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super | Mod::Alt, Key::Q, Dispatcher::Quit, 0});
}

TEST_CASE("a malformed line is diagnosed while valid binds still parse", "[config]") {
    const auto result = parse(
        "$mod = SUPER\n"
        "bind = $mod, 1, workspace, 1\n"
        "this line has no equals sign\n"
        "bind = $mod, 2, workspace, 2\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 3);  // 1-based, collect-and-continue

    REQUIRE(result.config.binds.size() == 2);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::N1, Dispatcher::Workspace, 1});
    REQUIRE(result.config.binds[1] == Bind{Mod::Super, Key::N2, Dispatcher::Workspace, 2});
}

TEST_CASE("letters, function keys, and named keys all parse", "[config]") {
    const auto result = parse(
        "bind = SUPER, A, quit\n"          // letter
        "bind = SUPER, f12, quit\n"        // function key, lowercase
        "bind = SUPER, Return, quit\n"     // named
        "bind = SUPER, PgDn, quit\n");     // named alias

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 4);
    REQUIRE(result.config.binds[0].key == Key::A);
    REQUIRE(result.config.binds[1].key == Key::F12);
    REQUIRE(result.config.binds[2].key == Key::Return);
    REQUIRE(result.config.binds[3].key == Key::PageDown);
}

TEST_CASE("an out-of-range function key is diagnosed", "[config]") {
    const auto result = parse("bind = SUPER, F25, workspace, 1\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
}

TEST_CASE("an unknown dispatcher is diagnosed in the parser, not deferred", "[config]") {
    const auto result = parse("bind = SUPER, 1, teleport, 1\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
}

TEST_CASE("an unknown modifier name is diagnosed", "[config]") {
    const auto result = parse("bind = HYPER, 1, workspace, 1\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("a workspace dispatcher with a non-integer argument is diagnosed", "[config]") {
    const auto result = parse("bind = SUPER, 1, workspace, two\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("a reference to an undefined variable is diagnosed", "[config]") {
    const auto result = parse("bind = $undefined, 1, workspace, 1\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("movetoworkspace and movetoworkspacesilent bind with a workspace number", "[config]") {
    const auto result = parse(
        "bind = SUPER SHIFT, 3, movetoworkspace, 3\n"
        "bind = SUPER SHIFT, 4, movetoworkspacesilent, 4\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 2);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super | Mod::Shift, Key::N3, Dispatcher::MoveToWorkspace, 3});
    REQUIRE(result.config.binds[1] ==
            Bind{Mod::Super | Mod::Shift, Key::N4, Dispatcher::MoveToWorkspaceSilent, 4});
}

TEST_CASE("the move-to-workspace target is independent of the bound key", "[config]") {
    // Super+Shift+1 moves the window to workspace 6 — arg is not derived from the key.
    const auto result = parse("bind = SUPER SHIFT, 1, movetoworkspacesilent, 6\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super | Mod::Shift, Key::N1, Dispatcher::MoveToWorkspaceSilent, 6});
}

TEST_CASE("a movetoworkspace with a malformed number is diagnosed while valid binds still parse", "[config]") {
    const auto result = parse(
        "bind = SUPER, 1, movetoworkspace, two\n"   // malformed N
        "bind = SUPER, 2, movetoworkspace, 2\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);  // collect-and-continue, mirroring workspace
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::N2, Dispatcher::MoveToWorkspace, 2});
}

TEST_CASE("a movetoworkspace with a missing number is diagnosed", "[config]") {
    const auto result = parse("bind = SUPER, 1, movetoworkspacesilent\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
}

TEST_CASE("a focus bind parses to Bind{Focus, dir}, with key and direction independent", "[config]") {
    // The bound KEY (Left arrow) and the DIRECTION word (right) are separate
    // fields — pressing Super+Left here focuses to the right.
    const auto result = parse("bind = SUPER, Left, focus, right\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super, Key::Left, Dispatcher::Focus, 0, config::Direction::Right});
}

TEST_CASE("all four focus directions parse, case-insensitively", "[config]") {
    const auto result = parse(
        "bind = SUPER, H, focus, left\n"
        "bind = SUPER, L, focus, RIGHT\n"    // upper-case
        "bind = SUPER, K, focus, Up\n"       // mixed-case
        "bind = SUPER, J, focus, down\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 4);
    REQUIRE(result.config.binds[0].dir == config::Direction::Left);
    REQUIRE(result.config.binds[1].dir == config::Direction::Right);
    REQUIRE(result.config.binds[2].dir == config::Direction::Up);
    REQUIRE(result.config.binds[3].dir == config::Direction::Down);
}

TEST_CASE("a focus bind with a missing direction is diagnosed while valid binds still load", "[config]") {
    const auto result = parse(
        "bind = SUPER, 1, workspace, 1\n"
        "bind = SUPER, Left, focus\n"       // no direction argument
        "bind = SUPER, 2, workspace, 2\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 2);
    // The two valid binds on the other lines are unaffected.
    REQUIRE(result.config.binds.size() == 2);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::N1, Dispatcher::Workspace, 1});
    REQUIRE(result.config.binds[1] == Bind{Mod::Super, Key::N2, Dispatcher::Workspace, 2});
}

TEST_CASE("a focus bind with an unknown direction is diagnosed per-line", "[config]") {
    const auto result = parse("bind = SUPER, Left, focus, sideways\n");

    REQUIRE(result.config.binds.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
}

TEST_CASE("changing $mod in one place re-modifies every bind that references it", "[config]") {
    const auto result = parse(
        "$mod = SUPER SHIFT\n"
        "bind = $mod, 1, workspace, 1\n"
        "bind = $mod, 2, workspace, 2\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 2);
    for (const auto& b : result.config.binds)
        REQUIRE(contains(b.mods, Mod::Super));
    for (const auto& b : result.config.binds)
        REQUIRE(contains(b.mods, Mod::Shift));
}

// ── windowrule directive ─────────────────────────────────────────────────────

TEST_CASE("each windowrule field kind parses to the right WindowRule", "[config]") {
    const auto result = parse(
        "windowrule = workspace 2, exe:Slack.exe\n"
        "windowrule = workspace 3, class:Chrome_WidgetWin_1\n"
        "windowrule = workspace 4, title:^Term\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 3);

    REQUIRE(result.config.rules[0].field == Field::Exe);
    REQUIRE(result.config.rules[0].workspace == 2);
    REQUIRE(result.config.rules[0].pattern == "slack.exe");  // lowercased

    REQUIRE(result.config.rules[1].field == Field::Class);
    REQUIRE(result.config.rules[1].workspace == 3);
    REQUIRE(result.config.rules[1].pattern == "chrome_widgetwin_1");

    REQUIRE(result.config.rules[2].field == Field::Title);
    REQUIRE(result.config.rules[2].workspace == 4);
    // The compiled regex behaves as authored (anchored substring).
    REQUIRE(std::regex_search(std::string("Terminal"), result.config.rules[2].regex));
    REQUIRE_FALSE(std::regex_search(std::string("xTerminal"), result.config.rules[2].regex));
}

TEST_CASE("the first-comma split keeps a comma inside a title regex", "[config]") {
    // Only the FIRST comma splits action from spec, so a{1,3}'s comma survives.
    const auto result = parse("windowrule = workspace 1, title:a{1,3}\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].field == Field::Title);
    REQUIRE(result.config.rules[0].pattern == "a{1,3}");
}

TEST_CASE("the first-colon split keeps a colon inside a title regex", "[config]") {
    const auto result = parse("windowrule = workspace 1, title:foo:bar\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].pattern == "foo:bar");
}

TEST_CASE("a windowrule with a non-integer workspace is diagnosed", "[config]") {
    const auto result = parse("windowrule = workspace two, exe:slack.exe\n");

    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("an unsupported windowrule action is diagnosed and the file continues", "[config]") {
    // `float` is a Hyprland action winspace does not support (only workspace/ignore).
    const auto result = parse(
        "windowrule = float, exe:foo.exe\n"
        "windowrule = workspace 2, exe:bar.exe\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].pattern == "bar.exe");
}

TEST_CASE("an unknown windowrule field is diagnosed", "[config]") {
    const auto result = parse("windowrule = workspace 2, pid:1234\n");

    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("a windowrule missing the field colon is diagnosed", "[config]") {
    const auto result = parse("windowrule = workspace 2, slack.exe\n");

    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("a windowrule with an empty pattern is diagnosed (exe and title)", "[config]") {
    const auto exe = parse("windowrule = workspace 2, exe:\n");
    REQUIRE(exe.config.rules.empty());
    REQUIRE(exe.diagnostics.size() == 1);

    const auto title = parse("windowrule = workspace 2, title:\n");
    REQUIRE(title.config.rules.empty());
    REQUIRE(title.diagnostics.size() == 1);
}

TEST_CASE("an invalid title regex is diagnosed while the rest of the file parses", "[config]") {
    const auto result = parse(
        "windowrule = workspace 2, title:(unclosed\n"
        "windowrule = workspace 3, exe:ok.exe\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].pattern == "ok.exe");
}

TEST_CASE("exe and class patterns are lowercased and whitespace-trimmed", "[config]") {
    const auto result = parse("windowrule = workspace 2, exe:  SLACK.EXE\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].pattern == "slack.exe");
}

TEST_CASE("one parse returns both binds and rules", "[config]") {
    const auto result = parse(
        "bind = SUPER, 1, workspace, 1\n"
        "windowrule = workspace 2, exe:slack.exe\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.rules.size() == 1);
}

// ── windowrule ignore action ─────────────────────────────────────────────────

TEST_CASE("windowrule ignore parses to an Ignore-action rule for each match field", "[config]") {
    const auto result = parse(
        "windowrule = ignore, exe:Dock.exe\n"
        "windowrule = ignore, class:Shell_TrayWnd\n"
        "windowrule = ignore, title:^Widget\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 3);

    REQUIRE(result.config.rules[0].action == RuleAction::Ignore);
    REQUIRE(result.config.rules[0].field == Field::Exe);
    REQUIRE(result.config.rules[0].pattern == "dock.exe");  // lowercased like a Place exe rule

    REQUIRE(result.config.rules[1].action == RuleAction::Ignore);
    REQUIRE(result.config.rules[1].field == Field::Class);
    REQUIRE(result.config.rules[1].pattern == "shell_traywnd");

    REQUIRE(result.config.rules[2].action == RuleAction::Ignore);
    REQUIRE(result.config.rules[2].field == Field::Title);
    REQUIRE(std::regex_search(std::string("Widget"), result.config.rules[2].regex));
}

TEST_CASE("a workspace windowrule still parses with the Place action", "[config]") {
    // The existing action keeps its tag — Place is not silently turned into Ignore.
    const auto result = parse("windowrule = workspace 2, exe:slack.exe\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].action == RuleAction::Place);
    REQUIRE(result.config.rules[0].workspace == 2);
}

TEST_CASE("a windowrule ignore with a missing or empty pattern is diagnosed", "[config]") {
    // Missing field colon.
    const auto noColon = parse("windowrule = ignore, dock.exe\n");
    REQUIRE(noColon.config.rules.empty());
    REQUIRE(noColon.diagnostics.size() == 1);

    // Empty exe pattern.
    const auto emptyExe = parse("windowrule = ignore, exe:\n");
    REQUIRE(emptyExe.config.rules.empty());
    REQUIRE(emptyExe.diagnostics.size() == 1);

    // Empty title pattern.
    const auto emptyTitle = parse("windowrule = ignore, title:\n");
    REQUIRE(emptyTitle.config.rules.empty());
    REQUIRE(emptyTitle.diagnostics.size() == 1);
}

// ── windowrule spread action retired (ADR-0020) ──────────────────────────────

TEST_CASE("the retired spread action is diagnosed with a distribution-is-automatic message", "[config]") {
    // ADR-0020 removed `spread`: distribution is now automatic for every eligible
    // window. A ported config still carrying it earns a targeted diagnostic (naming
    // automatic distribution) rather than becoming a rule.
    const auto result = parse(
        "windowrule = spread, exe:Term.exe\n"
        "windowrule = workspace 2, exe:bar.exe\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
    REQUIRE(result.diagnostics[0].message.find("automatic") != std::string::npos);
    // No spread rule is produced; the following valid rule still parses.
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].pattern == "bar.exe");
}

// ── windowrule slot suffix (ADR-0016) ────────────────────────────────────────

TEST_CASE("a windowrule with no slot parses to the no-slot state (regression guard)", "[config]") {
    // Story 16: existing configs are unaffected — a slotless workspace rule keeps
    // its exact meaning and its slot is nullopt.
    const auto result = parse("windowrule = workspace 2, exe:slack.exe\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].action == RuleAction::Place);
    REQUIRE(result.config.rules[0].workspace == 2);
    REQUIRE_FALSE(result.config.rules[0].slot.has_value());
}

TEST_CASE("a valid slot suffix parses to the matching Slot", "[config]") {
    const auto result = parse(
        "windowrule = workspace 2 slot right-half, exe:slack.exe\n"
        "windowrule = workspace 1 slot maximized, class:Chrome_WidgetWin_1\n"
        "windowrule = workspace 3 slot top-left-quarter, title:^Term\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 3);

    REQUIRE(result.config.rules[0].workspace == 2);
    REQUIRE(result.config.rules[0].slot == Slot::RightHalf);

    REQUIRE(result.config.rules[1].workspace == 1);
    REQUIRE(result.config.rules[1].slot == Slot::Maximized);

    REQUIRE(result.config.rules[2].workspace == 3);
    REQUIRE(result.config.rules[2].slot == Slot::TopLeftQuarter);
}

TEST_CASE("slot names are matched case-insensitively", "[config]") {
    const auto result = parse("windowrule = workspace 2 slot LEFT-HALF, exe:slack.exe\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].slot == Slot::LeftHalf);
}

TEST_CASE("an unknown slot name is diagnosed and skips only that rule", "[config]") {
    // Story 14: a mistyped slot names the vocabulary and drops just its line — the
    // following valid rule still parses.
    const auto result = parse(
        "windowrule = workspace 2 slot rihgt-half, exe:slack.exe\n"
        "windowrule = workspace 3 slot left-half, exe:code.exe\n");

    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
    REQUIRE(result.diagnostics[0].message.find("rihgt-half") != std::string::npos);
    // Only the good rule survives.
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.rules[0].workspace == 3);
    REQUIRE(result.config.rules[0].slot == Slot::LeftHalf);
}

TEST_CASE("slot without a name is diagnosed", "[config]") {
    const auto result = parse("windowrule = workspace 2 slot, exe:slack.exe\n");
    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
}

TEST_CASE("a token other than slot after the workspace number is diagnosed", "[config]") {
    const auto result = parse("windowrule = workspace 2 zzz right-half, exe:slack.exe\n");
    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].message.find("zzz") != std::string::npos);
}

TEST_CASE("slot on an ignore rule is diagnosed", "[config]") {
    // Story 15: slot is Place-only; on ignore it is a meaningless combination.
    const auto result = parse("windowrule = ignore slot right-half, exe:dock.exe\n");
    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].message.find("ignore") != std::string::npos);
}

// ── start_at_login setting ───────────────────────────────────────────────────

TEST_CASE("start_at_login parses true and false case-insensitively", "[config]") {
    REQUIRE(parse("start_at_login = true\n").config.startAtLogin == true);
    REQUIRE(parse("start_at_login = TRUE\n").config.startAtLogin == true);
    REQUIRE(parse("start_at_login = False\n").config.startAtLogin == false);

    const auto t = parse("start_at_login = true\n");
    REQUIRE(t.diagnostics.empty());
}

TEST_CASE("start_at_login absent defaults to false with no diagnostic", "[config]") {
    const auto result = parse("bind = SUPER, 1, workspace, 1\n");
    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.startAtLogin == false);
}

TEST_CASE("a non-bool start_at_login value is diagnosed", "[config]") {
    const auto result = parse("start_at_login = maybe\n");
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(result.diagnostics[0].line == 1);
    REQUIRE(result.config.startAtLogin == false);
}

// ── reload dispatcher ────────────────────────────────────────────────────────

TEST_CASE("the reload dispatcher parses as a zero-argument bind", "[config]") {
    const auto result = parse("bind = SUPER SHIFT, R, reload\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] ==
            Bind{Mod::Super | Mod::Shift, Key::R, Dispatcher::Reload, 0});
}

// ── tile dispatcher (ADR-0016) ───────────────────────────────────────────────

TEST_CASE("the tile dispatcher parses as a zero-argument bind", "[config]") {
    const auto result = parse("bind = SUPER, T, tile\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.binds[0] == Bind{Mod::Super, Key::T, Dispatcher::Tile, 0});
}

// ── removed-with-tiling diagnostics ──────────────────────────────────────────

TEST_CASE("a bind to each removed tiling dispatcher yields a targeted removed-with-tiling diagnostic", "[config]") {
    for (const std::string disp :
         {"movewindow", "maximize", "resizeactive", "togglefloat", "movetomonitor"}) {
        const auto result = parse("bind = SUPER, X, " + disp + "\n");
        REQUIRE(result.config.binds.empty());
        REQUIRE(result.diagnostics.size() == 1);
        REQUIRE(result.diagnostics[0].message.find("removed with tiling") != std::string::npos);
        REQUIRE(result.diagnostics[0].message.find(disp) != std::string::npos);
    }
}

TEST_CASE("the removed tiling settings yield the same targeted diagnostic", "[config]") {
    for (const std::string setting : {"min_tile_width", "min_tile_height"}) {
        const auto result = parse(setting + " = 100\n");
        REQUIRE(result.diagnostics.size() == 1);
        REQUIRE(result.diagnostics[0].message.find("removed with tiling") != std::string::npos);
        REQUIRE(result.diagnostics[0].message.find(setting) != std::string::npos);
    }
}

TEST_CASE("an unknown name that is NOT a removed tiling one keeps the generic diagnostic", "[config]") {
    // Unknown dispatcher.
    const auto disp = parse("bind = SUPER, X, teleport\n");
    REQUIRE(disp.diagnostics.size() == 1);
    REQUIRE(disp.diagnostics[0].message.find("unknown dispatcher") != std::string::npos);
    REQUIRE(disp.diagnostics[0].message.find("removed with tiling") == std::string::npos);

    // Unknown directive.
    const auto dir = parse("frobnicate = 1\n");
    REQUIRE(dir.diagnostics.size() == 1);
    REQUIRE(dir.diagnostics[0].message.find("unknown directive") != std::string::npos);
    REQUIRE(dir.diagnostics[0].message.find("removed with tiling") == std::string::npos);
}

// ── exec / exec-once directive ───────────────────────────────────────────────

TEST_CASE("exec and exec-once parse into one source-ordered list with the right once flag", "[config]") {
    // Interleaved exec / exec-once lines must preserve global source order in the
    // single tagged list — that order is the launch order.
    const auto result = parse(
        "exec-once = firefox\n"
        "exec = kitty\n"
        "exec-once = code --new-window\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.execs.size() == 3);
    REQUIRE(result.config.execs[0] == ExecEntry{"firefox", true});
    REQUIRE(result.config.execs[1] == ExecEntry{"kitty", false});
    REQUIRE(result.config.execs[2] == ExecEntry{"code --new-window", true});
}

TEST_CASE("the exec command tail is stored verbatim — spaces, quotes, commas, and $ survive", "[config]") {
    // The tail after `=` is verbatim: no field-splitting on commas, no $var
    // expansion (unlike bind). A literal `$` in a path or arg passes through.
    const auto result = parse(
        "exec-once = \"C:\\Program Files\\App\\app.exe\" --a, --b $HOME\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.execs.size() == 1);
    REQUIRE(result.config.execs[0] ==
            ExecEntry{"\"C:\\Program Files\\App\\app.exe\" --a, --b $HOME", true});
}

TEST_CASE("no $var expansion is applied to a launch command even when the var is defined", "[config]") {
    // $mod is a real variable here; a bind would expand it, but exec must not —
    // the `$mod` is passed through literally.
    const auto result = parse(
        "$mod = SUPER\n"
        "exec = run $mod\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.execs.size() == 1);
    REQUIRE(result.config.execs[0] == ExecEntry{"run $mod", false});
}

TEST_CASE("an empty exec or exec-once tail is diagnosed and skipped while valid lines parse", "[config]") {
    const auto result = parse(
        "exec-once = firefox\n"
        "exec =\n"           // empty tail → diagnostic, skipped
        "exec-once =   \n"   // whitespace-only tail → also empty after trim
        "exec = kitty\n");

    REQUIRE(result.diagnostics.size() == 2);
    REQUIRE(result.diagnostics[0].line == 2);
    REQUIRE(result.diagnostics[1].line == 3);
    // The two well-formed entries still parse, in order.
    REQUIRE(result.config.execs.size() == 2);
    REQUIRE(result.config.execs[0] == ExecEntry{"firefox", true});
    REQUIRE(result.config.execs[1] == ExecEntry{"kitty", false});
}

TEST_CASE("one parse returns binds, rules, and execs together", "[config]") {
    const auto result = parse(
        "bind = SUPER, 1, workspace, 1\n"
        "windowrule = workspace 2, exe:firefox.exe\n"
        "exec-once = firefox\n");

    REQUIRE(result.diagnostics.empty());
    REQUIRE(result.config.binds.size() == 1);
    REQUIRE(result.config.rules.size() == 1);
    REQUIRE(result.config.execs.size() == 1);
    REQUIRE(result.config.execs[0] == ExecEntry{"firefox", true});
}

// ─────────────────────────────────────────────────────────────────────────────
// [command tests] command-intent parsing
// ─────────────────────────────────────────────────────────────────────────────

// Tests for the pure command-intent parser (ADR-0019). The argv ->
// {Run|Install|Uninstall|Other} decision is a pure function, so it is proven
// here at the highest point without the VM — command routing is correct before a
// single window or thread exists. The I/O layer's narrowing and dispatch are
// covered by the Installation VM Smoke seam.

TEST_CASE("no arguments is the Run (WM) command", "[command]") {
    REQUIRE(parseCommand({}) == Command::Run);
}

TEST_CASE("the sole token 'install' is the Install command", "[command]") {
    REQUIRE(parseCommand({"install"}) == Command::Install);
}

TEST_CASE("the sole token 'uninstall' is the Uninstall command", "[command]") {
    REQUIRE(parseCommand({"uninstall"}) == Command::Uninstall);
}

TEST_CASE("an unknown single verb is Other, never Run", "[command]") {
    REQUIRE(parseCommand({"start"}) == Command::Other);
    REQUIRE(parseCommand({"reload"}) == Command::Other);
    REQUIRE(parseCommand({"instal"}) == Command::Other);  // a typo must not fall through to Run
}

TEST_CASE("install/uninstall with a stray extra argument is Other, not the command", "[command]") {
    REQUIRE(parseCommand({"install", "now"}) == Command::Other);
    REQUIRE(parseCommand({"uninstall", "--force"}) == Command::Other);
}

TEST_CASE("the verbs are matched case-sensitively and exactly", "[command]") {
    // The Scoop hooks invoke the lowercase verbs verbatim; anything else is Other.
    REQUIRE(parseCommand({"Install"}) == Command::Other);
    REQUIRE(parseCommand({"INSTALL"}) == Command::Other);
    REQUIRE(parseCommand({"install "}) == Command::Other);
}
