// App translation unit — unity build.
//
// The single source file that becomes winspace.exe. It aggregates, in order,
// the pure core, the I/O adapters, and the process entry point into one cl.exe
// invocation. It links the WM import libraries and is built /SUBSYSTEM:WINDOWS
// (windowless — no console, no taskbar button, no Alt-Tab entry).
//
// The two marked sections below aggregate the pure core and the I/O adapters;
// the entry point follows them.

// ── core (pure, no <windows.h>) ─────────────────────────────────────────────
// The pure core: the config parser and the reducer.
#include "winspace/config.cpp"
#include "winspace/reducer.cpp"

// ── I/O adapters (own all <windows.h> / COM) ────────────────────────────────
// io/worker.cpp + io/app.cpp are the process spine. io/hotkeys.cpp is pulled in
// by io/app.cpp; io/vd_bridge.cpp is pulled in by io/worker.cpp, which owns the
// bridge on its STA thread. io/probe.cpp is the reactive window sweep behind the
// `focus` dispatcher, pulled in by io/worker.cpp. io/window_hook.cpp is the
// SetWinEventHook lifecycle adapter, pulled in by io/app.cpp, which spawns its
// thread. io/error.cpp holds the shared error vocabulary + diagnostic sink, so
// it precedes the adapters.
#include "io/error.cpp"
#include "io/autostart.cpp"
#include "io/config_io.cpp"
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
