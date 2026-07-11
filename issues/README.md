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

## Dependency graph

```
01 ─┬─ 08   (08's launcher builds on the VD move (06) and the hook/rules (07),
    │        both now landed)
    ├─ 09
    └─ 10
```

`08`'s launcher reuses the Virtual Desktop move path from `06` and the `SetWinEventHook`
adapter / `Appeared` / `Vanished` stream reintroduced by `07` — both landed, so `08` is
unblocked.

| # | Title | Blocked by |
|---|-------|-----------|
| 08 | Launcher: exec / exec-once with PID-match workspace assignment | 01, 06, 07 |
| 09 | Full config grammar + reload | 01 |
| 10 | Autostart via Task Scheduler logon task | 01 |

_Landed and no longer listed: 01 (walking skeleton), 02 (window tracking + eligibility —
its positioning half is now removed, the eligibility gate survives), 05 (spatial directional
focus), 06 (move-to-workspace via internal VD move), 07 (windowrule place-once rules —
reintroduced the hook), 11 (Win32/COM error handling), 12 (VM seam-test harness)._
