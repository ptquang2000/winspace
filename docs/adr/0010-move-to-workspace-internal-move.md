# Move-to-workspace uses the *internal* `MoveViewToDesktop`

**Status:** Accepted (2026-07-09; **revised 2026-07-10** after VM verification — the
original "use the public interface" decision was reversed by a live `E_ACCESSDENIED`).

[ADR-0002](0002-workspaces-as-os-virtual-desktops.md) routes all Virtual Desktop
COM through the undocumented `IVirtualDesktopManagerInternal`, hand-declared
per-variant from the community RE lineage, behind `IVirtualDesktopBridge`. Moving
a window to another desktop (PRD 06's `movetoworkspace`, PRD 07's `windowrule`) is
where this ADR lives.

## Context

There are two ways to move a window to another Virtual Desktop:

- **Public** `IVirtualDesktopManager::MoveWindowToDesktop(HWND, REFGUID)`
  (`shobjidl_core.h`) — documented, stable across builds, `HWND`-native.
- **Internal** `IVirtualDesktopManagerInternal::MoveViewToDesktop(IApplicationView*,
  IVirtualDesktop*)` — takes an `IApplicationView`, not an `HWND`, so it needs a
  *second* undocumented interface, `IApplicationViewCollection::GetViewForHwnd`,
  whose IID/vtable must also be versioned per Windows build.

The first cut of this ADR chose the public API precisely to avoid that second RE
surface. **That was wrong, and a VM seam proved it.** The public
`MoveWindowToDesktop` returns `E_ACCESSDENIED (0x80070005)` for any window the
calling process does **not** own — and winspace's entire job is moving *other*
apps' windows (the foreground app, a rule-matched app). The move-to-workspace
Smoke seam (`scripts/guest/MoveToWorkspace.Tests.ps1`), driving a real
cross-process window on the 24H2 guest, failed with exactly that HRESULT on both
the move and the DWM cloak. Switching to the internal `MoveViewToDesktop` path
made the same seam pass.

The rejected option turned out to be the *only* one that works for foreign windows
— which is why the whole community lineage (VirtualDesktopAccessor, MScholtes)
uses it. The public API is for an application managing **its own** windows.

The originally-planned **cloak → move → uncloak** no-flash wrapping is also dead:
`DwmSetWindowAttribute(DWMWA_CLOAK)` is documented as valid only for windows in the
calling process and returns the same `E_ACCESSDENIED` on a foreign window. The
internal `MoveViewToDesktop` reassigns the window's desktop without ever painting
it on the current one, so no cloak is needed.

## Decision

- `IVirtualDesktopBridge` gains `bool moveWindowToWorkspace(WindowId, int logical)`,
  implemented with the **internal** interfaces:
  `IApplicationViewCollection::GetViewForHwnd(hwnd) → IApplicationView`, then
  `IVirtualDesktopManagerInternal::MoveViewToDesktop(view, desktop)`.
- Re-admit **one** more undocumented interface, `IApplicationViewCollection`
  (`k_iidApplicationViewCollection`, acquired via the ImmersiveShell
  `IServiceProvider::QueryService`). Only `GetViewForHwnd` (vtable slot 6) is
  called; the preceding slots are declared solely to fix its offset. Same
  fail-loud posture as ADR-0002: only 24H2 is wired; a QueryService failure
  degrades move-to-workspace to a no-op with a loud diagnostic.
- `MoveViewToDesktop` requires the target desktop to already exist, so the existing
  `materialize(logical)` is factored into **create-and-bind (no switch)** +
  `doSwitch`; the move path resolves the logical→live-desktop binding (ADR-0003),
  materializing without switching on a miss.
- **No DWM cloak.** The Worker converts `WindowId → HWND` (`toHwnd`), resolves the
  foreground window at execute time, and calls the bridge — nothing more. The
  no-flash property rests on the internal move being seamless, not on cloaking; it
  has no automatable observable and stays a manual smoke step.

## Considered Options

- **Public `IVirtualDesktopManager::MoveWindowToDesktop`.** *Rejected after
  testing* — `E_ACCESSDENIED` for windows the caller does not own, i.e. every
  window winspace actually moves. Zero RE surface, but it does not do the job.
- **Internal `MoveViewToDesktop` + `IApplicationViewCollection`.** *Chosen* —
  the one path that moves a foreign window. Costs a second versioned interface,
  contained the same way ADR-0002 contains the first (fail loud, 24H2-only).
- **Cloak → move → uncloak for no-flash.** *Rejected* — `DWMWA_CLOAK` is
  same-process-only; unusable on a foreign window, and unnecessary here.

## Consequences

- winspace now holds two undocumented VD interfaces behind the bridge:
  `IVirtualDesktopManagerInternal` (switch / enumerate / create / **move**) and
  `IApplicationViewCollection` (HWND→view for the move). A future IID bump can now
  affect either; both are variant-probed / fail-loud, and older builds stay stubbed.
- `IVirtualDesktopBridge` grows one method and gains a materialize-without-switch
  path; `switchTo` is unaffected.
- The move path is **not** immune to an ABI bump (the earlier draft's claimed
  benefit of the public API) — but that immunity was worthless, since the public
  call could not move the target window at all.
