# winspace — Issues

Vertical-slice (tracer-bullet) issues derived from `../DESIGN.md`. Each slice cuts
end-to-end through config → hotkey/event adapter → pure reducer → effect executor →
Win32/COM → visible result, plus reducer/parser tests. All tagged `ready-for-agent`.

**Seams:** (1) pure core reducer `reduce(state, event) → (newState, effects)` — the whole
brain, tested with zero Windows deps; (2) config parser `parse(text) → (config, diagnostics)`.
Win32/COM/DWM/hotkey/event-hook code = thin I/O adapters, verified by manual/smoke runs only.

## Dependency graph

```
01 ─┬─ 11 ── 02 ── 03 ─┬─ 04
    │                  ├─ 05
    │                  └─ 07
    ├─ 06 ── 08
    ├─ 09
    └─ 10
```

`11` is a horizontal refactor (not a vertical slice): it reworks the walking skeleton's
I/O-layer Win32/COM error handling before issue 02 grows the Win32 surface. Sequenced
before 02 so new adapters copy the new idiom.

| # | Title | Blocked by |
|---|-------|-----------|
| 01 | Walking skeleton: config-driven workspace switch | — |
| 11 | Win32/COM error handling (`std::expected` + `std::variant`) | 01 |
| 02 | Window tracking + eligibility + fill-one-window | 01 (idiom: 11) |
| 03 | BSP tiling on one display (split + reclaim) | 02 |
| 04 | Multi-display fill order + float overflow | 03 |
| 05 | Spatial directional focus + move | 03 |
| 06 | Move-to-workspace + place-once rules with cloak-move | 01, 02 |
| 07 | Place-once behaviors: togglefloat / drag-pops-to-float / fullscreen | 03 |
| 08 | Launcher: exec / exec-once with PID-match placement | 01, 06 |
| 09 | Full config grammar + reload | 01 |
| 10 | Autostart via Task Scheduler logon task | 01 |
