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

#include "io/error.cpp"      // io::Error vocabulary (ok() wrappers) + lg:: levels
#include "io/vd_bridge.cpp"  // IVirtualDesktopBridge + factory (this thread owns it)
#include "winspace/reducer.cpp"

namespace winspace::io {

// The custom window message that carries an Event* to the Worker. WM_APP is the
// first id reserved for private application use, so it never collides with
// system or common-control messages.
inline constexpr UINT k_wmEvent = WM_APP + 0;

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
    Worker() {
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
        if (m_bridge) m_state.current_workspace = m_bridge->currentWorkspace();
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
            },
            effect);
    }

    State m_state{};
    HWND m_hwnd = nullptr;
    bool m_comInitialized = false;
    std::unique_ptr<IVirtualDesktopBridge> m_bridge;  // COM VD bridge; null if unsupported
};

// Worker thread entry. Constructs the Worker (COM + message-only HWND), publishes
// the HWND to the spine, then pumps until Exit. On return the Worker destructor
// tears COM and the window down on this same thread. A null published HWND
// signals a construction failure the spine treats as fatal.
inline void workerThreadMain(std::promise<HWND> ready) {
    Worker worker;
    const HWND hwnd = worker.hwnd();
    ready.set_value(hwnd);
    if (hwnd) worker.run();
}

}  // namespace winspace::io
