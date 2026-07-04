// Process spine — I/O adapter (owns <windows.h> and thread lifecycle).
//
// The backbone the whole product plugs into: it spawns the Worker thread and the
// Hotkey thread, wires the Hotkey→Worker transport, and joins both on a clean
// exit. No domain logic lives here — behavior is the Reducer's, executed by the
// Worker. This file carries only the two-thread wiring.
//
//   Hotkey thread ──PostMessage(Event*)──▶ Worker's message-only HWND
//                                          Worker: reduce → execute Effects
//                                          Exit Effect → PostQuitMessage → unwind
#pragma once

#include <windows.h>

#include <future>
#include <thread>

#include "io/worker.cpp"
#include "winspace/reducer.cpp"

namespace winspace::io {

// TEMPORARY (task 04): with no hotkeys registered yet, opt into a one-shot
// self-test that drives a Quit through the real transport so the clean-exit
// spine is provable end-to-end. Set WINSPACE_SELFTEST_QUIT to exercise it; leave
// it unset to run windowless indefinitely (the manual "visible only in Task
// Manager" check). Removed once task 05 supplies real hotkey-driven Events.
inline bool selftestQuitEnabled() {
    return GetEnvironmentVariableW(L"WINSPACE_SELFTEST_QUIT", nullptr, 0) != 0;
}

// Hotkey thread entry. Owns its own GetMessage loop, ready to receive WM_HOTKEY
// (RegisterHotKey wiring is task 05). It publishes its thread id so the spine can
// unwind the loop with a WM_QUIT, then pumps until then.
inline void hotkeyThreadMain(HWND workerHwnd, std::promise<DWORD> tidReady) {
    MSG msg;
    // Force this thread's message queue into existence before publishing the id,
    // so the spine's later PostThreadMessage can't race queue creation.
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    // task 05: RegisterHotKey each parsed Bind here; the loop below then
    // translates WM_HOTKEY → Event and calls postEvent(workerHwnd, ev).

    if (selftestQuitEnabled()) {
        postEvent(workerHwnd, new Event{Quit{}});
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Bring up both threads, run until Exit, then tear down cleanly. Returns the
// process exit code. Called by wWinMain.
inline int runApp() {
    // Start the Worker and wait for its message-only HWND before anyone posts.
    std::promise<HWND> hwndReady;
    std::future<HWND> hwndFuture = hwndReady.get_future();
    std::thread workerThread(workerThreadMain, std::move(hwndReady));

    const HWND workerHwnd = hwndFuture.get();
    if (!workerHwnd) {
        workerThread.join();
        return 1;  // Worker could not create its window — nothing can proceed
    }

    // Start the Hotkey thread once the Worker HWND exists to post to.
    std::promise<DWORD> tidReady;
    std::future<DWORD> tidFuture = tidReady.get_future();
    std::thread hotkeyThread(hotkeyThreadMain, workerHwnd, std::move(tidReady));
    const DWORD hotkeyTid = tidFuture.get();

    // Block until an Exit Effect posts WM_QUIT to the Worker and its loop returns.
    workerThread.join();

    // Worker is down; unwind the Hotkey thread's loop. This WM_QUIT is
    // thread-lifecycle control — NOT the Event transport, which is always a
    // PostMessage to the Worker's HWND.
    PostThreadMessageW(hotkeyTid, WM_QUIT, 0, 0);
    hotkeyThread.join();
    return 0;
}

}  // namespace winspace::io
