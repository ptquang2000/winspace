// Window Probe — I/O adapter (owns <windows.h>). The reactive sweep behind the
// `focus` dispatcher (issue 05): on a keypress the Worker runs it once, reads
// every top-level window's live attributes+rect and the foreground Origin into
// plain data, and hands that to the pure Reducer, which decides. Nothing is
// persisted and no timer runs — the adapter is idle until the next keypress
// (ADR-0006's reactive Probe, ADR-0007's stateless focus).
//
// This is the Probe half of the Probe/policy split: it gathers facts, it never
// decides. The Eligibility gate and the directional resolution both live in the
// Reducer (reducer.cpp), so this file has no policy to unit-test — its only job
// is to translate an HWND into a WindowAttrs faithfully.
//
// The WindowId ⇄ HWND mint lives here too: WindowId is an opaque uint64 minted
// from the HWND's pointer bits, and toHwnd reverses it for the
// SetForegroundWindow Effect (the "named reverse-mint helper" of the PRD).
#pragma once

#include <windows.h>

#include <dwmapi.h>  // DwmGetWindowAttribute / DWMWA_CLOAKED

#include <optional>
#include <string>
#include <string_view>
#include <utility>  // std::to_underlying
#include <vector>

#include "io/error.cpp"          // narrow() — UTF-16 → UTF-8 for WindowIdentity strings
#include "winspace/reducer.cpp"  // WindowAttrs, WindowId, MonitorId, Rect, WindowIdentity

namespace winspace::io {

// ── WindowId ⇄ HWND (and MonitorId ← HMONITOR) mint ──────────────────────────
// The core stores these opaque identities and hands them back on Effects; only
// this adapter ever converts to/from the live handle.
inline WindowId toWindowId(HWND h) {
    return static_cast<WindowId>(reinterpret_cast<uintptr_t>(h));
}
inline HWND toHwnd(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(std::to_underlying(id)));
}
inline MonitorId toMonitorId(HMONITOR m) {
    return static_cast<MonitorId>(reinterpret_cast<uintptr_t>(m));
}

// Read one window's live attributes into plain data — the eligibility facts the
// Reducer's gate ANDs together, plus the rect and monitor the directional
// resolution reasons over. One read per window; no decision is made here.
inline WindowAttrs probeWindow(HWND h) {
    WindowAttrs a{};
    a.id = toWindowId(h);
    a.topLevel = GetAncestor(h, GA_ROOT) == h;
    a.visible = IsWindowVisible(h) != FALSE;

    const LONG style = GetWindowLongW(h, GWL_STYLE);
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    a.thickFrame = (style & WS_THICKFRAME) != 0;
    a.caption = (style & WS_CAPTION) == WS_CAPTION;  // WS_CAPTION = WS_BORDER|WS_DLGFRAME
    a.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0;

    int cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    a.cloaked = cloaked != 0;

    RECT r{};
    GetWindowRect(h, &r);
    a.rect = Rect{r.left, r.top, r.right, r.bottom};

    const HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    a.monitor = toMonitorId(mon);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        // Fullscreen ⇔ the window covers its monitor's full bounds (rcMonitor, not
        // the work area) — a maximized app that leaves the taskbar visible is not
        // fullscreen. Skipped as a focus target by isEligible.
        a.fullscreen = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                       r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
    }
    return a;
}

// The sweep: every top-level window, probed into plain data. EnumWindows walks
// only genuine top-level windows, so a message-only HWND (winspace's own) is
// never seen. The Reducer filters this unfiltered set through isEligible.
inline std::vector<WindowAttrs> probeTopLevelWindows() {
    std::vector<WindowAttrs> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<WindowAttrs>*>(lp)->push_back(probeWindow(h));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// The string half of a window Probe — exe basename, window class, and title,
// narrowed to UTF-8 (PRD 06). Called ONLY on the Appeared path (window rules
// need it); the focus sweep never calls it, so these string reads stay off the
// focus hot path. Each read degrades to an empty string on failure — a rule
// simply won't match on a field it couldn't read, never a crash.
inline WindowIdentity probeIdentity(HWND h) {
    WindowIdentity id{};
    id.id = toWindowId(h);

    // exe: the process image basename via QueryFullProcessImageNameW (works for
    // foreign processes with PROCESS_QUERY_LIMITED_INFORMATION, unlike the
    // module-handle APIs).
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD len = static_cast<DWORD>(std::size(buf));
        if (QueryFullProcessImageNameW(proc, 0, buf, &len)) {
            const std::wstring_view full(buf, len);
            const size_t slash = full.find_last_of(L"\\/");
            id.exe = narrow(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
        }
        CloseHandle(proc);
    }

    // class: GetClassNameW into a fixed buffer (class names are short).
    wchar_t cls[256];
    if (const int n = GetClassNameW(h, cls, static_cast<int>(std::size(cls))); n > 0)
        id.windowClass = narrow(std::wstring_view(cls, static_cast<size_t>(n)));

    // title: sized by GetWindowTextLengthW, then filled (GetWindowTextW writes at
    // most size-1 chars + a null, so the buffer needs the extra slot).
    if (const int len = GetWindowTextLengthW(h); len > 0) {
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        const int got = GetWindowTextW(h, title.data(), len + 1);
        id.title = narrow(std::wstring_view(title.data(), static_cast<size_t>(got)));
    }
    return id;
}

// The Origin: the current foreground window as plain data, or nullopt when the
// desktop has no foreground window (e.g. only the shell is active) — which the
// Reducer treats as a no-op. The Origin's own Eligibility is never checked; it is
// only a reference rect and is excluded from its own Candidate set by WindowId.
inline std::optional<WindowAttrs> probeForeground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return std::nullopt;
    return probeWindow(fg);
}

}  // namespace winspace::io
