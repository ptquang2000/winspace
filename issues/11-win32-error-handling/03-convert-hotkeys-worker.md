# 11.03 — Convert hotkey + worker adapters

**Labels:** `ready-for-agent`
**Blocked by:** 11.01

## What to build

Convert the remaining OS-call sites in `src/io/hotkeys.cpp` and `src/io/worker.cpp` to the
`error.cpp` vocabulary. Independent of 11.02.

**`hotkeys.cpp`:**
- `emitDiagnostic` is already gone (moved in 11.01); keep the file's `#include "io/error.cpp"`.
- `RegisterHotKey` → `checkWin32`. The ctor is the **boundary consumer** for the table: on
  failure it inspects the `Win32Error` code to preserve the
  `ERROR_HOTKEY_ALREADY_REGISTERED` vs generic-failure message distinction, logs via
  `formatError`/`emitDiagnostic` (folding in the combo name via `transform_error` or at the
  log site), and **skips-and-continues** so the rest still register.
- `HotkeyTable` keeps `registeredCount()`; **no `Error` in its public surface**.

**`worker.cpp`:**
- `CoInitializeEx` → `checkHr`; derive the existing `m_comInitialized` bool from the
  `expected` (e.g. `.has_value()`), preserving the "release COM only if initialized"
  teardown.
- `Worker::execute` is **unchanged** — the bridge still returns `bool`; the seam contract
  holds.
- `RegisterClassExW`/`CreateWindowExW`: convert to `checkWin32` where a failure is
  actionable; the null-HWND-is-fatal path (published to the spine) is preserved. Don't
  over-check calls whose failure is already handled structurally.

Behavior must be **identical**: already-registered hotkey skip-and-log, other binds still
register; COM init failure still tolerated; a null worker HWND still signals fatal to the
spine.

## Acceptance criteria

- [ ] `RegisterHotKey` uses `checkWin32`; `ERROR_HOTKEY_ALREADY_REGISTERED` vs generic message distinction preserved at the boundary consumer
- [ ] `HotkeyTable` still exposes only `registeredCount()`/`eventFor()`; no `Error` in its signature; skip-and-continue behavior intact
- [ ] `CoInitializeEx` routes through `checkHr`; `m_comInitialized` derived from the `expected`; COM teardown still gated on it
- [ ] `Worker::execute` unchanged; bridge still returns `bool`
- [ ] Window-creation failure still yields a null HWND the spine treats as fatal
- [ ] Builds clean `/W4 /WX /permissive-`; hotkey registration + quit smoke behaves identically
