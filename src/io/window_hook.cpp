// Window-hook adapter — I/O adapter, sole owner of the OS accessibility stream.
//
// Turns the WinEvent stream into the pure-core lifecycle Events (Appeared /
// Vanished) and the monitor topology into MonitorsChanged. It is the Probe side
// of the probe/decide seam (ADR-0006): the hook says *when* a window crosses a
// lifecycle edge, and a synchronous Probe here says *what* the window is, into a
// plain-data WindowAttrs the pure Reducer then classifies. Including <windows.h>
// keeps this in the app TU only, so the parser and Reducer it depends on stay
// linker-provably pure.
//
// Its own third thread, modeled on the Hotkey thread: an out-of-context
// SetWinEventHook binds to the calling thread's queue and its callback is invoked
// while this thread pumps GetMessage, so the hook must live on the thread that
// runs the loop, and UnhookWinEvent must fire on that same thread at teardown.
//
//   Window-hook thread ──PostMessage(Event*)──▶ Worker's message-only HWND
#pragma once

#include <windows.h>

#include <dwmapi.h>  // DwmGetWindowAttribute / DWMWA_CLOAKED — the cloak probe

#include <future>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <cstdint>

#include "io/error.cpp"          // io::Error vocabulary (ok() wrappers) + lg:: levels
#include "io/worker.cpp"         // postEvent — the Event* ownership transport
#include "winspace/reducer.cpp"  // WindowId, MonitorId, WindowAttrs, Appeared, …

namespace winspace::io {

namespace hook_detail {

// The out-of-context callback carries no user pointer, so the one thing it needs
// — the Worker HWND to post Events to — is stashed here by the thread entry
// before the hook is armed, and read on each callback. thread_local because the
// callback runs on (and only on) the hooking thread while it pumps GetMessage.
inline thread_local HWND g_workerHwnd = nullptr;

// Mint the opaque core identities from their OS handles. The core never does
// arithmetic on these — it stores, compares, and hands them back on Effects — so
// the raw pointer bits are a fine, stable identity for a window's lifetime.
inline WindowId windowId(HWND h) {
    return static_cast<WindowId>(reinterpret_cast<uintptr_t>(h));
}
inline MonitorId monitorId(HMONITOR m) {
    return static_cast<MonitorId>(reinterpret_cast<uintptr_t>(m));
}

inline Rect toRect(const RECT& r) { return Rect{r.left, r.top, r.right, r.bottom}; }

// The Probe: one synchronous read of a single window's live attributes into the
// plain-data WindowAttrs the Reducer decides over. Runs on the hook thread at
// event time (reactive, never on a timer). Every read degrades to a safe default
// on failure — a probe fires constantly, so a transient read error leaves the
// window classified as it stood, never crashes the stream.
inline WindowAttrs probeWindow(HWND hwnd) {
    WindowAttrs a{};
    a.id = windowId(hwnd);

    // Styles / ex-styles — the WS_* eligibility bits, read verbatim.
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    a.visible = (style & WS_VISIBLE) != 0;
    a.thickFrame = (style & WS_THICKFRAME) != 0;
    a.caption = (style & WS_CAPTION) != 0;
    a.toolWindow = (exStyle & WS_EX_TOOLWINDOW) != 0;

    // Top-level AND unowned: it is its own root window and has no owner (owned
    // dialogs/popups are Ineligible). GA_ROOT walks to the top-level ancestor;
    // GW_OWNER is null only for an unowned window.
    a.topLevel = GetAncestor(hwnd, GA_ROOT) == hwnd && GetWindow(hwnd, GW_OWNER) == nullptr;

    // DWM cloak state — how a UWP host or a virtual-desktop-parked window reads
    // as "hidden" while still being WS_VISIBLE. A failed read means not-cloaked.
    DWORD cloaked = 0;
    if (ok(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        a.cloaked = cloaked != 0;

    // Monitor the window sits on, and fullscreen by rect-vs-monitor: a window
    // whose bounds cover its monitor's full rect is already fullscreen and drops
    // out of tiling. A failed rect read leaves fullscreen false.
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    a.monitor = monitorId(mon);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT rc{};
    if (ok(GetWindowRect(hwnd, &rc)) && ok(GetMonitorInfoW(mon, &mi))) {
        a.fullscreen = rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                       rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom;
    }
    return a;
}

// The win-event callback (out-of-context: invoked on the hook thread as it pumps
// GetMessage). First-pass noise gate drops everything that is not a real window
// object — carets, menus, scrollbars, list items all arrive on this same stream.
// Then the lifecycle edge maps to an Event: SHOW / UNCLOAKED probe-and-Appeared,
// DESTROY / HIDE / CLOAKED Vanished (identity alone). Raw CREATE is dropped — the
// window is half-born there (styles/visibility not final) and misclassifies.
inline void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    switch (event) {
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            postEvent(g_workerHwnd, new Event{Appeared{probeWindow(hwnd)}});
            break;
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
            postEvent(g_workerHwnd, new Event{Vanished{windowId(hwnd)}});
            break;
        default:
            break;  // CREATE (and any other event in range) — dropped
    }
}

// EnumWindows callback: post a synthetic Appeared for each currently-shown
// top-level window, down the exact same path a live SHOW takes (probe → Appeared),
// so the Reducer's Eligibility gate classifies adopted windows identically to
// ones that appear later. lParam carries the Worker HWND. EnumWindows visits only
// top-level windows; IsWindowVisible narrows to the shown ones.
inline BOOL CALLBACK adoptWindow(HWND hwnd, LPARAM lparam) {
    const HWND worker = reinterpret_cast<HWND>(lparam);
    if (IsWindowVisible(hwnd)) postEvent(worker, new Event{Appeared{probeWindow(hwnd)}});
    return TRUE;
}

// EnumDisplayMonitors callback: accumulate one MonitorInfo (identity + work area)
// per monitor into the vector handed through lParam. A monitor whose info can't
// be read is skipped rather than aborting the sweep.
inline BOOL CALLBACK collectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (ok(GetMonitorInfoW(mon, &mi)))
        out->push_back(MonitorInfo{monitorId(mon), toRect(mi.rcWork)});
    return TRUE;
}

// Startup Adoption: inherit the running session so winspace tiles what is already
// open. The monitor sweep goes FIRST — the Reducer's head-fill layout resolves a
// window's rcWork against State.monitors, so MonitorsChanged must land before the
// Appeared events that depend on it, or the adopted head would place nowhere until
// the next re-tile.
inline void adopt(HWND workerHwnd) {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    postEvent(workerHwnd, new Event{MonitorsChanged{std::move(monitors)}});

    EnumWindows(&adoptWindow, reinterpret_cast<LPARAM>(workerHwnd));
}

}  // namespace hook_detail

// Window-hook thread entry. Publishes its thread id (so the spine can unwind it
// with a WM_QUIT), arms one out-of-context hook over the whole object-event range,
// runs the Adoption sweep, then pumps: each delivered win-event runs winEventProc
// inline, which posts the mapped Event to the Worker. On WM_QUIT it unhooks on
// this same thread (no dangling system hook) and returns.
inline void windowHookThreadMain(HWND workerHwnd, std::promise<DWORD> tidReady) {
    MSG msg;
    // Force this thread's message queue into existence before publishing the id,
    // so the spine's later PostThreadMessage can't race queue creation.
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    // The callback reaches the Worker HWND through this thread_local; set before
    // arming the hook so no delivered event can find it null.
    hook_detail::g_workerHwnd = workerHwnd;

    // One hook over EVENT_OBJECT_CREATE … EVENT_OBJECT_UNCLOAKED — the whole
    // object-event range covering SHOW/HIDE/DESTROY/CLOAKED/UNCLOAKED. Out of
    // context (delivered to this thread's queue, dispatched from GetMessage) and
    // skipping our own windowless process's events. The unique_ptr owns the
    // handle; when this frame unwinds its deleter UnhookWinEvents on this same
    // thread AND clears the thread_local the callback reached through. A null
    // handle (a failed SetWinEventHook) owns nothing, so the deleter never fires.
    const std::unique_ptr<std::remove_pointer_t<HWINEVENTHOOK>,
                          decltype([](HWINEVENTHOOK h) {
                              UnhookWinEvent(h);
                              hook_detail::g_workerHwnd = nullptr;
                          })>
        hook(SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_UNCLOAKED, nullptr,
                             &hook_detail::winEventProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
    if (const auto armed = ok(static_cast<BOOL>(hook != nullptr)); !armed) {
        // A failed hook means no live window tracking — loud, but not fatal: the
        // Adoption sweep below still seeds State from the current session.
        lg::error("window hook: SetWinEventHook failed — live window tracking disabled: {}",
                  armed.error());
    }

    // Inherit the current session (monitors, then existing windows).
    hook_detail::adopt(workerHwnd);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // `hook` unwinds here: its deleter UnhookWinEvents on the same thread that
    // armed it (so no system hook outlives us) and clears g_workerHwnd.
}

}  // namespace winspace::io
