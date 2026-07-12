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

    const auto miss = reduce(s, appeared(WindowId{2}, "slack.exe.bak", "cls", "t"));
    REQUIRE(miss.effects.empty());
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

TEST_CASE("an eligible non-matching Appeared is placed and never re-evaluated", "[reducer][rules]") {
    const State s = stateWith({exeRule(2, "slack.exe")});

    const auto first = reduce(s, appeared(WindowId{7}, "notepad.exe", "cls", "t"));
    REQUIRE(first.effects.empty());

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
    // title:^Slack$ does NOT match "Slack | general" (anchored whole-title).
    {
        const auto r = reduce(stateWith({titleRule(3, "^Slack$")}),
                              appeared(WindowId{2}, "e", "c", "Slack | general"));
        REQUIRE(r.effects.empty());
    }
    // title:slack does NOT match "Slack" (case-sensitive).
    {
        const auto r = reduce(stateWith({titleRule(3, "slack")}),
                              appeared(WindowId{3}, "e", "c", "Slack"));
        REQUIRE(r.effects.empty());
    }
}

TEST_CASE("an Appeared with no rules seeded emits nothing but still places the id", "[reducer][rules]") {
    // A default State has a null rules handle; matching must be skipped safely.
    const auto first = reduce(State{}, appeared(WindowId{1}, "slack.exe", "cls", "Slack"));
    REQUIRE(first.effects.empty());
    const auto second = reduce(first.state, appeared(WindowId{1}, "slack.exe", "cls", "Slack"));
    REQUIRE(second.effects.empty());
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
