// Process spine — I/O adapter (owns <windows.h> and thread lifecycle).
//
// The backbone the whole product plugs into: it spawns the Worker, Hotkey, and Hook
// threads, wires the producer→Worker transport, and tears all three down on a clean
// exit. No domain logic lives here — behavior is the Reducer's, executed by the
// Worker. This file carries only the thread wiring.
//
//   Hotkey / Hook thread ─PostMessage(Event*)─▶ Worker's message-only HWND
//                                               Worker: reduce → execute Effects
//                                               Exit Effect → PostQuitMessage → unwind
#pragma once

#include <windows.h>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "io/config_io.cpp"  // config load/parse helpers + LoadedConfig
#include "io/hotkeys.cpp"
#include "io/window_hook.cpp"
#include "io/worker.cpp"
#include "winspace/config.cpp"
#include "winspace/reducer.cpp"

namespace winspace::io {

// Load the config for STARTUP. Shares the read+parse mechanism with the Worker's
// reload path (io/config_io.cpp: readAndParseConfig) but passes SeedPolicy::Seed so
// a first run creates the file, and layers startup's own fallback on top: if
// %USERPROFILE% is unset or the file is unreadable, fall back to the built-in
// default text. Startup degrades PER-LINE (each diagnostic is logged and the good
// lines still apply) because its only fallback is the destructive built-in default
// — the deliberate asymmetry with reload's atomic keep-last-good (ADR-0012).
inline LoadedConfig loadConfig() {
    const std::optional<std::filesystem::path> path = configPath();
    if (!path)
        lg::warn("config: %USERPROFILE% is unset; using built-in defaults");
    ConfigReadResult load =
        path ? readAndParseConfig(*path, SeedPolicy::Seed) : ConfigReadResult{};
    ParseResult parsed =
        load.read ? std::move(load.parsed) : parse(std::string(k_defaultConfig));
    if (path && !load.read)
        lg::warn("config: file unreadable at startup; using built-in defaults");
    for (const Diagnostic& d : parsed.diagnostics)
        lg::warn("config line {}: {}", d.line, d.message);
    return toLoaded(std::move(parsed.config));
}

// A non-interactive exit path: setting WINSPACE_SELFTEST_QUIT drives one Quit
// through the real transport so clean shutdown is provable without a live desktop.
// Unset (the default), winspace runs windowless until a bound `quit` hotkey.
inline bool selftestQuitEnabled() {
    return GetEnvironmentVariableW(L"WINSPACE_SELFTEST_QUIT", nullptr, 0) != 0;
}

// Hotkey thread entry. Registers the Binds on THIS thread (so WM_HOTKEY lands in
// its queue), publishes its thread id so the spine can unwind it with a WM_QUIT,
// then pumps: each WM_HOTKEY becomes an Event posted to the Worker.
inline void hotkeyThreadMain(HWND workerHwnd, const std::vector<Bind>& binds,
                             std::promise<DWORD> tidReady) {
    MSG msg;
    // Force this thread's message queue into existence before publishing the id,
    // so the spine's later PostThreadMessage can't race queue creation.
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    // The live table, held in an optional so a reload can tear it down (destroying
    // it unregisters every hotkey on THIS thread) BEFORE building the new one —
    // registering the new set first would collide with a still-registered old combo
    // (ERROR_HOTKEY_ALREADY_REGISTERED) and silently drop the re-bound hotkey.
    std::optional<HotkeyTable> hotkeys;
    hotkeys.emplace(binds);

    if (selftestQuitEnabled()) {
        postEvent(workerHwnd, new Event{Quit{}});
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // A hotkey has a null window, so it never dispatches to a WndProc — handle
        // it here. wParam is the hotkey id, i.e. the Bind's index in the table.
        if (msg.message == WM_HOTKEY) {
            if (!hotkeys)
                continue;
            if (auto ev = hotkeys->eventFor(static_cast<int>(msg.wParam)))
                postEvent(workerHwnd, new Event{std::move(*ev)});
            continue;
        }
        // A live reload: the Worker handed us a heap-allocated Bind
        // vector to re-register. Take ownership, drop the old table (unregister-all),
        // then build the new one (register-all) — in that order (see above).
        if (msg.message == k_wmReloadBinds) {
            const std::unique_ptr<std::vector<Bind>> newBinds(
                reinterpret_cast<std::vector<Bind>*>(msg.lParam));
            hotkeys.reset();
            hotkeys.emplace(*newBinds);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Bring up all three threads, run until Exit, then tear down cleanly. Returns the
// process exit code. Called by wWinMain.
inline int runApp() {
    // Per-Monitor-V2 DPI awareness, set before any window exists (the Worker's
    // message-only HWND included), so it leads runApp. This puts the process and
    // every window rect in ONE physical-pixel coordinate space — the precondition
    // for correct spatial-focus resolution across scaled displays. Best
    // effort: a failure (an OS predating V2, or awareness already pinned by a
    // manifest) leaves the prior awareness in place and is logged, never fatal.
    if (const auto aware =
            ok(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        !aware) {
        lg::warn("app: Per-Monitor-V2 DPI awareness not set: {}", aware.error());
    }

    // One parse yields all three halves. The Binds are owned here and passed to the
    // Hotkey thread by const reference (this declaration outlives hotkeyThread, which
    // is torn down at scope exit); the WindowRules and the launch entries are moved
    // into the Worker thread, which seeds both into State before publishing its HWND.
    LoadedConfig cfg = loadConfig();
    const std::vector<Bind> binds = std::move(cfg.binds);
    const bool startAtLogin = cfg.startAtLogin;

    // Start the Worker and wait for its message-only HWND before anyone posts. The
    // rules, execs, and start_at_login flag are seeded in the Worker ctor, so they
    // precede any Appeared and the Started{} below.
    std::promise<HWND> hwndReady;
    std::future<HWND> hwndFuture = hwndReady.get_future();
    std::jthread workerThread(workerThreadMain, std::move(hwndReady), std::move(cfg.rules),
                              std::move(cfg.execs), startAtLogin);

    const HWND workerHwnd = hwndFuture.get();
    if (!workerHwnd) {
        workerThread.join();
        return 1;  // Worker could not create its window — nothing can proceed
    }

    // The Worker HWND exists and its execs are seeded: post Started{} so the launch
    // entries fire. Done before the hotkey/hook threads wire up — ordering
    // vs. the hook thread is not correctness-critical, since the
    // register-then-EnumWindows adoption places a launched window whether it appears
    // before or after the hooks register.
    postEvent(workerHwnd, new Event{Started{}});

    // Start the Hotkey thread once the Worker HWND exists to post to. It registers
    // the Binds as OS hotkeys on its own thread.
    std::promise<DWORD> tidReady;
    std::future<DWORD> tidFuture = tidReady.get_future();
    auto* rawHotkeyThread = new std::jthread(hotkeyThreadMain, workerHwnd, std::cref(binds),
                                             std::move(tidReady));
    const DWORD hotkeyTid = tidFuture.get();
    auto quitHotkey = [hotkeyTid](std::jthread* t) {
        PostThreadMessageW(hotkeyTid, WM_QUIT, 0, 0);
        delete t;
    };
    std::unique_ptr<std::jthread, decltype(quitHotkey)> hotkeyThread(rawHotkeyThread,
                                                                     std::move(quitHotkey));

    // Tell the Worker the Hotkey thread's id so a live `reload` can hand that thread
    // the freshly-parsed Binds to re-register (ADR-0012). Posted after the
    // id is known — the Worker was constructed before this thread existed.
    PostMessageW(workerHwnd, k_wmSetHotkeyThread, static_cast<WPARAM>(hotkeyTid), 0);

    // Start the Hook thread (third jthread, mirroring the Hotkey thread). It owns
    // the SetWinEventHook adapter, registers its hooks then runs startup adoption,
    // and posts Appeared / Vanished Events to the Worker.
    std::promise<DWORD> hookTidReady;
    std::future<DWORD> hookTidFuture = hookTidReady.get_future();
    auto* rawHookThread = new std::jthread(hookThreadMain, workerHwnd, std::move(hookTidReady));
    const DWORD hookTid = hookTidFuture.get();
    auto quitHook = [hookTid](std::jthread* t) {
        PostThreadMessageW(hookTid, WM_QUIT, 0, 0);
        delete t;
    };
    std::unique_ptr<std::jthread, decltype(quitHook)> hookThread(rawHookThread,
                                                                 std::move(quitHook));

    // Block until an Exit Effect posts WM_QUIT to the Worker and its loop returns.
    workerThread.join();
    return 0;
}

}  // namespace winspace::io
