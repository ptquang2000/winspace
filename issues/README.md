# winspace — Issues

Vertical-slice (tracer-bullet) issues derived from `../DESIGN.md`. Each slice cuts
end-to-end through config → hotkey/event adapter → pure reducer → effect executor →
Win32/COM → visible result, plus reducer/parser tests. All tagged `ready-for-agent`.

**Seams:** (1) pure core reducer `reduce(state, event) → (newState, effects)` — the whole
brain, tested with zero Windows deps; (2) config parser `parse(text) → (config, diagnostics)`.
Win32/COM/DWM/hotkey/event-hook code = thin I/O adapters, verified by manual/smoke runs only.

**Tiling was dropped** ([ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md)):
winspace owns no window geometry. It switches Workspaces and switches focus; it never moves
or sizes a window. The old tiling slices (03 BSP, 04 multi-display fill, and the original 07
togglefloat/drag/fullscreen) are deleted; slice 05 was reduced to spatial **focus** only, and
has since landed. The number **07** is now reused for the windowrule slice.

## Status

**No open issues — every planned slice has landed.**

`08`'s launcher was **launch-only** (ADR-0011): it starts processes via `CreateProcess` and
does no PID matching. All Workspace placement stays with the `windowrule` path reintroduced by
`07` (landed). Now landed: the `exec` / `exec-once` parse, the `Started`/`Reloaded` →
`LaunchApp` reducer, and the `CreateProcessW` detach in the Worker.

_Landed and no longer listed: 01 (walking skeleton), 02 (window tracking + eligibility —
its positioning half is now removed, the eligibility gate survives), 05 (spatial directional
focus), 06 (move-to-workspace via internal VD move), 07 (windowrule place-once rules —
reintroduced the hook), 08 (launcher: exec / exec-once, launch-only — placement via
windowrule), 09 (full config grammar + reload — windowrule `ignore`, `start_at_login`,
removed-with-tiling diagnostics, live `reload`), 10 (autostart via per-user Task Scheduler
logon task — [ADR-0013](../docs/adr/0013-autostart-per-user-logon-task.md)), 11 (Win32/COM
error handling), 12 (VM seam-test harness)._
