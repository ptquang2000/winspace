// Window-hook adapter — I/O adapter (owns <windows.h>). The SetWinEventHook
// lifecycle stream behind window rules (PRD 06), reintroduced on its OWN
// dedicated thread when this slice became the hook's first genuine consumer
// (ADR-0007 removed it with tiling). It mirrors the Hotkey thread: it owns no
// State and makes no decision — its callback runs the noise gate, Probes the
// window, and posts an Appeared / Vanished Event to the Worker, which reduces it.
//
// Kept off the Worker thread so hook delivery never queues behind a blocking
// Effect (a SwitchDesktop or a move round-trip). Two tight SetWinEventHook ranges
// share one WINEVENTPROC, both WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS.
// The callback reaches the Worker HWND through a thread_local (a WINEVENTPROC
// takes no user context), so it must be set on this thread before the hooks fire.
#pragma once

#include <windows.h>

#include <future>
#include <utility>

#include "io/probe.cpp"          // probeWindow, probeIdentity, toWindowId
#include "io/worker.cpp"         // postEvent, k_wmEvent transport
#include "winspace/reducer.cpp"  // Appeared, Vanished, Event

namespace winspace::io {

// The Worker HWND the callback posts to. A WINEVENTPROC carries no user pointer,
// so the target is stashed thread-locally on the hook thread before registration.
inline thread_local HWND t_hookWorkerHwnd = nullptr;

// One callback for both ranges. The noise gate keeps only genuine top-level
// window events (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd):
// SHOW / UNCLOAKED → Appeared (probe BOTH attrs and identity); DESTROY / HIDE /
// CLOAKED → Vanished (id only — never probe a possibly-dead HWND).
inline void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    const HWND worker = t_hookWorkerHwnd;
    if (!worker) return;

    switch (event) {
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            postEvent(worker, new Event{Appeared{probeWindow(hwnd), probeIdentity(hwnd)}});
            break;
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
            postEvent(worker, new Event{Vanished{toWindowId(hwnd)}});
            break;
        default:
            break;  // an event inside a range we don't act on — ignore
    }
}

// Hook thread entry. Mirrors hotkeyThreadMain: force the message queue into
// existence, publish the thread id (so the spine can WM_QUIT it), then register
// the hooks and run. Register-then-enumerate closes the create-in-the-gap race —
// the hooks are live before EnumWindows posts a synthetic Appeared per top-level
// window (startup adoption); double-sightings are deduped for free by `placed`.
// On WM_QUIT the loop returns and both hooks are unhooked on THIS thread (the
// only thread allowed to unhook them), so quit leaves no dangling hook.
inline void hookThreadMain(HWND workerHwnd, std::promise<DWORD> tidReady) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    t_hookWorkerHwnd = workerHwnd;

    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const HWINEVENTHOOK showHide = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, winEventProc, 0, 0, flags);
    const HWINEVENTHOOK cloak = SetWinEventHook(
        EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, winEventProc, 0, 0, flags);
    if (!showHide || !cloak)
        lg::error("window hook: SetWinEventHook failed — window rules disabled");

    // Startup adoption: post a synthetic Appeared for every top-level window,
    // through the same path a live SHOW takes (EnumWindows never sees winspace's
    // own message-only HWND).
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            postEvent(reinterpret_cast<HWND>(lp),
                      new Event{Appeared{probeWindow(h), probeIdentity(h)}});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(workerHwnd));

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (showHide) UnhookWinEvent(showHide);
    if (cloak) UnhookWinEvent(cloak);
}

}  // namespace winspace::io
