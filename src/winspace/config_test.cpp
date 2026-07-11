// Tests for the pure config parser (Seam 2). Included by the test TU only, so
// it links no WM import libraries — the parser under test is provably pure.
#pragma once

#include "catch2/catch_amalgamated.hpp"
#include "winspace/config.cpp"

using namespace winspace;

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

// ── windowrule directive (PRD 06) ────────────────────────────────────────────

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
    const auto result = parse(
        "windowrule = ignore, exe:foo.exe\n"
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
