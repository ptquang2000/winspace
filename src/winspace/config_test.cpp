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
