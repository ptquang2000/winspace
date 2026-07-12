// Worker thread — I/O adapter (owns <windows.h>, COM, and all State).
//
// The single STA thread at winspace's center. It:
//   * CoInitializeEx(COINIT_APARTMENTTHREADED) — sole apartment owner of the
//     COM Virtual Desktop bridge (wired in task 06);
//   * creates a message-only (HWND_MESSAGE) window in its constructor, so the
//     HWND is valid before anything can post to it;
//   * runs a GetMessage loop whose WndProc feeds each incoming Event through the
//     pure `reduce` and executes the emitted Effects on this thread;
//   * owns the authoritative State and the Reducer.
//
// Transport contract: producers (the Hotkey thread, task 05) hand an Event to
// the Worker by PostMessage-ing a heap-allocated `Event*` in LPARAM to this
// window (never PostThreadMessage — a thread-queue post has no HWND to own the
// message and races the loop's realization). The Worker takes ownership and
// deletes the Event after reducing.
#pragma once

#include <windows.h>
#include <objbase.h>  // CoInitializeEx / CoUninitialize (excluded by WIN32_LEAN_AND_MEAN)

#include <future>
#include <memory>
#include <thread>
#include <utility>  // std::move
#include <vector>

#include "io/autostart.cpp"  // syncAutostart — the ITaskService logon-task adapter (issue 10)
#include "io/config_io.cpp"  // readAndParseConfig — the shared reload read+parse (issue 09)
#include "io/error.cpp"      // io::Error vocabulary (ok() wrappers) + lg:: levels
#include "io/probe.cpp"      // window Probe sweep + WindowId ⇄ HWND mint (focus)
#include "io/vd_bridge.cpp"  // IVirtualDesktopBridge + factory (this thread owns it)
#include "winspace/reducer.cpp"

namespace winspace::io {

// The custom window message that carries an Event* to the Worker. WM_APP is the
// first id reserved for private application use, so it never collides with
// system or common-control messages.
inline constexpr UINT k_wmEvent = WM_APP + 0;

// Tells the Worker the Hotkey thread's id (WPARAM), so a live reload can hand that
// thread the new Binds. Posted to the Worker's HWND once by the spine after the
// Hotkey thread publishes its id (the Worker is constructed first, so it cannot
// know the id at construction). Issue 09 / ADR-0012.
inline constexpr UINT k_wmSetHotkeyThread = WM_APP + 1;

// Carries a freshly-parsed std::vector<Bind>* to the HOTKEY thread's queue on a
// reload (PostThreadMessage, so it has no HWND and is handled inline in that
// thread's loop, like WM_HOTKEY). The Hotkey thread takes ownership and rebuilds
// its HotkeyTable; on a failed post the sender deletes the vector (mirroring
// postEvent's delete-on-failure ownership). Issue 09 / ADR-0012.
inline constexpr UINT k_wmReloadBinds = WM_APP + 2;

// Hand an Event to the Worker across threads. Ownership transfers to the Worker
// on a successful post (it deletes after reducing); on failure — a full queue
// or a dead HWND — we delete here so a dropped Event never leaks.
inline void postEvent(HWND workerHwnd, Event* ev) {
    if (!PostMessageW(workerHwnd, k_wmEvent, 0, reinterpret_cast<LPARAM>(ev))) {
        delete ev;
    }
}

// The Worker thread's window and behavioral core. Constructed on the Worker
// thread so its COM apartment and its message-only window share that thread's
// affinity; destroyed on the same thread so teardown (DestroyWindow /
// CoUninitialize) is likewise thread-correct.
class Worker {
public:
    // Takes the parsed window rules and seeds m_state.rules BEFORE anything can
    // reach this Worker: the ctor runs before workerThreadMain publishes the HWND,
    // so rules are in place before any Appeared (synthetic-adoption or live) can
    // arrive (PRD 06 ordering). The rule list is wrapped in a shared handle so the
    // per-event State copy stays O(1) (ADR-0009 / PRD deviation).
    explicit Worker(std::vector<WindowRule> rules, std::vector<ExecEntry> execs,
                    bool startAtLogin) {
        m_state.rules = std::make_shared<const std::vector<WindowRule>>(std::move(rules));
        // Launch entries are seeded alongside rules and behind the same O(1)-copy
        // handle, so they precede the Started{} the spine posts right after the
        // HWND publishes (PRD 08).
        m_state.execs = std::make_shared<const std::vector<ExecEntry>>(std::move(execs));
        // The start_at_login flag is seeded here beside rules/execs (issue 09);
        // slice 10 will read it. It emits no Effect and is reseeded on reload.
        m_state.startAtLogin = startAtLogin;

        // This thread is the single STA that will own the COM bridge (task 06).
        // COINIT_APARTMENTTHREADED demands a message loop, which run() supplies.
        // A failed init is tolerated (the bridge factory degrades to a null bridge);
        // the derived bool is what gates the matching CoUninitialize in the dtor.
        m_comInitialized = ok(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)).has_value();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Worker::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = k_className;
        RegisterClassExW(&wc);  // harmless if already registered; CreateWindow still succeeds

        // HWND_MESSAGE → a message-only window: no pixels, no taskbar button, no
        // Alt-Tab entry. It exists solely to receive posted Events. Created in
        // the ctor so hwnd() is valid before run() (and before any producer posts).
        m_hwnd = CreateWindowExW(0, k_className, nullptr, 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance, this);
        // A null HWND is the fatal signal the spine acts on (via workerThreadMain);
        // checkWin32 here just names WHY it failed. Snapshot GetLastError immediately,
        // before any later call can clobber it. RegisterClassExW above stays unchecked:
        // its only failure is the benign already-registered case, which CreateWindowExW
        // shrugs off — a genuine problem resurfaces here as the null HWND.
        if (const auto created = ok(static_cast<BOOL>(m_hwnd != nullptr)); !created) {
            lg::error("worker: message-only window creation failed: {}", created.error());
        }

        // Build the COM Virtual Desktop bridge on this STA thread — its sole owner.
        // Null on an unsupported OS variant (a loud diagnostic is already logged);
        // switch Effects then no-op rather than calling through a wrong vtable.
        // Adoption ran in the factory, so seed State from the active desktop.
        m_bridge = makeVirtualDesktopBridge();
        if (m_bridge) m_state.currentWorkspace = m_bridge->currentWorkspace();
    }

    ~Worker() {
        // Release the COM bridge BEFORE CoUninitialize: a member is destroyed
        // only after this body runs, so an implicit release would fire on a
        // torn-down apartment. Reset here to keep COM teardown thread-correct.
        m_bridge.reset();
        if (m_hwnd) DestroyWindow(m_hwnd);
        if (m_comInitialized) CoUninitialize();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Valid immediately after construction (null only if window creation failed).
    HWND hwnd() const { return m_hwnd; }

    // The message pump. Returns when an Exit Effect has posted WM_QUIT. Posted
    // Events are routed to wndProc by DispatchMessage. GetMessage returns 0 on
    // WM_QUIT and -1 on error, so `> 0` exits cleanly on both.
    void run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    static constexpr const wchar_t* k_className = L"winspace.worker";

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            // Stash the Worker* handed in via CreateWindowEx's lpParam so later
            // messages can reach the instance.
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        auto* self = reinterpret_cast<Worker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && msg == k_wmEvent) {
            return self->onEvent(reinterpret_cast<Event*>(lparam));
        }
        if (self && msg == k_wmSetHotkeyThread) {
            self->m_hotkeyThreadId = static_cast<DWORD>(wparam);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // Take ownership of the posted Event, reduce it against current State, adopt
    // the new State, and execute each emitted Effect — all on the Worker thread.
    LRESULT onEvent(Event* raw) {
        const std::unique_ptr<Event> ev(raw);  // delete after reducing
        ReduceResult result = reduce(m_state, *ev);
        m_state = std::move(result.state);
        for (const Effect& effect : result.effects) execute(effect);
        return 0;
    }

    void execute(const Effect& effect) {
        std::visit(
            detail::overload{
                [&](const SwitchToWorkspace& s) {
                    // The bridge (sole owner: this thread) resolves the Logical
                    // number to a Virtual Desktop GUID and calls SwitchDesktop,
                    // materializing the workspace on demand. Null bridge (an
                    // unsupported OS variant) → the switch is a no-op.
                    if (m_bridge) m_bridge->switchTo(s.logical);
                },
                [&](const Exit&) {
                    // End run()'s loop; the process then unwinds cleanly.
                    PostQuitMessage(0);
                },
                [&](const ResolveFocus& rf) {
                    // Phase one of the focus round-trip (ADR-0008): run the Probe
                    // sweep and post the rects back to ourselves as a FocusResolve
                    // Event, which the Reducer then resolves. The Worker stays a
                    // mechanical executor — it never decides which window to focus.
                    postEvent(m_hwnd, new Event{FocusResolve{rf.dir, probeTopLevelWindows(),
                                                             probeForeground()}});
                },
                [&](const SetForegroundWindow& sf) {
                    // Bare call, degrade-and-log: a just-fired hotkey
                    // usually satisfies Win32's foreground-lock, so this succeeds;
                    // on failure we log and move on — never crash (null-bridge
                    // precedent). The scoped AttachThreadInput recovery is deferred
                    // until the Smoke seam proves the bare call insufficient.
                    if (const auto set = ok(::SetForegroundWindow(toHwnd(sf.id))); !set)
                        lg::warn("focus: SetForegroundWindow failed: {}", set.error());
                },
                [&](const MoveForegroundWindowToWorkspace& m) {
                    // Ungated by Eligibility (issue 06): the user aimed at the
                    // foreground window explicitly. Resolve it inline — no probe
                    // round-trip, since there is no pure decision to make. No bridge
                    // or no foreground window → degrade-and-log, never crash. The
                    // internal MoveViewToDesktop reassigns the window's desktop
                    // without ever painting it on the current one, so no DWM cloak
                    // is needed (and DWMWA_CLOAK is same-process-only anyway — it
                    // returns E_ACCESSDENIED on a foreign window; ADR-0010 revised).
                    if (!m_bridge) return;
                    const HWND fg = GetForegroundWindow();
                    if (!fg) {
                        lg::warn("move to workspace {}: no foreground window", m.logical);
                        return;
                    }
                    m_bridge->moveWindowToWorkspace(toWindowId(fg), m.logical);
                },
                [&](const MoveWindowToWorkspace& m) {
                    // The WindowRule move (PRD 06): a SPECIFIC window that just
                    // Appeared, by id — no GetForegroundWindow, no cloak. The
                    // internal MoveViewToDesktop reassigns the desktop without ever
                    // painting on the current one (ADR-0010 revised), so a
                    // cross-Workspace pin never flashes here. Null bridge → no-op.
                    if (m_bridge) m_bridge->moveWindowToWorkspace(m.id, m.logical);
                },
                [&](const LaunchApp& l) {
                    // Launch-only (PRD 08 / ADR-0011): start a detached child and
                    // forget it — no PID kept, no Workspace assigned (placement is a
                    // paired `windowrule`). CreateProcessW itself parses exe + args
                    // from the command line and searches %PATH%; it WRITES to
                    // lpCommandLine, so the widened command goes into a mutable
                    // buffer. cwd/env are inherited (nullptr). On success both handles
                    // are closed immediately, fully detaching the child so it outlives
                    // winspace; on failure we degrade-and-log and continue — one bad
                    // entry never takes down the WM or blocks the others.
                    std::wstring cmdline = widen(l.command);
                    STARTUPINFOW si{};
                    si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    if (const auto launched =
                            ok(CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                              0, nullptr, nullptr, &si, &pi))) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    } else {
                        lg::warn("exec: launch failed for '{}': {}", l.command,
                                 launched.error());
                    }
                },
                [&](const ReloadConfig&) {
                    // Live reload (issue 09, ADR-0012), executed on THIS thread. Re-read
                    // + re-parse via the shared helper, then apply ATOMICALLY: any
                    // Diagnostic — or an unreadable file — keeps the currently-running
                    // config live and changes nothing (the fallback is the user's own
                    // working config, so reload can afford to reject the whole file).
                    ConfigReadResult load = readAndParseConfig();
                    if (!load.read) {
                        lg::warn("reload: config unreadable; keeping the running config");
                        return;
                    }
                    if (!load.parsed.diagnostics.empty()) {
                        for (const Diagnostic& d : load.parsed.diagnostics)
                            lg::warn("reload: config line {}: {}", d.line, d.message);
                        lg::warn("reload: {} diagnostic(s); keeping the running config",
                                 load.parsed.diagnostics.size());
                        return;
                    }

                    // Clean parse — fan out to the three owners of config-derived state
                    // (ADR-0012). (1) Reseed the Worker's own State behind fresh O(1)-copy
                    // handles; the flag is a plain reseed.
                    Config& cfg = load.parsed.config;
                    m_state.rules =
                        std::make_shared<const std::vector<WindowRule>>(std::move(cfg.rules));
                    m_state.execs =
                        std::make_shared<const std::vector<ExecEntry>>(std::move(cfg.execs));
                    m_state.startAtLogin = cfg.startAtLogin;

                    // (2) Hand the new Binds to the Hotkey thread to rebuild its
                    // HotkeyTable on that thread (RegisterHotKey is thread-affine). Heap
                    // pointer with delete-on-failed-post ownership, mirroring postEvent.
                    if (m_hotkeyThreadId != 0) {
                        auto* binds = new std::vector<Bind>(std::move(cfg.binds));
                        if (!PostThreadMessageW(m_hotkeyThreadId, k_wmReloadBinds, 0,
                                                reinterpret_cast<LPARAM>(binds))) {
                            delete binds;
                            lg::warn("reload: could not hand new binds to the hotkey thread; "
                                     "hotkeys unchanged");
                        }
                    } else {
                        lg::warn("reload: hotkey thread id unknown; hotkeys unchanged");
                    }

                    // (3) Post Reloaded{} to ourselves so the existing exec-relaunch path
                    // runs unchanged — `exec` entries re-launch, `exec-once` do not — and
                    // the Reloaded handler additionally emits SyncAutostart, so the logon
                    // task is re-synced to the freshly-reseeded flag on this same reload.
                    postEvent(m_hwnd, new Event{Reloaded{}});
                    lg::info("reload: applied new config");
                },
                [&](const SyncAutostart& s) {
                    // Delegate the whole ITaskService interaction to the adapter (issue 10,
                    // ADR-0013): create-or-update the `\winspace\<username>` logon task when
                    // enabled, delete-if-exists when not. Degrade-and-log on any failure
                    // (ADR-0004) — autostart never blocks or crashes the WM.
                    if (const auto synced = syncAutostart(s.enabled); !synced)
                        lg::warn("autostart: sync (enabled={}) failed: {}", s.enabled,
                                 synced.error());
                },
            },
            effect);
    }

    State m_state{};
    HWND m_hwnd = nullptr;
    bool m_comInitialized = false;
    DWORD m_hotkeyThreadId = 0;  // set by k_wmSetHotkeyThread; target of reload's new Binds
    std::unique_ptr<IVirtualDesktopBridge> m_bridge;  // COM VD bridge; null if unsupported
};

// Worker thread entry. Constructs the Worker (COM + message-only HWND), publishes
// the HWND to the spine, then pumps until Exit. On return the Worker destructor
// tears COM and the window down on this same thread. A null published HWND
// signals a construction failure the spine treats as fatal.
inline void workerThreadMain(std::promise<HWND> ready, std::vector<WindowRule> rules,
                             std::vector<ExecEntry> execs, bool startAtLogin) {
    Worker worker(std::move(rules), std::move(execs), startAtLogin);
    const HWND hwnd = worker.hwnd();
    ready.set_value(hwnd);
    if (hwnd) worker.run();
}

}  // namespace winspace::io
