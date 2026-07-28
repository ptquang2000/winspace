// Spike: shell service registration ordering (PRD 0027 ticket 04, ADR-0025).
//
// NOT production code and NOT part of the winspace build. This is a measurement
// instrument: it answers one unmeasured question that gates a decision in the
// three-state availability model.
//
// THE QUESTION. ADR-0025 distinguishes **Disconnected** (recoverable) from
// **Unsupported** (terminal). The natural discriminator is *"the ImmersiveShell
// object was created successfully, but no known IVirtualDesktopManagerInternal IID
// matched → this OS is unsupported"*. That is only safe if object creation and
// interface registration happen close together. If a starting shell accepts object
// creation well BEFORE it registers the virtual desktop interfaces, that
// discriminator would wrongly latch the terminal state during ordinary startup —
// which is the exact bug the whole effort is fixing.
//
// winspace already implements a rule that is safe either way (within an acquisition
// attempt an unmatched IID counts as Disconnected; Unsupported is concluded only
// after the attempt's budget elapses). This measures how much MARGIN that rule has.
//
// WHAT IT DOES. Per run: kill explorer.exe, stamp t0, then poll every few ms —
// creating everything FRESH each time, so a cached proxy can never answer for a
// service that is not really back — recording the first millisecond at which each
// of these succeeds:
//
//   1. CoCreateInstance(ImmersiveShell)             <- the object exists
//   2. QueryService(IVirtualDesktopManagerInternal) <- the discriminator's subject
//   3. QueryService(IApplicationViewCollection)
//   4. QueryService(IVirtualDesktopNotificationService)
//
// The reported GAP is (2) - (1): the window in which a naive discriminator misfires.
//
// BUILD (from an x64 Native Tools prompt):
//   cl /std:c++20 /EHsc /O2 /nologo scripts\spike\VdRegistrationTiming.cpp ^
//      /Fe:vd-timing.exe /link ole32.lib advapi32.lib
//
// RUN (interactive session; it restarts the shell, so it disables any running
// winspace until that winspace is restarted):
//   vd-timing.exe [runs]        (default 3)

#include <windows.h>

#include <objbase.h>
#include <servprov.h>
#include <tlhelp32.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// The same CLSIDs / IIDs winspace pins (src/win32.cpp, namespace vd). Duplicated
// rather than shared: this file is not part of the winspace build, and a spike that
// silently drifts with production is worse than one that is obviously a copy.
constexpr GUID k_clsidImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
constexpr GUID k_clsidVirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
constexpr GUID k_iidVDMInternal_53F5CA0B = {
    0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
constexpr GUID k_iidVDMInternal_A3175F2D = {
    0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}};
constexpr GUID k_iidVDMInternal_B2F925B9 = {
    0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}};
constexpr GUID k_iidApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};
constexpr GUID k_clsidVirtualDesktopNotificationService = {
    0xA501FDEC, 0x4A09, 0x464C, {0xAE, 0x4E, 0x1B, 0x9C, 0x21, 0xB8, 0x49, 0x18}};
constexpr GUID k_iidVirtualDesktopNotificationService = {
    0x0CD45E71, 0xD927, 0x4F15, {0x8B, 0x0A, 0x8F, 0xEF, 0x52, 0x53, 0x37, 0xBF}};

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

void killShell() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) continue;
        if (const HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID)) {
            TerminateProcess(p, 0);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
}

// One probe of the whole set, everything created fresh. Returns which milestones
// answered on THIS attempt.
struct Attempt {
    bool object = false;
    bool manager = false;
    bool views = false;
    bool notifications = false;
};

Attempt probe() {
    Attempt got;
    IServiceProvider* shell = nullptr;
    if (FAILED(CoCreateInstance(k_clsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_IServiceProvider, reinterpret_cast<void**>(&shell))) ||
        !shell)
        return got;
    got.object = true;

    // Probe the manager newest→oldest exactly as winspace does; ANY match counts as
    // "the interface is registered", which is what the discriminator keys on.
    for (const GUID& iid : {k_iidVDMInternal_53F5CA0B, k_iidVDMInternal_A3175F2D,
                            k_iidVDMInternal_B2F925B9}) {
        IUnknown* m = nullptr;
        if (SUCCEEDED(shell->QueryService(k_clsidVirtualDesktopManagerInternal, iid,
                                          reinterpret_cast<void**>(&m))) &&
            m) {
            got.manager = true;
            m->Release();
            break;
        }
    }

    IUnknown* v = nullptr;
    if (SUCCEEDED(shell->QueryService(k_iidApplicationViewCollection,
                                      k_iidApplicationViewCollection,
                                      reinterpret_cast<void**>(&v))) &&
        v) {
        got.views = true;
        v->Release();
    }

    IUnknown* n = nullptr;
    if (SUCCEEDED(shell->QueryService(k_clsidVirtualDesktopNotificationService,
                                      k_iidVirtualDesktopNotificationService,
                                      reinterpret_cast<void**>(&n))) &&
        n) {
        got.notifications = true;
        n->Release();
    }

    shell->Release();
    return got;
}

struct Run {
    long long object = -1;
    long long manager = -1;
    long long views = -1;
    long long notifications = -1;
};

Run measureOnce() {
    Run r;
    killShell();
    const auto t0 = Clock::now();

    // 30 s is far past any plausible shell restart; a -1 in the output means the
    // milestone never arrived, which is itself the finding.
    while (msSince(t0) < 30000) {
        const Attempt got = probe();
        if (got.object && r.object < 0) r.object = msSince(t0);
        if (got.manager && r.manager < 0) r.manager = msSince(t0);
        if (got.views && r.views < 0) r.views = msSince(t0);
        if (got.notifications && r.notifications < 0) r.notifications = msSince(t0);
        if (r.object >= 0 && r.manager >= 0 && r.views >= 0 && r.notifications >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return r;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int runs = argc > 1 ? _wtoi(argv[1]) : 3;

    DWORD build = 0, ubr = 0, cb = sizeof(DWORD);
    wchar_t buildStr[32]{};
    DWORD cbBuild = sizeof(buildStr);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buildStr,
                     &cbBuild) == ERROR_SUCCESS)
        build = wcstoul(buildStr, nullptr, 10);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &ubr, &cb);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wprintf(L"vd-registration-timing — build %lu.%lu, %d run(s)\n", build, ubr, runs);
    std::wprintf(L"ms from the shell process being killed:\n\n");
    std::wprintf(L"%-4s %10s %10s %10s %10s %10s\n", L"run", L"object", L"manager", L"views",
                 L"notify", L"GAP");

    std::vector<long long> gaps;
    for (int i = 1; i <= runs; ++i) {
        const Run r = measureOnce();
        const long long gap =
            (r.object >= 0 && r.manager >= 0) ? r.manager - r.object : -1;
        if (gap >= 0) gaps.push_back(gap);
        std::wprintf(L"%-4d %10lld %10lld %10lld %10lld %10lld\n", i, r.object, r.manager,
                     r.views, r.notifications, gap);
        // Let the shell settle fully before the next kill, so run N+1 measures a
        // restart from a steady state rather than from a half-started shell.
        std::this_thread::sleep_for(std::chrono::seconds(8));
    }

    if (!gaps.empty()) {
        long long lo = gaps[0], hi = gaps[0], sum = 0;
        for (const long long g : gaps) {
            lo = min(lo, g);
            hi = max(hi, g);
            sum += g;
        }
        std::wprintf(L"\nGAP (object -> manager): min %lld ms, max %lld ms, mean %lld ms\n", lo,
                     hi, sum / static_cast<long long>(gaps.size()));
        std::wprintf(L"This is the window in which a naive \"object created but no known IID "
                     L"matched => Unsupported\"\ndiscriminator would misfire.\n");
    }

    CoUninitialize();
    return 0;
}
