# 02 — Window tracking + eligibility + fill-one-window

Vertical slice decomposed into ordered tasks. Derived from
[PRD 0002](../../docs/prd/0002-window-tracking.md); architecture in
[ADR-0006](../../docs/adr/0006-window-tracking-probe-decide-seam.md); vocabulary in
`../../CONTEXT.md` (Window tracking). All tagged `ready-for-agent`.

The slice teaches winspace to perceive and place windows for the first time: an
out-of-context `SetWinEventHook` adapter feeds lifecycle Events into the pure Reducer, which
classifies windows via the Eligibility gate and fills a single Tileable window to the focused
Display's work area, flush and DPI-correct. Ineligible windows are classified and left alone.

**Seams:** all behavior lands on Seam (1), the pure Reducer, unit-tested with synthetic
Events (no new seam). Live-only behaviors (real hook firing, flush `SetWindowPos` + DWM
frame-bounds compensation, Probe accuracy) ride the existing VM Smoke seam (ADR-0005).

## Task order

```
02.01 ─┬─ 02.02 ─┐
       └─ 02.03 ─┴─ 02.04 ── 02.05 ── 02.06
```

| # | Title | Depends on |
|---|-------|-----------|
| 02.01 | Core vocabulary: identities, Events, Effect, State | 01, 11 |
| 02.02 | Eligibility predicate + Focus order + layout + tests | 02.01 |
| 02.03 | Monitor model: `MonitorsChanged` fold + lookup + tests | 02.01 |
| 02.04 | Hook adapter thread: probe, noise gate, transport, Adoption | 02.02, 02.03 |
| 02.05 | Worker `PositionWindow`: frame-bounds + DPI compensation | 02.04 |
| 02.06 | VM Smoke-seam steps for the live behaviors | 02.05, 12 |
