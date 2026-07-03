// Test translation unit — unity build.
//
// The single source file that becomes winspace_tests.exe. It aggregates the
// pure core ONLY, plus vendored Catch2, into one cl.exe invocation, and is
// built /SUBSYSTEM:CONSOLE. It links NONE of the WM import libraries.
//
// That last fact is the whole point of splitting the harness this way: because
// this TU links no user32/ole32/dwmapi/…, any accidental OS call reachable from
// core becomes a *link* error here, not a runtime surprise in the app. The
// linker enforces "core touches no Windows API" — discipline is not required.
//
// Catch2 is amalgamated and #included (header + implementation) so this stays a
// single translation unit — keeping compile_commands.json to exactly two
// entries. The push(0)/pop wrapper silences third-party warnings so our /W4 /WX
// applies only to winspace code.
#pragma warning(push, 0)
#include "catch2/catch_amalgamated.hpp"
#include "catch2/catch_amalgamated.cpp"  // brings the library + default main()
#pragma warning(pop)

// ── core under test (pure, no <windows.h>) ──────────────────────────────────
// Populated by tasks 02 (config parser) and 03 (reducer). Each core source is
// pulled in through its test file, which #includes the source under test:
#include "winspace/config_test.cpp"
#include "winspace/reducer_test.cpp"

TEST_CASE("build harness compiles, links, and runs a pure test", "[harness]") {
    REQUIRE(1 + 1 == 2);
}
