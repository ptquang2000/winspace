# PRD — Win32/COM error handling via `std::expected` + `std::variant`

**Labels:** `ready-for-agent`
**Slice:** issue 11 (see `issues/11-win32-error-handling.md`)
**Respects:** `docs/adr/0004-win32-error-handling.md` (and the existing `0002`/`0003` bridge ADRs it refactors under)
**Vocabulary:** `CONTEXT.md` + ADR-0004 (`io::Error`, `HrError`, `Win32Error`, `NotFound`, `checkHr`, `checkWin32`, `hrGet`, `formatError`, boundary consumer, seam, adapter)

## Problem Statement

As winspace's author, I'm about to integrate a large Win32 surface — `SetWindowPos`,
`DeferWindowPos`, `SetWinEventHook`, `EnumWindows`, `WM_GETMINMAXINFO` — on top of the COM
Virtual Desktop bridge already in `src/io/`. But the error handling those first adapters
established doesn't scale: every OS call is checked by hand with `FAILED(hr)` or a
`BOOL`/`GetLastError()` dance, each failure hand-writes a bespoke `emitDiagnostic` message
and then swallows the error by returning `false`/`nullptr`, and "the API failed" is
conflated with "the thing legitimately isn't there." If I copy this pattern across dozens
of new call sites I'll have hundreds of scattered logging sites, drifting messages, and a
`resolveLiveDesktop`-style ambiguity everywhere. I need one honest error idiom, proven
against the existing COM chains, before the surface multiplies.

## Solution

A single I/O-layer error vocabulary built on `std::expected<T, io::Error>`, where
`io::Error` carries a `std::variant` discriminating the two Win32 failure conventions
(`HrError` for COM `HRESULT`, `Win32Error` for `GetLastError()` `DWORD`) plus semantic
arms (`NotFound`), and an auto-captured `std::source_location`. Thin free-function wrappers
(`checkHr`, `checkWin32`, `hrGet<T>`) turn raw calls into `expected`; errors propagate
through `and_then` chains and are consumed once at a boundary per public operation, which
logs via `formatError` + `emitDiagnostic` and degrades to a no-op — exactly today's
crash-never behavior, but with one logging site instead of eleven. Absence becomes a
`NotFound` arm consumed locally by `.or_else` (recover-in-place, never logged). The whole
vocabulary is sealed inside the concrete adapters: the `IVirtualDesktopBridge` seam keeps
speaking pure winspace vocabulary (`bool`), so the abstraction is untouched. The existing
`vd_bridge.cpp`, `hotkeys.cpp`, and `worker.cpp` are converted now, becoming the copyable
exemplar every future Win32 file follows.

## User Stories

1. As winspace's author, I want one error type for both COM and classic-Win32 failures, so that I stop hand-writing two different failure checks at every OS call site.
2. As winspace's author, I want the error's `std::variant` to discriminate COM (`HRESULT`) from Win32 (`DWORD`), so that each convention is formatted with the code that actually describes it and I never lossily normalize one into the other.
3. As winspace's author, I want `checkHr(hr)` to turn an `HRESULT` into `expected<void, Error>`, so that a COM call's success/failure is one expression instead of a `FAILED` branch.
4. As winspace's author, I want `checkWin32(ok)` to snapshot `GetLastError()` itself the instant a `BOOL` call fails, so that I can't accidentally clobber the error code with an intervening call.
5. As winspace's author, I want an `hrGet<T>(callable)` helper for the ubiquitous "call method → fetch out-param pointer → reject null" pattern, so that the ~8 `FAILED(...) || !ptr` double-checks in the bridge collapse to one expression each.
6. As winspace's author, I want file:line:function captured automatically via a defaulted `source_location::current()` parameter, so that every error locates its own fault site with zero hand-written operation strings.
7. As winspace's author, I want dynamic context (which workspace) attached only where it aids diagnosis, via `transform_error`, so that I don't pay a mandatory `detail` field at every site.
8. As winspace's author, I want the wrappers to stay silent and merely return `unexpected`, so that logging is not duplicated at every intermediate call.
9. As winspace's author, I want errors to propagate through `and_then` chains, so that the happy path reads top-to-bottom and the sad path is a trailing `.or_else`.
10. As winspace's author, I want a single boundary consumer per public operation to log and degrade, so that eleven scattered `emitDiagnostic` sites collapse to a handful.
11. As winspace's author, I want "the OS call failed" and "the thing legitimately isn't there" to be distinct outcomes, so that `resolveLiveDesktop`'s `nullptr` stops meaning two different things.
12. As winspace's author, I want a vanished desktop to surface as a `NotFound` arm that `switchTo` recovers from locally by re-materializing, so that a routine cache-miss re-creates the workspace instead of logging as a fault.
13. As winspace's author, I want `NotFound` never to reach the boundary logger, so that the "did Windows break" signal in my diagnostics stays clean; an unhandled `NotFound` at the boundary is a bug and logs as one.
14. As winspace's author, I want `formatError` to render via `std::visit` over `detail::overload` (the same visitor idiom the Reducer uses), so that adding an error arm fails to compile until I handle it — exhaustiveness by construction.
15. As winspace's author, I want `formatError` to always emit `source_location` + hex and append `FormatMessageW` system text only when non-empty, so that a diagnosable message is guaranteed and undocumented COM codes degrade to code-only instead of printing garbage.
16. As winspace's author, I want the `FormatMessageW` buffer wrapped in an RAII `LocalFree` guard and its trailing CRLF trimmed, so that the enrichment path leaks nothing and reads cleanly in DebugView.
17. As winspace's author, I want `io::Error` and the wrappers to live in a new `src/io/error.cpp`, so that the error vocabulary has one home instead of being smeared across adapter files.
18. As winspace's author, I want `emitDiagnostic` relocated into `error.cpp` next to its consumer `formatError`, so that the awkward `vd_bridge.cpp` `#include "io/hotkeys.cpp"`-just-for-the-logger coupling disappears.
19. As winspace's author, I want `io::Error` to never appear in an interface signature, so that `IVirtualDesktopBridge` stays COM-free vocabulary and every future bridge impl isn't forced to traffic in `HRESULT`.
20. As winspace's author, I want `IVirtualDesktopBridge::switchTo` to keep returning `bool` and `HotkeyTable` to keep `registeredCount()`, so that the seam's contract is unchanged and `expected` is a pure implementation technique.
21. As winspace's author, I want all current OS-call sites in `vd_bridge.cpp`, `hotkeys.cpp`, and `worker.cpp` converted now, so that new Win32 code (issue 02+) has a proven, consistent exemplar rather than two coexisting styles.
22. As winspace's author, I want the degrade-don't-crash behavior preserved exactly — a failed switch no-ops, an already-registered hotkey is skipped-and-logged while the rest bind, a null bridge no-ops — so that the refactor changes structure, not runtime behavior.
23. As winspace's author, I want a future I/O-adapter error (e.g. `UnsupportedOsVariant`, `WrongThreadAffinity`) to be just a new arm on `io::Error`, so that error growth is a one-line addition the exhaustive `formatError` forces me to handle.
24. As winspace's author, I want future domain errors to stay pure in core alongside `config.cpp`'s `Diagnostic`, independent of `io::Error`, so that no `HRESULT` ever crosses the linker-enforced purity boundary into core.
25. As winspace's author, I want the code to keep compiling clean under `/W4 /WX /permissive-`, so that the refactor holds the project's zero-warning bar.
26. As winspace's author, I want the pure test TU to remain WM-library-free and unchanged, so that the core/io purity guarantee stays linker-enforced.

## Implementation Decisions

**New module — `src/io/error.cpp`** (owns the error vocabulary; `#include`d first in the app TU):
- `struct HrError { HRESULT hr; }`, `struct Win32Error { DWORD code; }`, `struct NotFound {}` (semantic absence). `struct Error { std::variant<HrError, Win32Error, NotFound> code; std::source_location loc; }`.
- Free `inline` functions in `winspace::io`, no macros, no `?`-operator:
  - `checkHr(HRESULT hr, std::source_location = current()) -> std::expected<void, Error>` — `SUCCEEDED` → `{}`, else `unexpected(Error{HrError{hr}, loc})`.
  - `checkWin32(bool ok, std::source_location = current()) -> std::expected<void, Error>` — reads `GetLastError()` internally on `!ok`.
  - `hrGet<T>(callable, std::source_location = current()) -> std::expected<Microsoft::WRL::ComPtr<T>, Error>` — runs the call, checks the `HRESULT`, rejects a null out-param.
- `formatError(const Error&) -> std::wstring` — `std::visit` over the project's existing `detail::overload`; always `<file>:<line> (hr=0x… / err=…)`, best-effort `FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, …)` text appended only when non-empty, CRLF trimmed, buffer freed via RAII.
- `emitDiagnostic` (both overloads) **moves here** from `hotkeys.cpp`.

**`vd_bridge.cpp` conversion** (the densest, ~11 sites):
- `switchTo` becomes an `and_then` chain over the hit path, with an `.or_else` that matches `NotFound` and re-materializes (the create-on-demand path). Returns `bool` at the seam.
- `resolveLiveDesktop` returns `std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error>`: `NotFound` when the GUID no longer names a live desktop, `unexpected(Hr…)` when `GetDesktops`/`GetCount` fail, value when found.
- `adopt` and `resolveLiveDesktop` **unwind their `ranges::views` pipelines to explicit loops** where a lazy `transform` cannot early-return an `unexpected`.
- `makeVirtualDesktopBridge`'s `CoCreateInstance`/`QueryService` checks route through `checkHr`/`hrGet`; the terminal per-operation logging is the boundary consumer.

**`hotkeys.cpp` conversion:**
- `RegisterHotKey` → `checkWin32`; the `ERROR_HOTKEY_ALREADY_REGISTERED` vs generic-failure distinction is made at the boundary consumer inside `HotkeyTable`'s ctor (skip-and-log preserved). `HotkeyTable` keeps `registeredCount()`; no `Error` in its public surface.

**`worker.cpp` conversion:**
- `CoInitializeEx` result routes through `checkHr`; the existing `m_comInitialized` bool is derived from the `expected`. `Worker::execute` is unchanged (the bridge still hands it `bool`).

**Sealing rule:** `io::Error` appears only inside concrete adapter implementations; it is absent from `IVirtualDesktopBridge` and any other interface. Two error tiers, never merged (ADR-0004): io-adapter errors → new `io::Error` arm; domain errors → pure core.

## Testing Decisions

- **No new test seam.** `io::Error` is I/O-only (it names `HRESULT`/`DWORD`/`GetLastError`/`FormatMessage`), so by the project's linker-enforced core/io split it belongs to the app TU only and the pure test TU cannot reach it — the same status as all existing io (`hotkeys`/`vd_bridge`/`worker`). Correctness is verified by **build-clean** (`/W4 /WX /permissive-`) + the existing task-07 integration smoke + a live run, re-run to prove behavior parity.
- **The two existing seams are untouched.** The pure Reducer (Seam 1) and config parser (Seam 2) get no changes; their Catch2 suites must still pass unmodified, and the test TU must still link **no** WM libraries — the guardrail that this refactor stayed I/O-only.
- **What a good check looks like here** = observable behavior, not internals: the six-step slice-01 smoke behaves identically post-refactor; a forced failure emits a richer-but-never-garbage `formatError` line; degrade-don't-crash still holds (already-registered hotkey skip-and-log, null-bridge no-op).
- **Prior art:** the manual six-step smoke script in `issues/01-walking-skeleton/PRD.md` / task 07 — re-used verbatim as the parity oracle. The `WINSPACE_FORCE_VD_VARIANT` hook drives the diagnostic-quality check.
- **Deferred, not built preemptively:** if an error-*classification* policy ever becomes non-trivial (e.g. "which HRESULTs are recoverable"), extract it as a pure predicate over a plain `long` and unit-test it in the core TU. Not warranted yet (ADR-0004 / Q7).

## Out of Scope

- Any new Win32 surface — `SetWindowPos`/`DeferWindowPos`, `SetWinEventHook`, `EnumWindows`, `WM_GETMINMAXINFO` (issues 02+). This slice only refactors the error handling of the code that already exists.
- The `21H2` / `23H2-KB5034204+` bridge variant implementations — still declared + loud stub; their conversion is trivial once implemented but not in this slice.
- Any change to the `IVirtualDesktopBridge` contract, the reducer/effect vocabulary, or the two existing test seams.
- Introducing an io-test tier that links WM libraries — explicitly rejected (ADR-0004): it would crack the linker-provable core/io split for a handful of thin wrappers.
- A unified cross-layer "god-error" spanning core and io — forbidden; it would drag `HRESULT`/`windows.h` into core.
- Domain (core) error modeling beyond the existing `Diagnostic` — untouched here.

## Further Notes

- **Why now, before issue 02.** The vocabulary exists to be *used going forward*; converting the existing three files first proves it against real COM chains (especially the `switchTo`/`resolveLiveDesktop` absence-vs-failure split) before it scales to dozens of `SetWindowPos` sites, and gives every future Win32 file a copyable exemplar instead of a second competing style.
- **Behavior-preserving by intent.** This is a structural refactor: same crash-never, degrade-loudly runtime behavior, better-located and better-rendered diagnostics. The parity oracle is the slice-01 smoke.
- **The `ranges` caveat is the main hazard.** `adopt`/`resolveLiveDesktop` lean on lazy `views::transform`/`filter` pipelines that can't early-return an `unexpected`; converting them means reverting those to explicit loops. This is anticipated, not a surprise — call it out in the task.
- **Extensibility is the payoff.** `NotFound` already demonstrates the semantic-arm pattern; `formatError`'s exhaustive `std::visit` turns every future arm into a compile error until handled.
