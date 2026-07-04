# 11.01 — Error vocabulary (`io/error.cpp`)

**Labels:** `ready-for-agent`
**Blocked by:** issue 01 (the walking skeleton being refactored)

## What to build

A new `src/io/error.cpp` that is the single home for the I/O-layer error vocabulary, plus
the relocated diagnostic sink. This is the foundation the two conversion tasks build on.
No call sites change yet — this task only *introduces* the vocabulary and moves
`emitDiagnostic`.

```
namespace winspace::io {

struct HrError    { HRESULT hr; };
struct Win32Error { DWORD   code; };
struct NotFound   {};                       // semantic absence, not an OS failure

struct Error {
    std::variant<HrError, Win32Error, NotFound> code;
    std::source_location loc;
};

// free inline functions — no macros, no ?-operator
checkHr(HRESULT hr,   source_location = current()) -> std::expected<void, Error>;
checkWin32(bool ok,   source_location = current()) -> std::expected<void, Error>;  // reads GetLastError on !ok
template<class T>
hrGet(callable,       source_location = current()) -> std::expected<ComPtr<T>, Error>;  // call → out-param → reject null

formatError(const Error&) -> std::wstring;  // std::visit over detail::overload

emitDiagnostic(...)   // RELOCATED here from hotkeys.cpp (both overloads)
}
```

- `checkHr`: `SUCCEEDED(hr)` → `{}`; else `std::unexpected(Error{HrError{hr}, loc})`.
- `checkWin32`: `ok` → `{}`; else snapshot `GetLastError()` **immediately** → `Win32Error`.
- `hrGet<T>`: invoke the callable, `checkHr` its `HRESULT`, then reject a null out-param
  (collapses the `FAILED(...) || !ptr` double-check). Returns the `ComPtr<T>` on success.
- `formatError`: `std::visit` the **existing** `detail::overload` (as the Reducer does).
  Always `<file>:<line> (hr=0x…)` / `(err=…)`; then best-effort
  `FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, …)` appended
  **only when non-empty**, trailing CRLF trimmed, buffer freed via an RAII `LocalFree`
  guard. Undocumented COM codes → code-only, never garbage.
- Move `emitDiagnostic` (both overloads) out of `hotkeys.cpp` into `error.cpp`; update the
  app-TU include order so `error.cpp` precedes the adapters. `vd_bridge.cpp` will later
  `#include "io/error.cpp"` instead of `"io/hotkeys.cpp"` (done in 11.02).

## Acceptance criteria

- [ ] `src/io/error.cpp` defines `HrError`/`Win32Error`/`NotFound`/`Error`, `checkHr`/`checkWin32`/`hrGet`, and `formatError`
- [ ] `source_location` is auto-captured via a defaulted `current()` parameter on all three wrappers
- [ ] `formatError` uses `std::visit` + the existing `detail::overload`; is exhaustive over the three arms (adding an arm without a handler is a compile error)
- [ ] `FormatMessageW` text appended only when non-empty; CRLF trimmed; buffer freed via RAII (no leak)
- [ ] `emitDiagnostic` (both overloads) now lives in `error.cpp`; `hotkeys.cpp` no longer defines it
- [ ] App TU includes `error.cpp` before the adapters; builds clean under `/W4 /WX /permissive-`
- [ ] Pure test TU unchanged and still links no WM libraries; existing tests pass
