# 11.02 — Convert the COM VD bridge

**Labels:** `ready-for-agent`
**Blocked by:** 11.01

## What to build

Convert every OS-call site in `src/io/vd_bridge.cpp` (~11) to the `error.cpp` vocabulary.
This is the densest conversion and the one that proves the absence-vs-failure split, so it
is the reference exemplar for all future Win32 code.

- **`#include`:** replace `#include "io/hotkeys.cpp"` (borrowed only for the logger) with
  `#include "io/error.cpp"`.
- **`resolveLiveDesktop`** returns `std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error>`:
  - `GetDesktops`/`GetCount` failure → `unexpected(HrError…)`
  - GUID no longer names a live desktop → `unexpected(NotFound{})`  ← routine, recoverable
  - found → the `ComPtr`
- **`switchTo`** stays `bool` at the seam. Internally: `and_then` chain over the hit path
  (`resolveLiveDesktop` → `doSwitch`), with an `.or_else` that **matches `NotFound`** and
  re-materializes (`CreateDesktop` → bind logical→GUID → switch). The terminal consumer
  logs genuine `Hr` failures via `formatError` + `emitDiagnostic` and returns `false`;
  `NotFound` is consumed by the recovery `.or_else` and **never logged**.
- **`adopt`** and **`resolveLiveDesktop`**: unwind the `ranges::views::transform`/`filter`
  pipelines to **explicit loops** where a lazy transform cannot early-return an
  `unexpected`. (This is the main hazard — expected in advance.)
- **`makeVirtualDesktopBridge`**: `CoCreateInstance`/`QueryService` route through
  `checkHr`/`hrGet`; per-operation boundary logging replaces the inline messages. The
  variant-selection diagnostics (matched IID / not-yet-implemented) are preserved.
- **`CreateDesktop`/`GetID`/`GetDesktops`/`GetAt`/`SwitchDesktop`** calls use
  `checkHr`/`hrGet`; the `FAILED(...) || !ptr` double-checks collapse into `hrGet<T>`.

Behavior must be **identical**: null bridge still no-ops, a failed switch still degrades,
the sparse GUID-anchored model (ADR-0003) is unchanged.

## Acceptance criteria

- [ ] `vd_bridge.cpp` includes `io/error.cpp`, not `io/hotkeys.cpp`
- [ ] `resolveLiveDesktop` returns `expected<ComPtr, Error>` with `NotFound` for a vanished desktop and `Hr…` for an OS failure
- [ ] `switchTo` returns `bool`; hit path is an `and_then` chain; `NotFound` recovered by `.or_else` (re-materialize) and never logged
- [ ] All `FAILED(...)`/`||!ptr`/inline-message sites converted to `checkHr`/`hrGet`; boundary consumer does the only logging
- [ ] `adopt`/`resolveLiveDesktop` range pipelines unwound to loops where needed to propagate `unexpected`
- [ ] `IVirtualDesktopBridge` signature unchanged; no `Error`/`HRESULT` escapes the concrete class
- [ ] Builds clean `/W4 /WX /permissive-`; slice-01 adoption / create-on-demand / GUID-stability smoke behaves identically
