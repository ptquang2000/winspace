// COM Virtual Desktop bridge — I/O adapter, sole owner of the undocumented
// IVirtualDesktopManagerInternal ABI. All COM lives behind IVirtualDesktopBridge
// (winspace vocabulary: logical workspace in, switch out); no COM type escapes
// this file. The Worker (sole STA owner) runs the SwitchToWorkspace Effect via
// switchTo(), oblivious to COM. See docs/adr/0002 (COM VD bridge, IID-probe
// selection) and docs/adr/0003 (sparse GUID-anchored workspaces).
//
// Variant selection is a self-validating IID probe: QI each known IID
// newest→oldest, first S_OK wins (QI-success ⟺ correct vtable). Interfaces are
// hand-declared MIDL_INTERFACE structs from the community RE lineage
// (MScholtes/VirtualDesktop VirtualDesktop11-24H2.cs, cross-checked against
// Ciantic/VirtualDesktopAccessor). Only 24H2 (build 26100) is implemented; older
// builds and any unknown IID are stubbed with a loud diagnostic — a future OS
// fails LOUDLY rather than calling through a wrong vtable.
#pragma once

#include <windows.h>

#include <objbase.h>   // CoCreateInstance, StringFromGUID2, MIDL_INTERFACE
#include <servprov.h>  // IServiceProvider, IID_IServiceProvider
#include <wrl/client.h>  // Microsoft::WRL::ComPtr — RAII for the COM pointers

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

#include "io/hotkeys.cpp"  // emitDiagnostic — the shared I/O diagnostic sink

namespace winspace::io {

// ── the seam: pure winspace vocabulary, no COM type in sight ─────────────────

// The abstraction the rest of winspace sees. Logical workspace numbers in,
// switches out. The Worker holds one of these and never learns it is COM.
class IVirtualDesktopBridge {
public:
    virtual ~IVirtualDesktopBridge() = default;

    // Switch to the Logical workspace, materializing it on demand (sparse model,
    // ADR-0003): hit → resolve the stored GUID to its live desktop → SwitchDesktop;
    // miss → create exactly one desktop (appended), bind logical→GUID, switch.
    // Returns true iff the OS desktop is now the requested workspace.
    virtual bool switchTo(int logical) = 0;

    // The Logical workspace active at startup, seeded from the OS active desktop
    // during adoption. Lets the Worker align its State with reality on boot.
    virtual int currentWorkspace() const = 0;
};

// ── hand-declared undocumented COM ABI (RE lineage; see file header) ─────────
//
// Namespaced so these names can't collide with SDK headers. Methods are
// stdcall-returning-HRESULT; the C# sources' by-value returns become trailing
// out-params here. Params we never call through (IApplicationView*, HSTRING) are
// pointer-width `void*` — ABI-identical, keeps the vtable slots aligned.
namespace vd {

// CLSIDs — stable across all Windows 11 builds.
inline constexpr GUID k_clsidImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
inline constexpr GUID k_clsidVirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// IID_IVirtualDesktop — 24H2 / build 26100 (VirtualDesktop11-24H2.cs). Passed to
// IObjectArray::GetAt to fetch each desktop as this element type.
inline constexpr GUID k_iidVirtualDesktop24H2 = {
    0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};

// IID_IVirtualDesktopManagerInternal by variant, newest→oldest (the probe order).
// NOTE: {53F5CA0B-...} is shared by 24H2 (26100) AND post-KB5034204 23H2 (22631)
// — same IID, DIFFERENT vtable (24H2 inserts SwitchDesktopAndMoveForegroundView
// at slot 10). IID-probe therefore cannot tell them apart; the factory gates on
// the OS build number to fail closed on 23H2 rather than call a shifted vtable.
inline constexpr GUID k_iidVDMInternal_53F5CA0B = {  // 24H2 (26100) & 23H2-KB (22631)
    0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
inline constexpr GUID k_iidVDMInternal_A3175F2D = {  // 22H2 / pre-KB 23H2
    0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}};
inline constexpr GUID k_iidVDMInternal_B2F925B9 = {  // 21H2 (22000)
    0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}};

MIDL_INTERFACE("FF72FFDD-BE7E-43FC-9C03-AD81681E88E4")
IApplicationView : public IUnknown{};  // opaque — only ever passed as a pointer

// IVirtualDesktop — 24H2 vtable (VirtualDesktop11-24H2.cs). We call only GetID.
MIDL_INTERFACE("3F07F4BE-B107-441A-AF0F-39D82529072C")
IVirtualDesktop : public IUnknown {
    STDMETHOD(IsViewVisible)(void* pView, int* pfVisible) = 0;  // slot 3
    STDMETHOD(GetID)(GUID* pGuid) = 0;                          // slot 4
    STDMETHOD(GetName)(void* pName) = 0;                        // slot 5 (HSTRING)
    STDMETHOD(GetWallpaperPath)(void* pPath) = 0;               // slot 6 (HSTRING)
    STDMETHOD(IsRemote)(int* pfRemote) = 0;                     // slot 7
};

// IObjectArray — stable. We call GetCount and GetAt.
MIDL_INTERFACE("92CA9DCD-5622-4BBA-A805-5E9F541BD8C9")
IObjectArray : public IUnknown {
    STDMETHOD(GetCount)(UINT* pcObjects) = 0;                              // slot 3
    STDMETHOD(GetAt)(UINT uiIndex, REFIID riid, void** ppv) = 0;          // slot 4
};

// IVirtualDesktopManagerInternal — 24H2 vtable (VirtualDesktop11-24H2.cs),
// transcribed verbatim slot-for-slot. We call GetCurrentDesktop, GetDesktops,
// SwitchDesktop, and CreateDesktop; the rest are declared only to keep the
// vtable layout exact. On 24H2, SwitchDesktop/CreateDesktop take NO leading
// monitor/view parameter.
MIDL_INTERFACE("53F5CA0B-158F-4124-900C-057158060B27")
IVirtualDesktopManagerInternal : public IUnknown {
    STDMETHOD(GetCount)(UINT* pCount) = 0;                                       // 3
    STDMETHOD(MoveViewToDesktop)(void* pView, IVirtualDesktop* pDesktop) = 0;    // 4
    STDMETHOD(CanViewMoveDesktops)(void* pView, int* pfCanMove) = 0;             // 5
    STDMETHOD(GetCurrentDesktop)(IVirtualDesktop** ppDesktop) = 0;               // 6
    STDMETHOD(GetDesktops)(IObjectArray** ppDesktops) = 0;                       // 7
    STDMETHOD(GetAdjacentDesktop)(IVirtualDesktop* pFrom, int direction,
                                  IVirtualDesktop** ppDesktop) = 0;              // 8
    STDMETHOD(SwitchDesktop)(IVirtualDesktop* pDesktop) = 0;                     // 9
    STDMETHOD(SwitchDesktopAndMoveForegroundView)(IVirtualDesktop* pD) = 0;      // 10
    STDMETHOD(CreateDesktop)(IVirtualDesktop** ppNewDesktop) = 0;                // 11
    STDMETHOD(MoveDesktop)(IVirtualDesktop* pDesktop, int nIndex) = 0;           // 12
    STDMETHOD(RemoveDesktop)(IVirtualDesktop* pRemove,
                             IVirtualDesktop* pFallback) = 0;                    // 13
    STDMETHOD(FindDesktop)(GUID* pId, IVirtualDesktop** ppDesktop) = 0;          // 14
    STDMETHOD(GetDesktopSwitchIncludeExcludeViews)(IVirtualDesktop* pDesktop,
                                                   IObjectArray** ppIn,
                                                   IObjectArray** ppOut) = 0;    // 15
    STDMETHOD(SetDesktopName)(IVirtualDesktop* pDesktop, void* name) = 0;        // 16
    STDMETHOD(SetDesktopWallpaper)(IVirtualDesktop* pDesktop, void* path) = 0;   // 17
    STDMETHOD(UpdateWallpaperPathForAllDesktops)(void* path) = 0;                // 18
    STDMETHOD(CopyDesktopState)(void* pView0, void* pView1) = 0;                 // 19
    STDMETHOD(CreateRemoteDesktop)(void* path, IVirtualDesktop** ppDesktop) = 0; // 20
    STDMETHOD(SwitchRemoteDesktop)(IVirtualDesktop* pDesktop, void* type) = 0;   // 21
    STDMETHOD(SwitchDesktopWithAnimation)(IVirtualDesktop* pDesktop) = 0;        // 22
    STDMETHOD(GetLastActiveDesktop)(IVirtualDesktop** ppDesktop) = 0;            // 23
    STDMETHOD(WaitForAnimationToComplete)() = 0;                                 // 24
};

}  // namespace vd

// ── variant identity + diagnostics ──────────────────────────────────────────

namespace bridge_detail {

// Which RE-captured variant an IID probe (plus build gate) resolves to.
enum class VdVariant {
    None,     // no known IID matched — fail closed
    W24H2,    // 24H2 / build 26100 — the one implemented + verified here
    W23H2KB,  // post-KB5034204 23H2 — shares 24H2's IID, different vtable: stubbed
    W22H2,    // 22H2 / pre-KB 23H2 — stubbed
    W21H2,    // 21H2 / build 22000 — stubbed
};

// Format a GUID as the canonical "{XXXXXXXX-...}" string for diagnostics.
inline std::wstring guidToWString(const GUID& g) {
    std::wstring buf(40, L'\0');  // "{8-4-4-4-12}" is 38 chars + braces + null
    const int n = StringFromGUID2(g, buf.data(), static_cast<int>(buf.size()));
    buf.resize(n > 0 ? static_cast<size_t>(n) - 1 : 0);  // n counts the null terminator
    return buf;
}

// Format an HRESULT as "0xXXXXXXXX" for diagnostics.
inline std::wstring hrToWString(HRESULT hr) {
    std::wstring buf(11, L'\0');  // "0x" + 8 hex digits + null
    swprintf_s(buf.data(), buf.size(), L"0x%08lX", static_cast<unsigned long>(hr));
    buf.resize(10);
    return buf;
}

// Read HKLM CurrentBuildNumber (e.g. 26100); 0 if unreadable. Diagnostic-only
// per ADR-0002, plus the build gate that separates 24H2 from 23H2-KB.
inline DWORD readBuildNumber() {
    std::wstring buf(32, L'\0');
    DWORD cb = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buf.data(),
                     &cb) == ERROR_SUCCESS) {
        return static_cast<DWORD>(wcstoul(buf.c_str(), nullptr, 10));
    }
    return 0;
}

// Read HKLM UBR (the .NNNN update-build-revision); 0 if unreadable. Diagnostic-only.
inline DWORD readUbr() {
    DWORD ubr = 0;
    DWORD cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &ubr, &cb);
    return ubr;
}

// Optional smoke-test hook (task 07 step 6): WINSPACE_FORCE_VD_VARIANT=21h2|22h2|
// 23h2-kb forces selection of a stubbed variant so its "not yet implemented"
// diagnostic can be observed on the 24H2 dev machine. Unset → normal probing.
inline std::wstring forcedVariantOverride() {
    std::wstring buf(32, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"WINSPACE_FORCE_VD_VARIANT", buf.data(),
                                            static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};  // unset, or too long for the buffer
    buf.resize(n);
    return buf;
}

}  // namespace bridge_detail

// ── the 24H2 implementation ──────────────────────────────────────────────────

// GUID-anchored sparse bridge (ADR-0003). Owns the logical→GUID map, adoption,
// create-on-demand, and live GUID→desktop resolution — none of which the pure
// reducer ever sees. Constructed on (and destroyed on) the Worker's STA thread,
// so every COM pointer here is touched only by its apartment owner.
class VirtualDesktop24H2Bridge final : public IVirtualDesktopBridge {
public:
    // Takes the already-QI'd manager (its S_OK proved the vtable) and runs
    // adoption immediately, binding pre-existing desktops to logical 1..N by GUID.
    explicit VirtualDesktop24H2Bridge(
        Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> manager)
        : m_manager(std::move(manager)) {
        adopt();
    }

    bool switchTo(int logical) override {
        if (!m_manager) return false;

        // Hit: resolve the stored GUID to whatever live desktop now holds it
        // (survives Task View reorder — we match by identity, not position).
        if (const auto it = m_logicalToGuid.find(logical); it != m_logicalToGuid.end()) {
            if (auto desktop = resolveLiveDesktop(it->second)) {
                return doSwitch(logical, desktop.Get());
            }
            // The bound desktop was destroyed out from under us (e.g. via Task
            // View). Fall through and re-materialize the workspace.
            emitDiagnostic(L"workspace " + std::to_wstring(logical) +
                           L": bound desktop no longer exists — recreating");
        }

        // Miss (or stale): create exactly ONE desktop, appended at the tail; bind
        // logical→GUID; switch. No intermediate filling, no clamp (ADR-0003).
        Microsoft::WRL::ComPtr<vd::IVirtualDesktop> created;
        if (FAILED(m_manager->CreateDesktop(created.ReleaseAndGetAddressOf())) || !created) {
            emitDiagnostic(L"workspace " + std::to_wstring(logical) +
                           L": CreateDesktop failed");
            return false;
        }
        GUID id{};
        if (FAILED(created->GetID(&id))) return false;
        m_logicalToGuid[logical] = id;
        return doSwitch(logical, created.Get());
    }

    int currentWorkspace() const override { return m_current; }

private:
    // Bind every pre-existing desktop to logical 1..N by GUID and seed the current
    // workspace from the active desktop (ADR-0003 startup adoption).
    void adopt() {
        Microsoft::WRL::ComPtr<vd::IObjectArray> desktops;
        if (FAILED(m_manager->GetDesktops(desktops.ReleaseAndGetAddressOf())) || !desktops)
            return;
        UINT count = 0;
        if (FAILED(desktops->GetCount(&count))) return;

        auto bindings =
            std::views::iota(0u, count) |
            std::views::transform([&](const UINT i) -> std::optional<std::pair<int, GUID>> {
                Microsoft::WRL::ComPtr<vd::IVirtualDesktop> desktop;
                GUID id{};
                if (FAILED(desktops->GetAt(
                        i, vd::k_iidVirtualDesktop24H2,
                        reinterpret_cast<void**>(desktop.ReleaseAndGetAddressOf()))) ||
                    FAILED(desktop->GetID(&id)))
                    return std::nullopt;
                return std::pair{static_cast<int>(i) + 1, id};  // logical is 1-based
            }) |
            std::views::filter([](const auto& binding) { return binding.has_value(); });

        std::ranges::for_each(
            bindings, [&](const auto& binding) { m_logicalToGuid[binding->first] = binding->second; });

        Microsoft::WRL::ComPtr<vd::IVirtualDesktop> active;
        GUID activeId{};
        if (SUCCEEDED(m_manager->GetCurrentDesktop(active.ReleaseAndGetAddressOf())) &&
            active && SUCCEEDED(active->GetID(&activeId))) {
            const auto match = std::ranges::find_if(
                m_logicalToGuid,
                [&](const auto& entry) { return IsEqualGUID(entry.second, activeId); });
            if (match != m_logicalToGuid.end()) m_current = match->first;
        }

        emitDiagnostic(L"virtual desktop bridge: adopted " + std::to_wstring(count) +
                       L" desktop(s); current workspace = " + std::to_wstring(m_current));
    }

    // Resolve a stored GUID to the live IVirtualDesktop that currently holds it,
    // by identity — not by remembered position. Null if it no longer exists.
    Microsoft::WRL::ComPtr<vd::IVirtualDesktop> resolveLiveDesktop(const GUID& target) {
        Microsoft::WRL::ComPtr<vd::IObjectArray> desktops;
        if (FAILED(m_manager->GetDesktops(desktops.ReleaseAndGetAddressOf())) || !desktops)
            return nullptr;
        UINT count = 0;
        if (FAILED(desktops->GetCount(&count))) return nullptr;

        // Lazily turn each index into its live desktop, then find the one that
        // projects to target's GUID. end() means the target no longer exists.
        auto liveDesktops = std::views::iota(0u, count) | std::views::transform([&](const UINT i) {
            Microsoft::WRL::ComPtr<vd::IVirtualDesktop> desktop;
            if (FAILED(desktops->GetAt(
                    i, vd::k_iidVirtualDesktop24H2,
                    reinterpret_cast<void**>(desktop.ReleaseAndGetAddressOf()))))
                return decltype(desktop){};  // null → projects to a zero GUID, never target
            return desktop;
        });

        const auto guidOf = [](const Microsoft::WRL::ComPtr<vd::IVirtualDesktop>& desktop) {
            GUID id{};
            return (desktop && SUCCEEDED(desktop->GetID(&id))) ? id : GUID{};
        };
        const auto it = std::ranges::find(liveDesktops, target, guidOf);
        return it != std::ranges::end(liveDesktops) ? *it : nullptr;
    }

    // Call SwitchDesktop and, on success, record the new current workspace.
    bool doSwitch(int logical, vd::IVirtualDesktop* desktop) {
        if (FAILED(m_manager->SwitchDesktop(desktop))) {
            emitDiagnostic(L"workspace " + std::to_wstring(logical) +
                           L": SwitchDesktop failed");
            return false;
        }
        m_current = logical;
        return true;
    }

    Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> m_manager;
    std::unordered_map<int, GUID> m_logicalToGuid;  // logical workspace → desktop identity
    int m_current = 1;
};

// ── acquisition + IID-probe variant selection (the factory) ──────────────────

// Build the bridge, or return null (with a loud diagnostic) if no known variant
// resolves. Must run on the STA thread owning the COM apartment (the Worker).
// Sequence per ADR-0002: CoCreateInstance(ImmersiveShell, IServiceProvider) →
// QueryService(VirtualDesktopManagerInternal, <probed IID>) newest→oldest → first
// S_OK wins (QI-success ⟺ correct vtable).
inline std::unique_ptr<IVirtualDesktopBridge> makeVirtualDesktopBridge() {
    using bridge_detail::VdVariant;
    const DWORD build = bridge_detail::readBuildNumber();
    const DWORD ubr = bridge_detail::readUbr();
    const std::wstring buildStr =
        L"build " + std::to_wstring(build) + L"." + std::to_wstring(ubr);

    Microsoft::WRL::ComPtr<IServiceProvider> shell;
    HRESULT hr = CoCreateInstance(vd::k_clsidImmersiveShell, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_IServiceProvider,
                                  reinterpret_cast<void**>(shell.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !shell) {
        emitDiagnostic(L"virtual desktop bridge: CoCreateInstance(ImmersiveShell) "
                       L"failed (hr=" +
                       bridge_detail::hrToWString(hr) + L") — COM VD switching disabled");
        return nullptr;
    }

    // The probe table, newest→oldest: each row is an IID to QI and the variant it
    // denotes. QI succeeds only when the vtable matches, so the first S_OK is
    // self-validating (ADR-0002).
    struct Probe {
        const GUID& iid;
        VdVariant variant;
    };
    static constexpr std::array probes = {
        Probe{vd::k_iidVDMInternal_53F5CA0B, VdVariant::W24H2},  // resolved by build below
        Probe{vd::k_iidVDMInternal_A3175F2D, VdVariant::W22H2},
        Probe{vd::k_iidVDMInternal_B2F925B9, VdVariant::W21H2},
    };

    const std::wstring forced = bridge_detail::forcedVariantOverride();

    // Transform each probe into its QI outcome: manager (null if this IID's vtable
    // is wrong) + concrete variant. The shared 53F5CA0B IID is 24H2 only on build
    // ≥ 26100; on 22631 it is 23H2-KB with a different vtable — must not drive it
    // as 24H2. WINSPACE_FORCE_VD_VARIANT overrides the variant to test a stub.
    struct Resolved {
        GUID iid;
        VdVariant variant;
        Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> manager;
    };
    auto resolved = probes | std::views::transform([&](const Probe& probe) {
        Resolved r{probe.iid, probe.variant, {}};
        if (FAILED(shell->QueryService(
                vd::k_clsidVirtualDesktopManagerInternal, probe.iid,
                reinterpret_cast<void**>(r.manager.ReleaseAndGetAddressOf()))) ||
            !r.manager) {
            r.manager.Reset();  // wrong vtable for this IID — null signals "skip"
            return r;
        }
        if (IsEqualGUID(probe.iid, vd::k_iidVDMInternal_53F5CA0B))
            r.variant = (build >= 26100) ? VdVariant::W24H2 : VdVariant::W23H2KB;
        if (forced == L"21h2") r.variant = VdVariant::W21H2;
        else if (forced == L"22h2") r.variant = VdVariant::W22H2;
        else if (forced == L"23h2-kb") r.variant = VdVariant::W23H2KB;
        return r;
    });

    // First probe with a valid manager wins (newest→oldest); dispatch on variant.
    for (auto&& [iid, variant, manager] : resolved) {
        if (!manager) continue;
        const std::wstring iidName = bridge_detail::guidToWString(iid);
        switch (variant) {
            case VdVariant::W24H2:
                emitDiagnostic(L"virtual desktop bridge: matched IID " + iidName + L" (" +
                               buildStr + L") — 24H2 variant");
                return std::make_unique<VirtualDesktop24H2Bridge>(std::move(manager));

            case VdVariant::W23H2KB:
                emitDiagnostic(L"virtual desktop bridge: IID " + iidName + L" resolved to "
                               L"23H2-KB5034204 variant (" + buildStr +
                               L") — NOT YET IMPLEMENTED (its vtable differs from 24H2); "
                               L"COM VD switching disabled");
                return nullptr;
            case VdVariant::W22H2:
                emitDiagnostic(L"virtual desktop bridge: IID " + iidName + L" resolved to "
                               L"22H2 / pre-KB 23H2 variant (" + buildStr +
                               L") — NOT YET IMPLEMENTED; COM VD switching disabled");
                return nullptr;
            case VdVariant::W21H2:
                emitDiagnostic(L"virtual desktop bridge: IID " + iidName + L" resolved to "
                               L"21H2 variant (" + buildStr +
                               L") — NOT YET IMPLEMENTED; COM VD switching disabled");
                return nullptr;
            case VdVariant::None:
                break;  // unreachable — a matched probe always names a variant
        }
    }

    // No known IID matched: a newer Windows bumped the interface. Fail LOUDLY
    // (null + diagnostic) — never call through a mismatched vtable (ADR-0002).
    emitDiagnostic(L"virtual desktop bridge: NO known IVirtualDesktopManagerInternal "
                   L"IID matched (" + buildStr +
                   L") — a new OS variant needs capturing; COM VD switching disabled");
    return nullptr;
}

}  // namespace winspace::io
