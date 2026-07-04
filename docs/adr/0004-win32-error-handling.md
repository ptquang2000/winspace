# 4. Win32/COM error handling via `std::expected` and a `std::variant` error type

**Status:** Accepted (2026-07-04)

## Context

winspace is about to grow a large Win32 surface (`SetWindowPos`/`DeferWindowPos`,
`SetWinEventHook`, `EnumWindows`, `WM_GETMINMAXINFO`, …) on top of the COM Virtual
Desktop bridge already in `src/io/`. The error handling that exists is ad-hoc and does
not scale:

- **Two failure conventions, handled by hand at every site.** COM returns an `HRESULT`
  (`FAILED(hr)`); classic Win32 returns `BOOL`/`NULL` and reports through `GetLastError()`
  (a `DWORD` that must be read *immediately*, before any other call clobbers it).
- **Log-at-site-and-swallow.** Each failure calls `emitDiagnostic` inline with a bespoke
  hand-written message, then returns `false`/`nullptr`. `vd_bridge.cpp` alone has ~11
  such sites, and the recurring `FAILED(...) || !ptr` double-check appears ~8 times.
- **Absence and failure are conflated.** `resolveLiveDesktop` returns a `ComPtr` whose
  `nullptr` means *both* "GetDesktops failed" (an OS error) and "the GUID no longer names
  a live desktop" (a routine, recoverable outcome) — the caller cannot tell them apart.
- **The diagnostic sink is misplaced.** `emitDiagnostic` lives in `hotkeys.cpp`, so
  `vd_bridge.cpp` `#include`s the *hotkey* adapter purely to borrow the logger.

The hard constraint: the core (reducer, config) is **`windows.h`-free, linker-enforced**
(the test TU links no WM libs). `HRESULT`/`DWORD`/`GetLastError`/`FormatMessage` are
strictly I/O-layer types. Any error vocabulary that touches them lives in io **only** and
must never enter core.

## Decision

Adopt `std::expected<T, io::Error>` as the return channel for Win32/COM wrappers, with a
`std::variant` error type, all sealed inside the I/O layer.

- **`io::Error`** = `{ std::variant<HrError, Win32Error, NotFound> code; std::source_location loc; }`.
  The variant discriminates the *source convention* — `HrError` carries the `HRESULT`,
  `Win32Error` carries the `GetLastError()` `DWORD` — plus semantic arms (`NotFound`).
  `source_location` is auto-captured via a defaulted `source_location::current()`
  parameter: file:line:function for free at every wrap site, no hand-written operation
  strings. Dynamic context (which workspace) is folded in only where it earns its keep,
  via `transform_error`.

- **Wrappers** — free `inline` functions in `winspace::io`, no macros, no `?`-operator:
  - `checkHr(HRESULT, source_location = current()) -> expected<void, Error>`
  - `checkWin32(bool ok, source_location = current()) -> expected<void, Error>` — snapshots
    `GetLastError()` itself when `!ok`.
  - `hrGet<T>(callable, source_location = current()) -> expected<ComPtr<T>, Error>` —
    collapses the pervasive "call method, fetch out-param, reject null" pattern into one
    expression.

- **Absence is an error arm, consumed locally (`NotFound`).** `resolveLiveDesktop` returns
  `expected<ComPtr<...>, Error>`; the vanished-desktop case is a `NotFound` that
  `switchTo` handles with `.or_else` → re-materialize → returns a fresh *success*.
  `NotFound` dies at its recovery site and **never reaches the boundary logger**; an
  unhandled `NotFound` reaching the boundary is a bug and logs as one.

- **Log at the boundary, propagate in between.** Wrappers stay silent and return
  `unexpected`; errors flow through `and_then` chains; a single boundary consumer per
  public operation calls `formatError` + `emitDiagnostic` and degrades to a no-op —
  preserving the existing crash-never, degrade-loudly behavior with one logging site
  instead of eleven.

- **`formatError`** — `std::visit` over `detail::overload` (the same visitor idiom the
  Reducer and `Worker::execute` use for `Effect`/`Event`), giving compile-time
  exhaustiveness: a new arm fails to compile until handled. Hybrid rendering: `source_location`
  + hex code **always**; best-effort `FormatMessageW(FROM_SYSTEM)` text appended **only when
  non-empty** (RAII `LocalFree` guard, trailing CRLF trimmed) so undocumented COM codes
  degrade to code-only instead of printing garbage.

- **Home:** new `src/io/error.cpp`. `emitDiagnostic` relocates into it (it sits with its
  consumer `formatError`), untangling the `vd_bridge → hotkeys`-for-logger include. Both
  adapters `#include "io/error.cpp"`, which comes first in the unity app TU.

- **The seam stays pure.** `io::Error` is an I/O *implementation technique*, sealed inside
  the concrete adapters — it never enters an interface signature. `IVirtualDesktopBridge::switchTo`
  keeps returning `bool`; `HotkeyTable` keeps exposing `registeredCount()`. The abstraction
  keeps speaking pure winspace vocabulary.

- **Migration:** convert all current io sites now — `vd_bridge.cpp` (~11 sites),
  `hotkeys.cpp` (`RegisterHotKey`), `worker.cpp` (`CoInitializeEx`). The `ranges::views`
  pipelines in `adopt`/`resolveLiveDesktop` unwind to explicit loops where a lazy
  `transform` cannot early-return an `unexpected`.

- **Testing:** `error.cpp` is io-only and, like all existing io, is **not** unit-tested —
  verified by the task-07 integration smoke + live run. Any *non-trivial* classification
  policy (e.g. "which HRESULTs are recoverable") is extracted as a pure predicate over a
  plain `long` and tested in the core TU; the tier is not built preemptively.

## Consequences

- One error idiom across the growing Win32 surface, proven against real COM chains
  (`switchTo`/`resolveLiveDesktop`) before it scales to dozens of `SetWindowPos` sites.
  The converted files become the copyable exemplar for issue 02+ code.
- Eleven scattered `emitDiagnostic` sites collapse to a handful of boundary consumers; the
  happy path reads as an `and_then` chain, the sad path as a trailing `.or_else`.
- Absence and OS-failure are finally distinct, so the boundary logger's "did Windows
  break" signal is clean.
- Auto-captured `source_location` replaces hand-tuned per-site messages; where a message
  genuinely needs dynamic context it is re-attached via `transform_error`.
- **Two error tiers, never merged.** Future *I/O-adapter* errors (e.g. `UnsupportedOsVariant`
  replacing the null-bridge dance, `WrongThreadAffinity`) become **new arms on `io::Error`**.
  Future *domain* errors stay pure in core alongside `config.cpp`'s `Diagnostic`,
  independent of `io::Error`. A single god-`Error` spanning both tiers is forbidden — it
  would drag `HRESULT`/`windows.h` across the linker-enforced purity boundary.

## Alternatives rejected

- **Normalize everything to one flat code** (`HRESULT_FROM_WIN32`, no variant) — lossy, and
  loses the ability to `FormatMessage` each convention correctly for no gain.
- **`expected<optional<T>, Error>`** to separate absence from failure by nesting — honest
  but double-unwraps at every call; folding absence into a `NotFound` arm reads cleaner
  given every producer has a local handler.
- **`?`-style macro / `TRY_HR(expr)`** — terse propagation but hides control flow and
  fights the codebase's macro-free `inline`-function style; `and_then` gives propagation
  without the hidden `return`.
- **Push `expected<void, Error>` into `IVirtualDesktopBridge`** — would leak an `HRESULT`-
  bearing io type into the seam that is deliberately COM-free vocabulary; every future
  bridge impl would be forced to traffic in it.
- **Log at the site *and* return `expected`** — two error channels that drift; the whole
  point of `expected` is to consume once, at the boundary.
- **A third "io-test" TU linking the WM libs** — establishes a whole new test tier for a
  handful of thin wrappers and cracks the deliberately linker-provable core/io test split.

## Amendment (2026-07-04): sink, rendering, and wrapper ergonomics

After building the vocabulary against the real COM chains, a review pass refined three
surfaces. These supersede the correspondingly-named pieces of the Decision above; the
substance (one error idiom, log-at-boundary, absence-as-`NotFound`, seam stays pure) is
unchanged.

- **Sink is `lg::{info,warn,error}` to stderr, not `emitDiagnostic`/`OutputDebugStringW`.**
  The single sink relocated to `error.cpp` is now a leveled, ANSI-colored, UTF-8 narrow
  API: `std::print(stderr, "\033[1m\033[3Xm[LEVEL]\033[0m {}\n", …)`. Each level is a
  **consteval variadic** wrapper — `template<class... A> void info(std::format_string<A...>, A&&...)`
  — so the format string is compile-time-checked and no call site hand-writes `std::format`.
  No `string_view` escape hatch: every site uses a literal format string, and passing a
  precomputed whole-message string is deliberately unsupported (`lg::info("{}", s)` if ever
  needed). Trade-off knowingly accepted: stderr is invisible under a normal windowless
  launch (visible only when run redirected/from a console), and localized `FormatMessageW`
  text is narrowed to UTF-8.

- **`Error` is rendered via `std::formatter<Error>`, not a free `formatError`.** The
  `std::visit`/`detail::overload` render (file:line + native code + best-effort
  `FormatMessageW`, narrowed) lives inside `std::formatter<winspace::io::Error, char>::format`;
  the `formatError` name is deleted. `Error` is now a first-class formattable, so it composes
  in any `std::format` context: bare sites read `lg::error("{}", e)`, and a message that
  embeds an error reads `lg::error("CoCreateInstance failed: {}", e)` with no manual render
  call. Rendering (formatter) and sink (`lg`) stay separate concerns.

- **The three wrappers collapse to one `ok` overload set.** `checkHr`/`checkWin32`/`hrGet<T>`
  become a single name `ok`, dispatched by **exact-type constraints** (not plain overloads,
  which are ambiguous for `BOOL` = `int` against `HRESULT` = `long`):
  - `template<std::same_as<HRESULT> H> ok(H, loc) -> expected<Success, Error>`
  - `template<std::same_as<BOOL> B> ok(B, loc) -> expected<Success, Error>` — snapshots
    `GetLastError()` immediately (the discipline the old name carried moves to a body comment).
  - `template<class F> requires GetterCallable<F> ok(F&&, loc) -> expected<ComPtr<T>, Error>`,
    where **`T` is deduced** from the getter lambda's `T**` parameter (a param-extraction
    trait), so call sites drop the explicit `<T>`.
  The exact-type constraints mean a stray `bool` (or any implicitly-convertible type) is a
  hard compile error rather than a silent mis-dispatch. Rationale for one name over three:
  requested for call-site uniformity; the intent the names carried is preserved by the
  constraints (which convention each overload accepts) and body comments. Consequence: the
  `if (const auto ok = …; !ok)` idiom renames its local (the function name `ok` would shadow
  it in its own initializer).

- **Still no io unit tests.** `lg`, the formatter, and `ok` remain io-only, verified by the
  task-07 smoke + live run, per the original Testing note.
