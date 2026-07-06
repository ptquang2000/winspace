# winspace — Issues

Vertical-slice (tracer-bullet) issues derived from `../DESIGN.md`. Each slice cuts
end-to-end through config → hotkey/event adapter → pure reducer → effect executor →
Win32/COM → visible result, plus reducer/parser tests. All tagged `ready-for-agent`.

**Seams:** (1) pure core reducer `reduce(state, event) → (newState, effects)` — the whole
brain, tested with zero Windows deps; (2) config parser `parse(text) → (config, diagnostics)`.
Win32/COM/DWM/hotkey/event-hook code = thin I/O adapters, verified by manual/smoke runs only.

**Tiling was dropped** ([ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md)):
winspace owns no window geometry. It switches Workspaces and switches focus; it never moves
or sizes a window. The old tiling slices (03 BSP, 04 multi-display fill, 07
togglefloat/drag/fullscreen) are deleted; slice 05 is now spatial **focus** only.

## Dependency graph

```
01 ─┬─ 05  (spatial focus — landed eligibility gate is enough)
    ├─ 06 ── 08   (rules reintroduce the hook adapter; launcher builds on it)
    ├─ 09
    └─ 10
```

`06` reintroduces the `SetWinEventHook` adapter and the `Appeared` / `Vanished` stream that
were removed from master with tiling — it is the hook's first genuine consumer.

| # | Title | Blocked by |
|---|-------|-----------|
| 05 | Spatial directional focus (`focus left/right/up/down`) | — (eligibility gate landed) |
| 06 | Move-to-workspace + place-once rules with cloak-move (reintroduces the hook) | 01 |
| 08 | Launcher: exec / exec-once with PID-match workspace assignment | 01, 06 |
| 09 | Full config grammar + reload | 01 |
| 10 | Autostart via Task Scheduler logon task | 01 |

_Landed and no longer listed: 01 (walking skeleton), 02 (window tracking + eligibility —
its positioning half is now removed, the eligibility gate survives), 11 (Win32/COM error
handling), 12 (VM seam-test harness)._
