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
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <source_location>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <format>

#include "io/error.cpp"  // io::Error vocabulary (ok() wrappers, formatter) + lg:: levels
#include "io/probe.cpp"  // toHwnd — the WindowId → HWND reverse-mint for the move call

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

    // Move a window to the Logical workspace, materializing the target on demand
    // WITHOUT switching to it (ADR-0010, revised). Implemented with the INTERNAL
    // IApplicationViewCollection::GetViewForHwnd + MoveViewToDesktop — the public
    // IVirtualDesktopManager::MoveWindowToDesktop returns E_ACCESSDENIED for windows
    // the caller does not own, which is precisely winspace's case (a foreground app
    // window from another process). Returns true iff the move landed.
    virtual bool moveWindowToWorkspace(WindowId window, int logical) = 0;
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

// IID_IApplicationViewCollection — the service id AND interface id (QueryService
// uses the same GUID for both). Stable across Win10/11 builds. GetViewForHwnd on
// this turns an HWND into the IApplicationView that MoveViewToDesktop consumes,
// which is how a FOREIGN window is moved (the public MoveWindowToDesktop returns
// E_ACCESSDENIED for windows the caller does not own — see ADR-0010 revision).
inline constexpr GUID k_iidApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

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

// IApplicationViewCollection — the undocumented HWND→IApplicationView resolver
// (MScholtes lineage). Only GetViewForHwnd (slot 6) is called; slots 3–5 are
// declared solely to fix its vtable offset (params we never pass are void*,
// ABI-identical). This is the second internal interface ADR-0010 was revised to
// re-admit: MoveViewToDesktop needs an IApplicationView, and only this yields one
// from an HWND — the price of moving windows the caller does not own.
MIDL_INTERFACE("1841C6D7-4F9D-42C0-AF41-8747538F10E5")
IApplicationViewCollection : public IUnknown {
    STDMETHOD(GetViews)(IObjectArray** ppViews) = 0;                             // 3
    STDMETHOD(GetViewsByZOrder)(IObjectArray** ppViews) = 0;                     // 4
    STDMETHOD(GetViewsByAppUserModelId)(void* id, IObjectArray** ppViews) = 0;   // 5
    STDMETHOD(GetViewForHwnd)(void* hwnd, IApplicationView** ppView) = 0;        // 6
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

// Optional smoke-test hook: WINSPACE_FORCE_VD_VARIANT=21h2|22h2|
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
    // Takes the already-QI'd internal manager (its S_OK proved the vtable) plus the
    // ImmersiveShell service provider used to acquire the view collection. Runs
    // adoption immediately, binding pre-existing desktops to logical 1..N by GUID.
    VirtualDesktop24H2Bridge(
        Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> manager,
        Microsoft::WRL::ComPtr<IServiceProvider> shell)
        : m_manager(std::move(manager)) {
        adopt();

        // Acquire the internal IApplicationViewCollection (ADR-0010, revised): the
        // move path needs an IApplicationView for the target HWND, and only
        // GetViewForHwnd yields one. QueryService takes the same GUID for the service
        // and the interface. A failure degrades move-to-workspace to a no-op (loud
        // diagnostic); switch / enumerate are unaffected.
        auto views = ok([&](vd::IApplicationViewCollection** pp) {
            return shell->QueryService(vd::k_iidApplicationViewCollection,
                                       vd::k_iidApplicationViewCollection,
                                       reinterpret_cast<void**>(pp));
        });
        if (views) {
            m_viewCollection = std::move(*views);
        } else {
            lg::error("virtual desktop bridge: QueryService(IApplicationViewCollection) failed "
                      "— move-to-workspace disabled: {}",
                      views.error());
        }
    }

    bool switchTo(int logical) override {
        if (!m_manager) return false;

        // Hit path as an and_then chain: resolve the binding to its live desktop
        // (matched by identity, so it survives a Task View reorder), then switch.
        // A vanished — or not-yet-created — binding surfaces as NotFound, which the
        // .or_else recovers by re-materializing the workspace (ADR-0003 sparse
        // model). Only genuine OS failures reach the terminal consumer, the sole
        // logging site; NotFound is consumed silently by the recovery.
        const std::expected<Success, Error> result =
            resolveBinding(logical)
                .and_then([&](const Microsoft::WRL::ComPtr<vd::IVirtualDesktop>& desktop) {
                    return doSwitch(logical, desktop.Get());
                })
                .or_else([&](const Error& e) -> std::expected<Success, Error> {
                    if (!std::holds_alternative<NotFound>(e.code))
                        return std::unexpected(e);  // real OS failure — let it log
                    return materialize(logical);
                });

        if (!result) {
            lg::error("{}", result.error());
            return false;
        }
        return true;
    }

    int currentWorkspace() const override { return m_current; }

    // Move `window` to Logical workspace `logical` (ADR-0010, revised): resolve the
    // HWND to its IApplicationView, resolve the target desktop — materializing
    // (create + bind, NO switch) on a miss — then MoveViewToDesktop. The internal
    // path is what moves a FOREIGN window; the public HWND API cannot. Null view
    // collection (acquisition failed) → no-op.
    bool moveWindowToWorkspace(WindowId window, int logical) override {
        if (!m_manager || !m_viewCollection) return false;

        auto view = ok([&](vd::IApplicationView** pp) {
            return m_viewCollection->GetViewForHwnd(toHwnd(window), pp);
        });
        if (!view) {
            lg::error("move window to workspace {}: GetViewForHwnd: {}", logical, view.error());
            return false;
        }
        const std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> desktop =
            resolveMoveTarget(logical);
        if (!desktop) {
            lg::error("{}", desktop.error());
            return false;
        }
        if (const auto moved = ok(m_manager->MoveViewToDesktop((*view).Get(), (*desktop).Get()));
            !moved) {
            lg::error("move window to workspace {}: {}", logical, moved.error());
            return false;
        }
        return true;
    }

private:
    // Bind every pre-existing desktop to logical 1..N by GUID and seed the current
    // workspace from the active desktop (ADR-0003 startup adoption). Best-effort:
    // an enumeration failure logs once and leaves the bridge with no bindings; a
    // single unreadable desktop is skipped, as the pre-refactor filtered pipeline did.
    void adopt() {
        auto desktops = ok(
            [&](vd::IObjectArray** pp) { return m_manager->GetDesktops(pp); });
        if (!desktops) {
            lg::error("{}", desktops.error());
            return;
        }
        UINT count = 0;
        if (const auto r = ok((*desktops)->GetCount(&count)); !r) {
            lg::error("{}", r.error());
            return;
        }

        // Bind each readable desktop's GUID to its 1-based logical slot.
        const auto readBinding = [&](UINT i) -> std::optional<std::pair<int, GUID>> {
            auto desktop = ok([&](vd::IVirtualDesktop** pp) {
                return (*desktops)->GetAt(i, vd::k_iidVirtualDesktop24H2,
                                          reinterpret_cast<void**>(pp));
            });
            if (!desktop) return std::nullopt;
            GUID id{};
            if (!ok((*desktop)->GetID(&id))) return std::nullopt;
            return std::pair{static_cast<int>(i) + 1, id};
        };
        std::ranges::for_each(
            std::views::iota(0u, count) | std::views::transform(readBinding) |
                std::views::filter([](const auto& b) { return b.has_value(); }),
            [&](const auto& b) { m_logicalToGuid[b->first] = b->second; });

        auto active = ok(
            [&](vd::IVirtualDesktop** pp) { return m_manager->GetCurrentDesktop(pp); });
        GUID activeId{};
        if (active.has_value() && ok((*active)->GetID(&activeId)).has_value()) {
            const auto match = std::ranges::find_if(
                m_logicalToGuid,
                [&](const auto& entry) { return IsEqualGUID(entry.second, activeId); });
            if (match != m_logicalToGuid.end()) m_current = match->first;
        }

        lg::info(
            "virtual desktop bridge: adopted {} desktop(s); current workspace = {}", count,
            m_current);
    }

    // Map a logical workspace to its live desktop. A logical with no binding yet,
    // or one whose bound desktop has vanished, surfaces as NotFound so switchTo's
    // .or_else re-materializes both cases uniformly.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveBinding(int logical) {
        const auto it = m_logicalToGuid.find(logical);
        if (it == m_logicalToGuid.end())
            return std::unexpected(Error{NotFound{}, std::source_location::current()});
        return resolveLiveDesktop(it->second);
    }

    // Resolve a stored GUID to the live IVirtualDesktop that currently holds it, by
    // identity — not by remembered position. An enumeration failure is an Hr error;
    // a GUID that no longer names any live desktop is NotFound (routine, recoverable).
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveLiveDesktop(
        const GUID& target) {
        auto desktops = ok(
            [&](vd::IObjectArray** pp) { return m_manager->GetDesktops(pp); });
        if (!desktops) return std::unexpected(desktops.error());
        UINT count = 0;
        if (const auto r = ok((*desktops)->GetCount(&count)); !r)
            return std::unexpected(r.error());

        // Snapshot each desktop once, then match by identity; a genuine enumeration
        // error exits early rather than masquerading as the absence that would
        // spuriously re-materialize.
        const auto snapshot = std::views::iota(0u, count) |
                              std::views::transform([&](UINT i) {
                                  return ok([&](vd::IVirtualDesktop** pp) {
                                      return (*desktops)->GetAt(
                                          i, vd::k_iidVirtualDesktop24H2,
                                          reinterpret_cast<void**>(pp));
                                  });
                              }) |
                              std::ranges::to<std::vector>();
        for (const auto& desktop : snapshot) {
            if (!desktop) return std::unexpected(desktop.error());
            GUID id{};
            if (const auto r = ok((*desktop)->GetID(&id)); !r)
                return std::unexpected(r.error());
            if (IsEqualGUID(id, target)) return *desktop;
        }
        // The target GUID no longer names a live desktop — routine, recoverable.
        return std::unexpected(Error{NotFound{}, std::source_location::current()});
    }

    // Create exactly ONE desktop (appended at the tail) and bind logical→GUID,
    // WITHOUT switching. No intermediate filling, no clamp (ADR-0003). The shared
    // root of materialize (which then switches) and the move path (which needs the
    // target to exist but must not steal focus unless following) — ADR-0010.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> createAndBind(int logical) {
        auto created = ok(
            [&](vd::IVirtualDesktop** pp) { return m_manager->CreateDesktop(pp); });
        if (!created) return std::unexpected(created.error());
        GUID id{};
        if (const auto r = ok((*created)->GetID(&id)); !r)
            return std::unexpected(r.error());
        m_logicalToGuid[logical] = id;
        return *created;
    }

    // Create-and-bind, then switch — the switchTo recovery path. Recovers a NotFound
    // from switchTo by materializing the workspace and landing on it.
    std::expected<Success, Error> materialize(int logical) {
        return createAndBind(logical).and_then(
            [&](const Microsoft::WRL::ComPtr<vd::IVirtualDesktop>& desktop) {
                return doSwitch(logical, desktop.Get());
            });
    }

    // Resolve the live target desktop for a move: a live binding hands back its
    // desktop; a missing or vanished one is re-materialized (create + bind, no
    // switch). A genuine OS/enumeration error propagates unchanged.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveMoveTarget(
        int logical) {
        return resolveBinding(logical).or_else(
            [&](const Error& e) -> std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>,
                                                 Error> {
                if (!std::holds_alternative<NotFound>(e.code))
                    return std::unexpected(e);
                return createAndBind(logical);
            });
    }

    // Call SwitchDesktop and, on success, record the new current workspace. The
    // .transform maps the Success through untouched while letting any Error skip
    // straight past it to the caller's boundary consumer.
    std::expected<Success, Error> doSwitch(int logical, vd::IVirtualDesktop* desktop) {
        return ok(m_manager->SwitchDesktop(desktop)).transform([&](Success s) {
            m_current = logical;
            return s;
        });
    }

    Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> m_manager;
    Microsoft::WRL::ComPtr<vd::IApplicationViewCollection> m_viewCollection;  // HWND→view (move)
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
    const std::string buildStr = std::format("build {}.{}", build, ubr);

    auto shell = ok([&](IServiceProvider** pp) {
        return CoCreateInstance(vd::k_clsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_IServiceProvider, reinterpret_cast<void**>(pp));
    });
    if (!shell) {
        lg::error("virtual desktop bridge: CoCreateInstance(ImmersiveShell) failed — "
                  "COM VD switching disabled: {}",
                  shell.error());
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

    // Resolve a probe to its QI result plus the variant it denotes. The shared
    // 53F5CA0B IID is 24H2 only on build ≥ 26100; on 22631 it is 23H2-KB with a
    // different vtable and must not drive 24H2. WINSPACE_FORCE_VD_VARIANT overrides
    // the variant to test a stub.
    struct Resolved {
        const GUID* iid;
        std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal>, Error>
            manager;
        VdVariant variant;
    };
    const auto resolve = [&](const Probe& probe) -> Resolved {
        auto manager = ok([&](vd::IVirtualDesktopManagerInternal** pp) {
            return (*shell)->QueryService(vd::k_clsidVirtualDesktopManagerInternal,
                                          probe.iid, reinterpret_cast<void**>(pp));
        });
        VdVariant variant = probe.variant;
        if (IsEqualGUID(probe.iid, vd::k_iidVDMInternal_53F5CA0B))
            variant = (build >= 26100) ? VdVariant::W24H2 : VdVariant::W23H2KB;
        if (forced == L"21h2") variant = VdVariant::W21H2;
        else if (forced == L"22h2") variant = VdVariant::W22H2;
        else if (forced == L"23h2-kb") variant = VdVariant::W23H2KB;
        return {&probe.iid, std::move(manager), variant};
    };

    // Probe newest→oldest; the first IID whose vtable matches (QI succeeds) wins.
    auto resolved = probes | std::views::transform(resolve);
    const auto found = std::ranges::find_if(
        resolved, [](const Resolved& r) { return r.manager.has_value(); });

    // No known IID matched: a newer Windows bumped the interface. Fail LOUDLY
    // (null + diagnostic) — never call through a mismatched vtable (ADR-0002).
    if (found == std::ranges::end(resolved)) {
        lg::error(
            "virtual desktop bridge: NO known IVirtualDesktopManagerInternal IID matched ({}) "
            "— a new OS variant needs capturing; COM VD switching disabled",
            buildStr);
        return nullptr;
    }

    Resolved winner = *found;
    const std::string iidName = narrow(bridge_detail::guidToWString(*winner.iid));
    switch (winner.variant) {
        case VdVariant::W24H2:
            lg::info("virtual desktop bridge: matched IID {} ({}) — 24H2 variant", iidName,
                     buildStr);
            // The bridge needs the shell service provider too, to QueryService the
            // IApplicationViewCollection for the move path (ADR-0010, revised).
            return std::make_unique<VirtualDesktop24H2Bridge>(std::move(*winner.manager),
                                                              std::move(*shell));

        case VdVariant::W23H2KB:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 23H2-KB5034204 variant ({}) — "
                "NOT YET IMPLEMENTED (its vtable differs from 24H2); COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::W22H2:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 22H2 / pre-KB 23H2 variant ({}) — "
                "NOT YET IMPLEMENTED; COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::W21H2:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 21H2 variant ({}) — NOT YET "
                "IMPLEMENTED; COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::None:
            break;  // unreachable — a matched probe always names a variant
    }
    return nullptr;  // unreachable — the None arm never triggers on a matched probe
}

}  // namespace winspace::io
