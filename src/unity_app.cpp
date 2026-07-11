// App translation unit — unity build.
//
// The single source file that becomes winspace.exe. It aggregates, in order,
// the pure core, the I/O adapters, and the process entry point into one cl.exe
// invocation. It links the WM import libraries and is built /SUBSYSTEM:WINDOWS
// (windowless — no console, no taskbar button, no Alt-Tab entry).
//
// Later tasks slot their sources into the two marked sections below; the entry
// point is the only thing this file carries today.

// ── core (pure, no <windows.h>) ─────────────────────────────────────────────
// Populated by tasks 02 (config parser) and 03 (reducer):
#include "winspace/config.cpp"
#include "winspace/reducer.cpp"

// ── I/O adapters (own all <windows.h> / COM) ────────────────────────────────
// Task 04 landed io/worker.cpp + io/app.cpp; task 05 landed io/hotkeys.cpp
// (pulled in by io/app.cpp); task 06 landed io/vd_bridge.cpp (pulled in by
// io/worker.cpp, which owns the bridge on its STA thread). issue 05 landed
// io/probe.cpp — the reactive window sweep behind the `focus` dispatcher (pulled
// in by io/worker.cpp). io/window_hook.cpp is the SetWinEventHook lifecycle
// adapter PRD 06 reintroduced (removed with tiling, ADR-0007) — pulled in by
// io/app.cpp, which spawns its thread. io/error.cpp holds the shared error
// vocabulary + diagnostic sink, so it precedes the adapters.
#include "io/error.cpp"
#include "io/hotkeys.cpp"
#include "io/probe.cpp"
#include "io/vd_bridge.cpp"
#include "io/window_hook.cpp"
#include "io/worker.cpp"
#include "io/app.cpp"

// ── entry point ─────────────────────────────────────────────────────────────
#include <windows.h>

// Windowless entry. Defining wWinMain makes the linker select the Unicode
// windows CRT startup (wWinMainCRTStartup) automatically; /SUBSYSTEM:WINDOWS
// gives us no console. Parameter names are omitted so /W4 /WX doesn't flag them
// as unreferenced. All wiring lives in the I/O spine (io/app.cpp).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return winspace::io::runApp();
}
