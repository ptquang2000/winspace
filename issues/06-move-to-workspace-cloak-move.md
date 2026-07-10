# 06 — Move-to-workspace command with cloak-move

**Labels:** `ready-for-agent`
**References:** [ADR-0010](../docs/adr/0010-move-to-workspace-internal-move.md) (internal VD move — the public API `E_ACCESSDENIED`s on foreign windows; revised after VM verification),
[ADR-0003](../docs/adr/0003-sparse-virtual-workspace-model.md) (sparse GUID-anchored workspaces),
[ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md) (no geometry),
`CONTEXT.md` (Windows & focus, The core seam), `DESIGN.md` §5 / §8.

> **No hook.** This slice acts on the **focused** window synchronously at keypress — the Worker
> resolves `GetForegroundWindow()` when it executes the move. The `SetWinEventHook` adapter and
> the `Appeared` / `Vanished` stream are reintroduced by the follow-on rules slice (07), which
> reuses the move machinery this slice builds. **Place-once / geometry** are not in play here:
> winspace assigns a window to a Workspace, it never sizes or positions it (ADR-0007).

## What to build

`movetoworkspace N` and `movetoworkspacesilent N` move the focused window to workspace N.
The plain form **follows** the window (switches the active desktop to N); the `silent` form
stays on the current desktop. Ungated by Eligibility — the user aimed at the foreground window
explicitly. (The originally-planned DWM cloak wrap was dropped: the internal move is seamless,
and `DWMWA_CLOAK` is same-process-only anyway — see the ADR-0010 revision below.)

This is the end-to-end "move a window to a workspace" capability and the VD-move path that the
rules slice (07) and the launcher (08) both build on.

## Design decisions

**Dispatcher & grammar.** New dispatcher `movetoworkspace` with a `bool follow`
(`movetoworkspacesilent` → `follow=false`), reusing `Bind.arg` for N. The parser is extended,
not reshaped (per its header contract with PRD 09). A missing/non-integer N yields a
`Diagnostic` and parsing continues, mirroring `workspace`.

**Event & Effects.** `movetoworkspace` becomes an Event carrying `{int logical; bool follow}`.
The Reducer emits `MoveForegroundWindowToWorkspace { int logical }`, and — only when
`follow` — additionally the existing `SwitchToWorkspace{N}`. No probe round-trip: unlike
`focus` (ADR-0008), there is no pure decision to make, so the Worker resolves the foreground
window inline at execute time (degrade-and-log if there is none).

**Bridge & internal move (ADR-0010, revised).** Add `bool moveWindowToWorkspace(WindowId, int
logical)` to `IVirtualDesktopBridge`, implemented with the **internal**
`IApplicationViewCollection::GetViewForHwnd` + `IVirtualDesktopManagerInternal::MoveViewToDesktop`
— **not** the public `IVirtualDesktopManager::MoveWindowToDesktop`, which returns
`E_ACCESSDENIED` for windows winspace does not own (proven on the VM guest). The target desktop
must exist first, so factor the existing `materialize(logical)` into **create-and-bind (no
switch)** + `doSwitch`; the move path resolves the logical→live-desktop binding, materializing
without switching on a miss. The **Worker** converts `WindowId → HWND` (`toHwnd`) and resolves
the foreground window at execute time. No DWM cloak (same-process-only, and unnecessary — the
internal move never paints the window on the current desktop).

## Acceptance criteria

- [x] `movetoworkspace N` moves the focused window to desktop N **and follows it** (active
      desktop becomes N)
- [x] `movetoworkspacesilent N` moves the focused window to desktop N but **stays** on the
      current desktop
- [x] Moving to a not-yet-created workspace N materializes it (append + bind) without switching
      to it (unless `follow`) — `createAndBind` in the bridge, driven by `resolveMoveTarget`
- [x] The command is ungated by Eligibility; no foreground window → no-op, logged
- [x] Reducer tests: a `movetoworkspace` (follow) emits `MoveForegroundWindowToWorkspace{N}`
      then `SwitchToWorkspace{N}`; the silent form emits only `MoveForegroundWindowToWorkspace{N}`
- [x] Parser tests: `movetoworkspace` / `movetoworkspacesilent` binds; malformed N →
      diagnostic + continue
- [x] VM Smoke seam (live-only, ADR-0005): a silent move to an inactive desktop reassigns the
      window to the target desktop's GUID (`Get-WindowDesktopId` Oracle) while the active desktop
      is unchanged; the cloak/uncloak wraps the move on that path. (The no-flash property itself
      is purely visual and stays a manual smoke step — `MoveToWorkspace.Tests.ps1` header.)

## Blocked by

- 01
