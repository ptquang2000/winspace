# Win32/COM error handling — implementation tasks

Decomposition of [`PRD.md`](./PRD.md) into dependency-ordered tasks. All carry the
`ready-for-agent` label. Respects `docs/adr/0004-win32-error-handling.md`; uses `CONTEXT.md`
+ ADR-0004 vocabulary throughout.

This slice is a **horizontal refactor** of the walking skeleton's I/O adapters — it adds
**no new test seam** (`io::Error` is I/O-only, verified by build-clean + the slice-01 smoke).
The two existing seams (pure Reducer, config parser) are untouched and their suites must
still pass unmodified.

| # | Task | Layer | Blocked by |
|---|------|-------|------------|
| 01 | [Error vocabulary — `io/error.cpp`](./01-error-vocabulary.md) | I/O | — (needs slice 01 landed) |
| 02 | [Convert the COM VD bridge](./02-convert-vd-bridge.md) | I/O | 11.01 |
| 03 | [Convert hotkey + worker adapters](./03-convert-hotkeys-worker.md) | I/O | 11.01 |
| 04 | [Behavior-parity smoke + build-clean](./04-parity-smoke.md) | I/O | 11.02, 11.03 |

**Parallelism:** 02 and 03 are independent I/O work once 11.01 lands. 04 closes the slice.

```
11.01 ─┬─ 11.02 ─┐
       └─ 11.03 ─┴─ 11.04
```

**Ordering vs the roadmap:** this slice should land **before issue 02** so the growing
Win32 surface copies the new idiom rather than the ad-hoc one. It depends only on issue 01
(the code it refactors).
