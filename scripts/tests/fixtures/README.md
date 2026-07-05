# Oracle decoder fixtures (12.02)

Inputs for the pure host-side decoder tests (`../Decoders.Tests.ps1`,
`../LogParser.Tests.ps1`) — run with **no VM**.

| File | What it is |
|------|-----------|
| `VirtualDesktopIDs.multi.bin`  | `VirtualDesktopIDs` REG_BINARY — three packed 16-byte GUIDs (`{111…}`, `{222…}`, `{333…}`) |
| `VirtualDesktopIDs.single.bin` | one-desktop `VirtualDesktopIDs` (16 bytes, `{111…}`) |
| `VirtualDesktopIDs.empty.bin`  | zero-length value (cold baseline) |
| `CurrentVirtualDesktop.bin`    | `CurrentVirtualDesktop` REG_BINARY — one GUID (`{222…}`, = the 2nd list entry) |
| `run.log`                      | captured stderr exercising every seam predicate (ANSI-coloured, UTF-8) |
| `formaterror-garbage.log`      | an unstructured line + a loc+hex line for an undocumented code — neither clears the `formatError` quality bar |

**Provenance.** These are hand-authored to the exact on-disk byte layout and the
exact stderr line formats winspace emits (`src/io/error.cpp` level tags +
ANSI codes; `src/io/vd_bridge.cpp` adoption/variant lines; `src/io/hotkeys.cpp`
skip line) — the GUID packing round-trips through `[guid]` the same way the real
`VirtualDesktopIDs` blob does. They were **not** captured from a live `winspace-e2e`
VM (none is provisioned in this environment). When a real 12.01 run is available,
regenerate `run.log` and the `.bin` blobs from it and re-baseline the fixed GUIDs
in `../Decoders.Tests.ps1`; the format is deliberately identical so nothing else
needs to change.
