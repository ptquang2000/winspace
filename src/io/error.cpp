// I/O-layer error vocabulary — the single home for Win32/COM failure handling.
//
// COM returns an HRESULT (FAILED(hr)); classic Win32 returns BOOL/NULL and
// reports through GetLastError() (a DWORD that must be read IMMEDIATELY, before
// any other call clobbers it). This file unifies both behind std::expected<T,
// Error>: the wrappers stay silent and return `unexpected`, errors propagate via
// and_then, and a single boundary consumer per public operation formats + logs.
//
// io::Error is an I/O implementation technique — it touches HRESULT/DWORD/
// FormatMessage and so lives in io ONLY, never entering an interface signature
// (the seam keeps speaking pure winspace vocabulary). See docs/adr/0004.
//
// The leveled diagnostic sink (lg::info/warn/error) sits here with the Error
// renderer (std::formatter<Error>); hotkeys.cpp and vd_bridge.cpp borrow it from
// this file, so it comes first in the app unity TU. Diagnostics are narrow (UTF-8)
// and go to
// stderr with ANSI level coloring; the only genuinely wide source is
// FormatMessageW's localized text, narrowed on the way out.
#pragma once

#include <windows.h>

#include <expected>
#include <format>
#include <memory>  // unique_ptr — owns FormatMessageW's LocalAlloc buffer
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>

#include <cstdio>        // std::FILE, stderr
#include <wrl/client.h>  // Microsoft::WRL::ComPtr — RAII for the COM pointers

#include "winspace/reducer.cpp"  // detail::overload — the shared exhaustive-visitor idiom

namespace winspace::io {

// ── leveled diagnostics sink ─────────────────────────────────────────────────
// One line per event, prefixed with a bold ANSI-colored level. Shared I/O sink;
// task 06 uses it for variant logging. Narrow (UTF-8) throughout.
namespace lg {

// Each level is a variadic format wrapper over a compile-time-checked format
// string, so no call site hand-writes std::format. No string_view escape hatch:
// pass a precomputed whole message as lg::info("{}", s) on the rare occasion it
// is needed.
template <class... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) {
    std::print(stderr, "\033[1m\033[34m[INFO]\033[0m {}\n",
               std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) {
    std::print(stderr, "\033[1m\033[33m[WARN]\033[0m {}\n",
               std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) {
    std::print(stderr, "\033[1m\033[31m[ERROR]\033[0m {}\n",
               std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace lg

// Widen-in-reverse: convert a wide string to UTF-8 narrow. Diagnostics are narrow
// now; the sole wide source is FormatMessageW's (possibly localized) text.
inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// UTF-8 narrow → wide (UTF-16), the inverse of narrow(). The launcher needs it to
// hand a config command line (stored as UTF-8 bytes) to CreateProcessW; no other
// caller today. Empty in → empty out (MultiByteToWideChar rejects a zero length).
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// ── the error vocabulary ─────────────────────────────────────────────────────

// The success value for the previously-void expecteds. A named empty type keeps
// the monadic chain uniform: every wrapper returns expected<T, Error>, and a
// .transform step yields a Success rather than a bare void that breaks the shape.
struct Success {};

// COM convention: a failed HRESULT.
struct HrError {
    HRESULT hr;
};

// Classic Win32 convention: the GetLastError() DWORD, snapshotted at the failure.
struct Win32Error {
    DWORD code;
};

// Semantic absence — not an OS failure (e.g. a stored GUID no longer names a
// live desktop). Consumed locally via .or_else; never reaches the boundary logger.
struct NotFound {};

// One error, tagged by its source convention, carrying the capture site for free.
// The variant discriminates the convention; loc is auto-filled at the wrap site.
struct Error {
    std::variant<HrError, Win32Error, NotFound> code;
    std::source_location loc;
};

// ── the ok() wrapper set: free inline functions, no macros, no ?-operator ────
//
// One name, three overloads dispatched by EXACT-TYPE constraints. Plain overloads
// would be ambiguous — BOOL is int, HRESULT is long, and a bare bool/int arg would
// mis-dispatch — so std::same_as pins each convention and any stray type is a hard
// compile error rather than a silent wrong pick.

// Getter param-extraction: deduce the element type T from a getter lambda whose
// sole parameter is a T**. Handles both a const and a mutable operator().
namespace error_detail {
template <class M>
struct getter_arg;
template <class C, class R, class P>
struct getter_arg<R (C::*)(P**) const> {
    using type = P;
};
template <class C, class R, class P>
struct getter_arg<R (C::*)(P**)> {
    using type = P;
};
template <class F>
using getter_element_t = typename getter_arg<decltype(&F::operator())>::type;
}  // namespace error_detail

template <class F>
concept GetterCallable = requires { typename error_detail::getter_element_t<F>; };

// COM convention: SUCCEEDED(hr) → Success; else an HrError carrying the HRESULT
// and the call site.
template <std::same_as<HRESULT> H>
[[nodiscard]] inline std::expected<Success, Error> ok(
    H hr, std::source_location loc = std::source_location::current()) {
    if (SUCCEEDED(hr)) return Success{};
    return std::unexpected(Error{HrError{hr}, loc});
}

// Classic Win32 convention: a truthy BOOL → Success; else snapshot GetLastError()
// IMMEDIATELY (before any other call can clobber it) into a Win32Error tagged with
// the call site.
template <std::same_as<BOOL> B>
[[nodiscard]] inline std::expected<Success, Error> ok(
    B okFlag, std::source_location loc = std::source_location::current()) {
    if (okFlag) return Success{};
    const DWORD code = GetLastError();
    return std::unexpected(Error{Win32Error{code}, loc});
}

// Collapse the pervasive "call a COM method, fetch its out-param, reject null"
// pattern into one expression. `call` receives a T** to fill and returns an
// HRESULT; on a FAILED hr the real HRESULT propagates, on a null-after-success
// (a contract violation) an E_POINTER does. T is deduced from the lambda's T**
// parameter, so call sites drop the explicit <T>. Returns the ComPtr<T> on success.
template <class F>
    requires GetterCallable<std::decay_t<F>>
[[nodiscard]] auto ok(F&& call, std::source_location loc = std::source_location::current())
    -> std::expected<Microsoft::WRL::ComPtr<error_detail::getter_element_t<std::decay_t<F>>>,
                     Error> {
    using T = error_detail::getter_element_t<std::decay_t<F>>;
    Microsoft::WRL::ComPtr<T> ptr;
    return ok(std::forward<F>(call)(ptr.ReleaseAndGetAddressOf()), loc)
        .and_then([&](Success) -> std::expected<Microsoft::WRL::ComPtr<T>, Error> {
            if (!ptr) return std::unexpected(Error{HrError{E_POINTER}, loc});
            return ptr;
        });
}

// ── rendering ─────────────────────────────────────────────────────────────────

namespace error_detail {

// FORMAT_MESSAGE_FROM_SYSTEM text for a code, trimmed of trailing CRLF/space and
// narrowed to UTF-8. Empty when the code is undocumented (an unknown COM code),
// so the caller degrades to code-only rather than printing garbage.
inline std::string systemMessage(DWORD code) {
    LPWSTR raw = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    // Owns the LPWSTR that FORMAT_MESSAGE_ALLOCATE_BUFFER hands back; LocalFree on
    // scope exit so the best-effort message never leaks.
    const std::unique_ptr<wchar_t, decltype([](wchar_t* p) { LocalFree(p); })> owner(raw);
    if (n == 0 || !raw) return {};  // undocumented code — no text
    std::wstring_view text(raw, n);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.remove_suffix(1);
    return narrow(text);
}

// Render a 32-bit code as 0xXXXXXXXX.
inline std::string toHex(unsigned long v) { return std::format("0x{:08X}", v); }

}  // namespace error_detail

}  // namespace winspace::io

// Render an Error in any std::format context (empty spec only), so it composes:
// bare sites read lg::error("{}", e), embedding sites lg::error("… : {}", e).
// Always <file>:<line> plus the code in its native convention (hr=0x… / err=…);
// best-effort FormatMessageW text is appended only when non-empty. std::visit
// over detail::overload is exhaustive by construction — adding a variant arm
// without a handler is a compile error.
template <>
struct std::formatter<winspace::io::Error, char> {
    constexpr auto parse(auto& ctx) { return ctx.begin(); }  // empty spec only

    auto format(const winspace::io::Error& e, auto& ctx) const {
        namespace error_detail = winspace::io::error_detail;
        const std::string where =
            std::format("{}:{}", e.loc.file_name(), e.loc.line());
        const std::string rendered = std::visit(
            winspace::detail::overload{
                [&](const winspace::io::HrError& h) {
                    const std::string out = std::format(
                        "{} (hr={})", where,
                        error_detail::toHex(static_cast<unsigned long>(h.hr)));
                    const std::string msg =
                        error_detail::systemMessage(static_cast<DWORD>(h.hr));
                    return msg.empty() ? out : std::format("{}: {}", out, msg);
                },
                [&](const winspace::io::Win32Error& w) {
                    const std::string out = std::format("{} (err={})", where, w.code);
                    const std::string msg = error_detail::systemMessage(w.code);
                    return msg.empty() ? out : std::format("{}: {}", out, msg);
                },
                [&](const winspace::io::NotFound&) {
                    return std::format("{} (not found)", where);
                },
            },
            e.code);
        return std::format_to(ctx.out(), "{}", rendered);
    }
};
