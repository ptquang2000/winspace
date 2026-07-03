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
//   #include "winspace/reducer.cpp"

// ── I/O adapters (own all <windows.h> / COM) ────────────────────────────────
// Populated by tasks 04 (worker + threads), 05 (hotkey adapter), 06 (VD bridge):
//   #include "io/worker.cpp"
//   #include "io/hotkeys.cpp"
//   #include "io/vd_bridge.cpp"

// ── entry point ─────────────────────────────────────────────────────────────
#include <windows.h>

// Windowless no-op. Defining wWinMain makes the linker select the Unicode
// windows CRT startup (wWinMainCRTStartup) automatically. Parameter names are
// omitted so /W4 /WX doesn't flag them as unreferenced. The real Hotkey→Worker
// spine lands in task 04.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return 0;
}
