# 11 — Win32/COM error handling via `std::expected` + `std::variant`

**Labels:** `ready-for-agent`
**Respects:** `docs/adr/0004-win32-error-handling.md`

## What to build

A single I/O-layer error vocabulary that replaces the ad-hoc, log-at-site-and-swallow
`FAILED(hr)` / `GetLastError()` handling scattered across `src/io/`, before the Win32
surface grows (issue 02+ brings `SetWindowPos`/`SetWinEventHook`/`EnumWindows`/…). This is
a **horizontal refactor of the walking skeleton's I/O adapters**, not a vertical feature
slice — it adds no user-visible behavior and, by design, **no new test seam**.

The shape (from ADR-0004):

```
io::Error = { std::variant<HrError, Win32Error, NotFound> code;
              std::source_location loc; }        // loc auto-captured

checkHr(HRESULT)            -> std::expected<void, Error>
checkWin32(bool ok)         -> std::expected<void, Error>   // snapshots GetLastError itself
hrGet<T>(callable)          -> std::expected<ComPtr<T>, Error>   // call → fetch out-param → reject null
formatError(const Error&)   -> std::wstring    // std::visit + detail::overload; hex+loc always,
                                               // best-effort FormatMessageW appended when non-empty
```

Wrappers stay silent and return `unexpected`; errors propagate via `and_then`; a **single
boundary consumer** per public operation calls `formatError` + `emitDiagnostic` and
degrades to a no-op — preserving today's crash-never, degrade-loudly behavior with one
logging site instead of eleven. Absence (`NotFound`) is consumed **locally** by `.or_else`
(e.g. `switchTo`'s vanished-desktop recreate) and never reaches the boundary logger.

`io::Error` is sealed inside the concrete adapters: it never enters an interface signature.
`IVirtualDesktopBridge::switchTo` keeps returning `bool`; `HotkeyTable` keeps
`registeredCount()`.

## Acceptance criteria

- [ ] New `src/io/error.cpp` holds the `io::Error` variant, `checkHr`/`checkWin32`/`hrGet`, and `formatError`; `emitDiagnostic` relocated into it
- [ ] `vd_bridge.cpp` and `hotkeys.cpp` `#include "io/error.cpp"` — the `vd_bridge → hotkeys`-for-logger include is gone
- [ ] Every `FAILED(...)`/`||!ptr`/`GetLastError` site in `vd_bridge.cpp`, `hotkeys.cpp`, `worker.cpp` converted to the wrappers; no bespoke inline error messages remain except boundary `formatError` output
- [ ] Absence vs OS-failure split: `resolveLiveDesktop` returns `expected<ComPtr, Error>` with `NotFound` for a vanished desktop, consumed by `switchTo`'s `.or_else` (never logged)
- [ ] `IVirtualDesktopBridge::switchTo` still returns `bool`; no `Error`/`HRESULT` in any interface signature
- [ ] `formatError` renders `source_location` + hex always, appends `FormatMessageW` text only when non-empty (RAII `LocalFree`, trailing CRLF trimmed)
- [ ] Builds clean under `/W4 /WX /permissive-`; the pure test TU still links **no** WM libraries and is unchanged
- [ ] Existing reducer + config-parser tests still pass unchanged

## Manual verification (I/O layer — not unit-testable)

`io::Error` is I/O-only (touches `HRESULT`/`DWORD`/`FormatMessage`), so it is verified the
same way as the rest of io — build-clean + the existing task-07 smoke, re-run to confirm
behavior is preserved:

1. **Behavior parity:** the six-step slice-01 smoke (windowless / adoption / create-on-demand / GUID-anchored stability / quit / variant diagnostic) behaves identically after the refactor.
2. **Diagnostic quality:** a forced failure (e.g. `WINSPACE_FORCE_VD_VARIANT=23h2-kb`) emits a `formatError` line carrying file:line + hex + system text — richer than the pre-refactor message, never garbage on an undocumented COM code.
3. **Degrade-don't-crash:** an already-registered hotkey still skips-and-logs while the rest bind; a null bridge still no-ops the switch.

## Blocked by

Issue 01 (walking skeleton) — the code being refactored. Should land **before** issue 02
so the new Win32 code has a consistent, copyable exemplar.
