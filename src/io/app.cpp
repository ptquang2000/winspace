// Process spine — I/O adapter (owns <windows.h> and thread lifecycle).
//
// The backbone the whole product plugs into: it spawns the Worker thread and the
// Hotkey thread, wires the producer→Worker transport, and joins both on a clean
// exit. No domain logic lives here — behavior is the Reducer's, executed by the
// Worker. This file carries only the thread wiring.
//
//   Hotkey thread ──PostMessage(Event*)──▶ Worker's message-only HWND
//                                          Worker: reduce → execute Effects
//                                          Exit Effect → PostQuitMessage → unwind
//
// (The Window-hook thread was removed with tiling — ADR-0007. PRD 06 reintroduces
// it as the first genuine consumer of the lifecycle event stream.)
#pragma once

#include <windows.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "io/hotkeys.cpp"
#include "io/window_hook.cpp"
#include "io/worker.cpp"
#include "winspace/config.cpp"
#include "winspace/reducer.cpp"

namespace winspace::io {

// The config winspace seeds on first run and falls back to when the on-disk file
// is unreadable. It is the Hyprland-subset grammar this slice supports; a user who
// edits the seeded file grows it from here. (Through task 05 this same literal was
// the only source; task 07 promoted it to the default and added the file read.)
inline constexpr std::string_view k_defaultConfig =
    "# winspace config — edit and save; winspace reads this at startup.\n"
    "# $mod = SUPER binds the Windows key. Win+<key> only registers when the\n"
    "# NoWinKeys policy is on (HKCU\\...\\Policies\\Explorer\\NoWinKeys=1); without\n"
    "# it Windows reserves these chords and winspace skips-and-logs each bind.\n"
    "$mod = SUPER\n"
    "bind = $mod, 1, workspace, 1\n"
    "bind = $mod, 2, workspace, 2\n"
    "bind = $mod, 3, workspace, 3\n"
    "bind = $mod, 4, workspace, 4\n"
    "bind = $mod, 5, workspace, 5\n"
    "bind = $mod, Q, quit\n"
    "# Spatial focus: vim-style Alt + h/j/k/l steer the keyboard to the nearest window\n"
    "# in a direction. Alt (not $mod) because Windows reserves the whole Win+h/j/k/l set\n"
    "# even under NoWinKeys (Win+J is Explorer's, Win+K/L are system-reserved), while\n"
    "# Alt+h/j/k/l registers cleanly. The bound key and the direction are independent\n"
    "# fields, so rebind freely (e.g. to the arrow keys).\n"
    "bind = ALT, H, focus, left\n"
    "bind = ALT, J, focus, down\n"
    "bind = ALT, K, focus, up\n"
    "bind = ALT, L, focus, right\n"
    "# Move the focused window to a workspace (Super+Shift+<n>, the hyprland idiom).\n"
    "# `movetoworkspacesilent` moves it but leaves you where you are; the plain\n"
    "# `movetoworkspace` would also follow the window (switch the active desktop to N).\n"
    "# A cross-desktop move is cloaked (DWM) around the move so it never flashes here.\n"
    "bind = $mod SHIFT, 1, movetoworkspacesilent, 1\n"
    "bind = $mod SHIFT, 2, movetoworkspacesilent, 2\n"
    "bind = $mod SHIFT, 3, movetoworkspacesilent, 3\n"
    "bind = $mod SHIFT, 4, movetoworkspacesilent, 4\n"
    "bind = $mod SHIFT, 5, movetoworkspacesilent, 5\n";

// Read an environment variable with the size-then-fill two-call pattern.
// GetEnvironmentVariableW returns the length INCLUDING the null when probing with
// a null buffer, EXCLUDING it when filling — so allocate n and fill n. Empty
// optional means the variable is unset (or unreadable): there is no home to anchor
// the config path to.
inline std::optional<std::wstring> envVar(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n - 1, L'\0');  // n counts the terminator; string owns size()+1
    GetEnvironmentVariableW(name, value.data(), n);
    return value;
}

// The known location: %USERPROFILE%\.config\winspace\winspace.conf — the Win32
// home for the Hyprland-style ~/.config/<app> layout. Empty optional when
// %USERPROFILE% is unset, which leaves winspace on its built-in defaults.
inline std::optional<std::filesystem::path> configPath() {
    const std::optional<std::wstring> home = envVar(L"USERPROFILE");
    if (!home) return std::nullopt;
    return std::filesystem::path(*home) / L".config" / L"winspace" / L"winspace.conf";
}

// Slurp a file's bytes verbatim (the config is UTF-8, which the parser consumes as
// bytes). Empty optional on any open failure.
inline std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return std::move(buffer).str();
}

// Resolve the config text for this run. On the happy path: read the file at the
// known location. On first run (no file): create the directory chain, seed it with
// the default, and read that back so the source of truth is always the file. Every
// failure along the way degrades to the built-in default with a diagnostic, so a
// missing home or an unwritable disk never leaves winspace with no binds.
inline std::string loadConfigText() {
    const std::optional<std::filesystem::path> path = configPath();
    if (!path) {
        lg::warn("config: %USERPROFILE% is unset; using built-in defaults");
        return std::string(k_defaultConfig);
    }
    const std::string shown = narrow(path->wstring());

    std::error_code ec;
    if (!std::filesystem::exists(*path, ec)) {
        std::filesystem::create_directories(path->parent_path(), ec);
        if (ec) {
            lg::warn("config: cannot create {}: {}; using built-in defaults",
                     narrow(path->parent_path().wstring()), ec.message());
            return std::string(k_defaultConfig);
        }
        std::ofstream out(*path, std::ios::binary);
        out << k_defaultConfig;
        if (!out) {
            lg::warn("config: cannot write default {}; using built-in defaults", shown);
            return std::string(k_defaultConfig);
        }
        lg::info("config: seeded default config at {}", shown);
    }

    if (std::optional<std::string> text = readFile(*path)) {
        lg::info("config: loaded {}", shown);
        return std::move(*text);
    }
    lg::warn("config: cannot read {}; using built-in defaults", shown);
    return std::string(k_defaultConfig);
}

// Both halves of one parse: the Binds the Hotkey thread registers and the
// WindowRules the Worker seeds into State (PRD 06). Owned by runApp for the
// lifetime of the threads that borrow them.
struct LoadedConfig {
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
};

// Parse the config once, surfacing any parser diagnostics through the I/O sink,
// and return both binds and rules. `text` outlives parse(); the returned data owns
// itself. (Replaces the former loadBinds — one parse, both halves; PRD 06.)
inline LoadedConfig loadConfig() {
    const std::string text = loadConfigText();
    ParseResult parsed = parse(text);
    for (const Diagnostic& d : parsed.diagnostics)
        lg::warn("config line {}: {}", d.line, d.message);
    return {std::move(parsed.config.binds), std::move(parsed.config.rules)};
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

    HotkeyTable hotkeys(binds);

    if (selftestQuitEnabled()) {
        postEvent(workerHwnd, new Event{Quit{}});
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // A hotkey has a null window, so it never dispatches to a WndProc — handle
        // it here. wParam is the hotkey id, i.e. the Bind's index in the table.
        if (msg.message == WM_HOTKEY) {
            if (auto ev = hotkeys.eventFor(static_cast<int>(msg.wParam)))
                postEvent(workerHwnd, new Event{std::move(*ev)});
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Bring up both threads, run until Exit, then tear down cleanly. Returns the
// process exit code. Called by wWinMain.
inline int runApp() {
    // Per-Monitor-V2 DPI awareness, set before any window exists (the Worker's
    // message-only HWND included), so it leads runApp. This puts the process and
    // every window rect in ONE physical-pixel coordinate space — the precondition
    // for correct spatial-focus resolution across scaled displays (issue 05). Best
    // effort: a failure (an OS predating V2, or awareness already pinned by a
    // manifest) leaves the prior awareness in place and is logged, never fatal.
    if (const auto aware =
            ok(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        !aware) {
        lg::warn("app: Per-Monitor-V2 DPI awareness not set: {}", aware.error());
    }

    // One parse yields both halves. The Binds are owned here and passed to the
    // Hotkey thread by const reference (this declaration outlives hotkeyThread,
    // joined below); the WindowRules are moved into the Worker thread, which seeds
    // them into State before publishing its HWND.
    LoadedConfig cfg = loadConfig();
    const std::vector<Bind> binds = std::move(cfg.binds);

    // Start the Worker and wait for its message-only HWND before anyone posts. The
    // rules are seeded in the Worker ctor, so they precede any Appeared.
    std::promise<HWND> hwndReady;
    std::future<HWND> hwndFuture = hwndReady.get_future();
    std::jthread workerThread(workerThreadMain, std::move(hwndReady), std::move(cfg.rules));

    const HWND workerHwnd = hwndFuture.get();
    if (!workerHwnd) {
        workerThread.join();
        return 1;  // Worker could not create its window — nothing can proceed
    }

    // Start the Hotkey thread once the Worker HWND exists to post to. It registers
    // the Binds as OS hotkeys on its own thread.
    std::promise<DWORD> tidReady;
    std::future<DWORD> tidFuture = tidReady.get_future();
    std::jthread hotkeyThread(hotkeyThreadMain, workerHwnd, std::cref(binds),
                              std::move(tidReady));
    const DWORD hotkeyTid = tidFuture.get();

    // Start the Hook thread (third jthread, mirroring the Hotkey thread). It owns
    // the SetWinEventHook adapter, registers its hooks then runs startup adoption,
    // and posts Appeared / Vanished Events to the Worker.
    std::promise<DWORD> hookTidReady;
    std::future<DWORD> hookTidFuture = hookTidReady.get_future();
    std::jthread hookThread(hookThreadMain, workerHwnd, std::move(hookTidReady));
    const DWORD hookTid = hookTidFuture.get();

    // Block until an Exit Effect posts WM_QUIT to the Worker and its loop returns.
    workerThread.join();

    // Worker is down; unwind the Hotkey and Hook loops. These WM_QUITs are
    // thread-lifecycle control — NOT the Event transport, which is always a
    // PostMessage to the Worker's HWND. The Hook thread unhooks both hooks after
    // its loop returns, so no dangling SetWinEventHook survives quit.
    PostThreadMessageW(hotkeyTid, WM_QUIT, 0, 0);
    hotkeyThread.join();
    PostThreadMessageW(hookTid, WM_QUIT, 0, 0);
    hookThread.join();
    return 0;
}

}  // namespace winspace::io
