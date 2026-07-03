# Walking skeleton — implementation tasks

Decomposition of [`PRD.md`](./PRD.md) into dependency-ordered tasks. All carry the
`ready-for-agent` label. Respect `docs/adr/0002` (workspaces = OS Virtual Desktops,
IID-probe) and `docs/adr/0003` (sparse/virtual GUID-anchored model); use `CONTEXT.md`
vocabulary throughout.

| # | Task | Seam / layer | Blocked by |
|---|------|--------------|------------|
| 01 | [Build harness & vendored Catch2](./01-build-harness.md) | build | — |
| 02 | [Config parser + semantic types](./02-config-parser.md) | Seam 2 (pure) | 01 |
| 03 | [Pure Reducer](./03-reducer.md) | Seam 1 (pure) | 01 |
| 04 | [Windowless process + two-thread wiring](./04-process-and-threads.md) | I/O | 01, 03 |
| 05 | [Hotkey adapter + Mod/Key→Win32](./05-hotkey-adapter.md) | I/O | 02, 04 |
| 06 | [COM Virtual Desktop bridge (24H2) + sparse model](./06-com-vd-bridge.md) | I/O | 04 |
| 07 | [End-to-end integration & manual smoke](./07-integration-smoke.md) | I/O | 02–06 |

**Parallelism:** 02 and 03 are independent pure-core work once 01 lands. 05 and 06 are
independent I/O work once 04 lands. 07 closes the slice.

**Two seams only** (per grilling): the pure Reducer (tested via emitted Effects) and the
config parser (`parse→(config,diagnostics)`). Everything Win32/COM is manual smoke — the
6-step script lives in the PRD and in task 07.
