// winspace win32 — the entire I/O layer and the process entry point.
//
// One of the project's two production translation-unit sources, and the app TU:
// build.ps1 compiles THIS file into winspace.exe (/SUBSYSTEM:WINDOWS, windowless).
// It owns every <windows.h>/COM dependency; the pure core lives in winspace.cpp,
// #included first below (core-before-<windows.h>, matching the historical unity
// order) so the core compiles against no OS headers.
//
// Each former io/ adapter is preserved as a banner-delimited section with its
// original doc-header. Section order is DEPENDENCY order: error first (shared
// diagnostics + Error vocabulary), then the adapters, then the process spine
// (worker, then window_hook which posts to it, then app), then wWinMain. No
// domain logic lives here; behavior is the Reducer's (winspace.cpp), executed by
// the Worker.
//
// This file, compiled and linked with the WM import libraries, is the ONLY place
// OS calls are allowed. winspace.cpp must never reach the OS — see its header.
#include "winspace.cpp"  // pure core first — before any <windows.h>

#include <windows.h>

#include <dwmapi.h>      // DwmGetWindowAttribute / DWMWA_CLOAKED (probe section)
#include <lmcons.h>      // UNLEN — GetUserNameW (autostart section)
#include <objbase.h>     // CoCreateInstance / CoInitializeEx (excluded by LEAN_AND_MEAN)
#include <servprov.h>    // IServiceProvider (vd_bridge section)
#include <taskschd.h>    // ITaskService — Task Scheduler 2.0 ABI (autostart section)
#include <wrl/client.h>  // Microsoft::WRL::ComPtr — RAII for COM pointers

#include <algorithm>
#include <array>
#include <cstdio>        // std::FILE, stderr
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// [io section] error
// ─────────────────────────────────────────────────────────────────────────────

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





namespace winspace::io {

// ── leveled diagnostics sink ─────────────────────────────────────────────────
// One line per event, prefixed with a bold ANSI-colored level. Shared I/O sink
// used for variant logging. Narrow (UTF-8) throughout.
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

// Convert a wide (UTF-16) string to UTF-8. Diagnostics are narrow now; the sole
// wide source is FormatMessageW's (possibly localized) text.
inline std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// UTF-8 → wide (UTF-16), the inverse of toUtf8(). The launcher needs it to hand a
// config command line (stored as UTF-8 bytes) to CreateProcessW; no other caller
// today. Empty in → empty out (MultiByteToWideChar rejects a zero length).
inline std::wstring toWide(std::string_view s) {
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
    return toUtf8(text);
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

// ─────────────────────────────────────────────────────────────────────────────
// [io section] autostart
// ─────────────────────────────────────────────────────────────────────────────

// Autostart adapter — I/O adapter (owns <windows.h> + the Task Scheduler COM ABI).
//
// The sole executor of the SyncAutostart Effect (ADR-0013). The pure
// Reducer emits SyncAutostart{state.startAtLogin} from Started/Reloaded; the Worker
// hands the bool to syncAutostart(), which owns the ENTIRE ITaskService interaction
// so no COM chain is inlined in the Worker (mirroring vd_bridge's ownership of the
// Virtual Desktop ABI).
//
// Autostart is a per-user Task Scheduler LOGON task registered at
// `\winspace\<username>` (ADR-0013 — not a service, not a watchdog): the OS
// scheduler is the supervisor and winspace the worker it launches into the session.
// The whole path is UNPRIVILEGED — a standard user may register/remove a task that
// runs as themselves with an interactive token — which is what lets start_at_login
// toggle autostart live on reload with no elevation.
//
// Declarative, not a transition: syncAutostart(true) create-or-updates the task
// (idempotent — TASK_CREATE_OR_UPDATE never duplicates, and the exec path is
// rewritten each time so a moved binary self-heals); syncAutostart(false) deletes
// it, counting ERROR_FILE_NOT_FOUND as success so a repeated disable is a clean
// no-op. Every failure surfaces as the project Result for the Worker to degrade-log
// (ADR-0004) — autostart never blocks or crashes the WM.





namespace winspace::io {

namespace autostart_detail {

using Microsoft::WRL::ComPtr;

// The dedicated task folder and its logon-task settings (ADR-0013). The folder is
// per-machine (`\winspace`) but holds one task PER USER, named by the account, so
// two accounts on one machine never collide.
inline constexpr std::wstring_view k_folderPath = L"\\winspace";
inline constexpr std::wstring_view k_rootPath = L"\\";
inline constexpr std::wstring_view k_author = L"winspace";
inline constexpr std::wstring_view k_noTimeLimit = L"PT0S";   // ExecutionTimeLimit: unbounded
inline constexpr std::wstring_view k_restartEvery = L"PT1M";  // RestartInterval: one minute
inline constexpr LONG k_restartCount = 3;

// The Task Scheduler 2.0 signature returns an HRESULT of ERROR_FILE_NOT_FOUND when
// a task or folder is absent — the routine, expected shape for "already removed".
inline constexpr HRESULT k_fileNotFound = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

// RAII BSTR — the Task Scheduler ABI speaks BSTR for every string in/out. Frees on
// scope exit so a build with a dozen strings leaks none.
struct Bstr {
    BSTR value;
    explicit Bstr(std::wstring_view s) : value(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return value; }
};

// An empty VARIANT — the "connect to the local computer as the current user" /
// "no explicit account or SDDL" argument the ITaskService calls take by value.
inline VARIANT emptyVariant() {
    VARIANT v;
    VariantInit(&v);
    return v;
}

// True iff the Error is a COM HRESULT of ERROR_FILE_NOT_FOUND — the "absent is fine"
// case for the delete path (a missing task, or the whole `\winspace` folder gone).
inline bool isFileNotFound(const Error& e) {
    const auto* hr = std::get_if<HrError>(&e.code);
    return hr && hr->hr == k_fileNotFound;
}

// The current account name (GetUserNameW), which names this user's task inside the
// `\winspace` folder. On failure returns the Error rather than a bare name: an empty
// name would register (or try to delete) a GUID-named task at neither
// `\winspace\<username>` nor where the delete path looks, silently breaking the
// per-user-named invariant — so the caller surfaces it to degrade-log instead.
inline std::expected<std::wstring, Error> currentUserName() {
    std::wstring buf(UNLEN + 1, L'\0');  // OS writes into the returned object directly
    DWORD size = UNLEN + 1;
    if (GetUserNameW(buf.data(), &size)) {
        buf.resize(size - 1);  // size counts the null
        return buf;
    }
    const DWORD code = GetLastError();
    return std::unexpected(Error{Win32Error{code}, std::source_location::current()});
}

// domain\user for the logon trigger's UserId, so the task fires at THIS user's
// logon rather than any user's. Built from %USERDOMAIN% + the account name; falls
// back to the bare account name if the domain is unavailable.
inline std::wstring currentUserId(const std::wstring& user) {
    std::wstring domain(256, L'\0');
    const DWORD n =
        GetEnvironmentVariableW(L"USERDOMAIN", domain.data(), static_cast<DWORD>(domain.size()));
    if (n == 0 || n >= domain.size()) return user;
    domain.resize(n);  // build domain\user in place, then move out
    domain += L'\\';
    domain += user;
    return domain;
}

// The running binary's full path (GetModuleFileNameW) — the task's exec action, so
// autostart launches exactly this winspace and a moved binary self-heals on the next
// sync (the path is rewritten every create-or-update). Grows the buffer until it fits.
inline std::expected<std::wstring, Error> modulePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            const DWORD code = GetLastError();
            return std::unexpected(Error{Win32Error{code}, std::source_location::current()});
        }
        if (n < buf.size()) {  // fit (n excludes the null on success)
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);  // truncated — ERROR_INSUFFICIENT_BUFFER; grow and retry
    }
}

// Populate a fresh ITaskDefinition with the ADR-0013 logon-task shape: a logon
// trigger for `userId` with no delay; principal at limited (LUA) run level with an
// interactive token; unbounded ExecutionTimeLimit; battery guards off; restart 3×
// at one-minute intervals; and an exec action running `exePath` with no arguments.
// Each sub-interface get_ is null-checked (a null deref would crash); the scalar
// put_ setters, which act on those valid pointers, accumulate their first failure.
inline std::expected<Success, Error> buildDefinition(ITaskDefinition* def,
                                                     const std::wstring& exePath,
                                                     const std::wstring& userId) {
    // First error wins; later setters run harmlessly on valid pointers, and the
    // definition is discarded unregistered if anything failed.
    std::expected<Success, Error> result = Success{};
    const auto record = [&](std::expected<Success, Error> r) {
        if (!r && result) result = std::unexpected(r.error());
    };

    auto info = ok([&](IRegistrationInfo** pp) { return def->get_RegistrationInfo(pp); });
    if (!info) return std::unexpected(info.error());
    record(ok((*info)->put_Author(Bstr(k_author))));

    auto principal = ok([&](IPrincipal** pp) { return def->get_Principal(pp); });
    if (!principal) return std::unexpected(principal.error());
    record(ok((*principal)->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)));
    record(ok((*principal)->put_RunLevel(TASK_RUNLEVEL_LUA)));

    auto settings = ok([&](ITaskSettings** pp) { return def->get_Settings(pp); });
    if (!settings) return std::unexpected(settings.error());
    record(ok((*settings)->put_DisallowStartIfOnBatteries(VARIANT_FALSE)));
    record(ok((*settings)->put_StopIfGoingOnBatteries(VARIANT_FALSE)));
    record(ok((*settings)->put_ExecutionTimeLimit(Bstr(k_noTimeLimit))));
    record(ok((*settings)->put_RestartCount(k_restartCount)));
    record(ok((*settings)->put_RestartInterval(Bstr(k_restartEvery))));

    auto triggers = ok([&](ITriggerCollection** pp) { return def->get_Triggers(pp); });
    if (!triggers) return std::unexpected(triggers.error());
    auto trigger = ok([&](ITrigger** pp) { return (*triggers)->Create(TASK_TRIGGER_LOGON, pp); });
    if (!trigger) return std::unexpected(trigger.error());
    auto logon = ok([&](ILogonTrigger** pp) { return (*trigger)->QueryInterface(IID_PPV_ARGS(pp)); });
    if (!logon) return std::unexpected(logon.error());
    record(ok((*logon)->put_UserId(Bstr(userId))));

    auto actions = ok([&](IActionCollection** pp) { return def->get_Actions(pp); });
    if (!actions) return std::unexpected(actions.error());
    auto action = ok([&](IAction** pp) { return (*actions)->Create(TASK_ACTION_EXEC, pp); });
    if (!action) return std::unexpected(action.error());
    auto exec = ok([&](IExecAction** pp) { return (*action)->QueryInterface(IID_PPV_ARGS(pp)); });
    if (!exec) return std::unexpected(exec.error());
    record(ok((*exec)->put_Path(Bstr(exePath))));

    return result;
}

}  // namespace autostart_detail

// Make the OS logon task match `enabled` (ADR-0013), owning the entire
// ITaskService interaction. `enabled` create-or-updates `\winspace\<username>`
// (idempotent — repeated enable never duplicates, and the exec path is rewritten so
// a moved binary self-heals); `!enabled` deletes it, counting ERROR_FILE_NOT_FOUND
// (task or folder already gone) as success so a repeated disable is a clean no-op.
// Any other failure returns the Error for the Worker to degrade-log.
inline std::expected<Success, Error> syncAutostart(bool enabled) {
    using namespace autostart_detail;

    auto service = ok([&](ITaskService** pp) {
        return CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(pp));
    });
    if (!service) return std::unexpected(service.error());

    // Connect to the local Task Scheduler as the current (unelevated) user.
    if (const auto r =
            ok((*service)->Connect(emptyVariant(), emptyVariant(), emptyVariant(), emptyVariant()));
        !r)
        return std::unexpected(r.error());

    auto root = ok([&](ITaskFolder** pp) { return (*service)->GetFolder(Bstr(k_rootPath), pp); });
    if (!root) return std::unexpected(root.error());

    const std::expected<std::wstring, Error> user = currentUserName();
    if (!user) return std::unexpected(user.error());
    Bstr taskName(*user);

    if (!enabled) {
        // Resolve the `\winspace` folder; if it is absent there is nothing to delete
        // (a clean no-op). Then delete this user's task, treating an absent task as
        // success too — so a repeated disable never fails.
        auto folder =
            ok([&](ITaskFolder** pp) { return (*root)->GetFolder(Bstr(k_folderPath), pp); });
        if (!folder)
            return isFileNotFound(folder.error()) ? std::expected<Success, Error>{Success{}}
                                                  : std::unexpected(folder.error());
        if (const auto deleted = ok((*folder)->DeleteTask(taskName, 0)); !deleted)
            return isFileNotFound(deleted.error()) ? std::expected<Success, Error>{Success{}}
                                                   : std::unexpected(deleted.error());
        return Success{};
    }

    // enabled: get-or-create the `\winspace` folder. CreateFolder is idempotent-safe
    // here — a pre-existing folder surfaces as ERROR_FILE_NOT_FOUND on GetFolder only
    // when truly absent, so create only on that miss.
    ComPtr<ITaskFolder> folder;
    if (auto existing =
            ok([&](ITaskFolder** pp) { return (*root)->GetFolder(Bstr(k_folderPath), pp); })) {
        folder = std::move(*existing);
    } else if (isFileNotFound(existing.error())) {
        auto created = ok([&](ITaskFolder** pp) {
            return (*root)->CreateFolder(Bstr(k_folderPath), emptyVariant(), pp);
        });
        if (!created) return std::unexpected(created.error());
        folder = std::move(*created);
    } else {
        return std::unexpected(existing.error());
    }

    const std::expected<std::wstring, Error> exePath = modulePath();
    if (!exePath) return std::unexpected(exePath.error());

    auto def = ok([&](ITaskDefinition** pp) { return (*service)->NewTask(0, pp); });
    if (!def) return std::unexpected(def.error());
    if (const auto built = buildDefinition((*def).Get(), *exePath, currentUserId(*user)); !built)
        return std::unexpected(built.error());

    // Create-or-update under `\winspace\<username>` with an interactive token and no
    // stored credentials (empty userId/password/SDDL). TASK_CREATE_OR_UPDATE makes a
    // repeated enable rewrite the one task in place — never a duplicate.
    auto registered = ok([&](IRegisteredTask** pp) {
        return folder->RegisterTaskDefinition(taskName, (*def).Get(), TASK_CREATE_OR_UPDATE,
                                              emptyVariant(), emptyVariant(),
                                              TASK_LOGON_INTERACTIVE_TOKEN, emptyVariant(), pp);
    });
    if (!registered) return std::unexpected(registered.error());
    return Success{};
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] config_io
// ─────────────────────────────────────────────────────────────────────────────

// Config I/O helper — I/O adapter (owns <windows.h> / <filesystem>).
//
// The single home for "find the config file, read it, parse it" — the path
// resolution, file read, and parse() call that BOTH the process spine's initial
// load (io/app.cpp) and the Worker's live reload (io/worker.cpp, ADR-0012) drive.
// Extracted here (a prefactor) so neither owner duplicates the path/read/
// parse logic, and so worker.cpp — which is #included before app.cpp in the unity
// TU — can reach it.
//
// The two callers layer DIFFERENT fallback policies on top of the same read+parse
// (the asymmetry ADR-0012 records):
//   * startup   — seed the built-in default on first run, degrade per-line, and if
//                 the file is unreadable fall back to the built-in default;
//   * reload    — keep the last-good running config on ANY diagnostic or an
//                 unreadable file (atomic), applying only a clean parse.
// Both are expressed against readAndParseConfig() below; the policy lives in the
// caller, the mechanism lives here.




namespace winspace::io {

// The config winspace seeds on first run and falls back to when the on-disk file
// is unreadable. It is the Hyprland-subset grammar winspace supports; a user who
// edits the seeded file grows it from here.
inline constexpr std::string_view k_defaultConfig =
    "# winspace config — edit and save, then hit your `reload` bind (or restart).\n"
    "# $mod = ALT binds the Alt key. Alt+<key> registers on a stock Windows 11 with\n"
    "# NO policy change — unlike Win+<key>, which Windows reserves for the shell (it\n"
    "# would need the NoWinKeys registry policy just to bind). The tradeoff: as global\n"
    "# hotkeys these Alt chords shadow the focused app's own Alt+<key> shortcuts while\n"
    "# winspace runs. Rebind freely if that bites (see ADR-0014).\n"
    "$mod = ALT\n"
    "bind = $mod, 1, workspace, 1\n"
    "bind = $mod, 2, workspace, 2\n"
    "bind = $mod, 3, workspace, 3\n"
    "bind = $mod, 4, workspace, 4\n"
    "bind = $mod, 5, workspace, 5\n"
    "# Re-read this file live. Edit, save, hit the bind — hotkeys, window rules, and\n"
    "# launch entries all re-apply with no restart. A file with any error is rejected\n"
    "# whole and the last good config stays live (so a typo never breaks your setup).\n"
    "bind = $mod SHIFT, R, reload\n"
    "# Quit winspace. On $mod SHIFT (not bare $mod) so a stray Alt+Q can't kill it.\n"
    "bind = $mod SHIFT, Q, quit\n"
    "# Spatial focus: vim-style $mod + h/j/k/l steer the keyboard to the nearest window\n"
    "# in a direction. The bound key and the direction are independent fields, so\n"
    "# rebind freely (e.g. to the arrow keys).\n"
    "bind = $mod, H, focus, left\n"
    "bind = $mod, J, focus, down\n"
    "bind = $mod, K, focus, up\n"
    "bind = $mod, L, focus, right\n"
    "# Move the focused window to a workspace ($mod SHIFT + <n>, the hyprland idiom).\n"
    "# On multi-keyboard-layout machines Alt+Shift may clash with the OS layout toggle.\n"
    "# `movetoworkspacesilent` moves it but leaves you where you are; the plain\n"
    "# `movetoworkspace` would also follow the window (switch the active desktop to N).\n"
    "# A cross-desktop move is cloaked (DWM) around the move so it never flashes here.\n"
    "bind = $mod SHIFT, 1, movetoworkspacesilent, 1\n"
    "bind = $mod SHIFT, 2, movetoworkspacesilent, 2\n"
    "bind = $mod SHIFT, 3, movetoworkspacesilent, 3\n"
    "bind = $mod SHIFT, 4, movetoworkspacesilent, 4\n"
    "bind = $mod SHIFT, 5, movetoworkspacesilent, 5\n"
    "# Launch apps at startup. `exec-once` runs once when winspace starts; `exec`\n"
    "# runs at startup and again on every config reload. Each line is a verbatim\n"
    "# command (exe + args, quoted paths for spaces), started detached — it keeps\n"
    "# running after winspace exits. The launcher only STARTS apps; it never places\n"
    "# them. To put a launched app on a workspace, pair it with a `windowrule` that\n"
    "# matches the window by exe. Example: start Firefox and pin it to workspace 2.\n"
    "# exec-once = firefox\n"
    "# windowrule = workspace 2, exe:firefox.exe\n"
    "# Exclude a dock or always-on-top widget from directional focus (it is never a\n"
    "# focus target, but stays Alt-Tab reachable and is never moved or sized).\n"
    "# windowrule = ignore, class:Shell_TrayWnd\n"
    "# Start winspace with your session (registered by the logon task).\n"
    "# start_at_login = false\n";

// Expand %VAR% tokens in a template against the process environment
// (ExpandEnvironmentStringsW), size-then-fill two-call pattern: the probe returns
// the length INCLUDING the null, the fill consumes it. An UNSET variable has no
// error signal — the API leaves its %VAR% token as literal text — so a caller that
// must distinguish "unset" inspects the returned string (see configPath).
inline std::wstring expandvars(std::wstring_view tmpl) {
    const std::wstring in(tmpl);  // ExpandEnvironmentStringsW needs a null-terminated source
    const DWORD n = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (n == 0) return in;  // failure: hand back the template unchanged
    std::wstring out(n - 1, L'\0');  // n counts the terminator; string owns size()+1
    ExpandEnvironmentStringsW(in.c_str(), out.data(), n);
    return out;
}

// The known location: %USERPROFILE%\.config\winspace\winspace.conf — the Win32
// home for the Hyprland-style ~/.config/<app> layout. Empty optional when
// %USERPROFILE% is unset — expandvars leaves the token unchanged (or empty) — which
// leaves winspace on its built-in defaults.
inline std::optional<std::filesystem::path> configPath() {
    const std::wstring home = expandvars(L"%USERPROFILE%");
    if (home.empty() || home == L"%USERPROFILE%") return std::nullopt;
    return std::filesystem::path(home) / L".config" / L"winspace" / L"winspace.conf";
}

// Slurp a file's bytes verbatim (the config is UTF-8, which the parser consumes as
// bytes). Empty optional on any open failure.
inline std::optional<std::string> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return std::move(buffer).str();
}

// First-run seed: when no file exists at `path`, create the directory chain and
// write the built-in default there. Best effort — an unwritable disk logs a
// diagnostic and returns, leaving readAndParseConfig() to report the file as
// unreadable (startup then falls back to the default text). Only the seeding path
// reaches here; reload passes SeedPolicy::NoSeed so a deleted file stays deleted
// and the last good config keeps running (the ADR-0012 asymmetry).
inline void seedDefaultConfig(const std::filesystem::path& path) {
    const std::string shown = toUtf8(path.wstring());

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;

    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        lg::warn("config: cannot create {}: {}; using built-in defaults",
                 toUtf8(path.parent_path().wstring()), ec.message());
        return;
    }
    std::ofstream out(path, std::ios::binary);
    out << k_defaultConfig;
    if (!out) {
        lg::warn("config: cannot write default {}; using built-in defaults", shown);
        return;
    }
    lg::info("config: seeded default config at {}", shown);
}

// The outcome of resolving + reading + parsing the on-disk config — the shared
// mechanism both the spine and the Worker drive. `read` is false when the path
// could not be resolved (unset %USERPROFILE%) or the file could not be opened (no
// file yet, unreadable); the parse is then default-empty. Callers layer their own
// fallback: startup seeds/keeps the built-in default, reload keeps the last-good
// running config. On a successful read, `parsed` is exactly parse(fileBytes).
// (Distinct from LoadedConfig below: this is the RAW read+parse outcome; that is
// the consumable binds/rules/execs the threads borrow.)
struct ConfigReadResult {
    bool read = false;
    ParseResult parsed;
};

// Whether readAndParseConfig may seed the built-in default when the file is
// absent. Startup passes Seed (first-run creates the file); reload passes NoSeed —
// a reload of a deleted file keeps the last good config rather than resurrecting
// the default (the ADR-0012 asymmetry).
enum class SeedPolicy { Seed, NoSeed };

// Given the resolved config `path`: optionally seed the default (per policy), then
// read + parse. The one place the seed/read/parse chain lives; startup and reload
// both call it and differ only in the SeedPolicy they pass and the fallback they
// layer on the result. Path resolution (configPath) and the unset-%USERPROFILE%
// fallback belong to the callers.
inline ConfigReadResult readAndParseConfig(const std::filesystem::path& path,
                                           SeedPolicy seed) {
    if (seed == SeedPolicy::Seed) seedDefaultConfig(path);
    std::optional<std::string> text = readFile(path);
    if (!text) return {};
    return {true, parse(*text)};
}

// Both halves of one parse plus the flat setting: the Binds the Hotkey thread
// registers, the WindowRules and Launch entries the Worker seeds into State, and
// the start_at_login flag. Owned by whoever loaded it for the lifetime of the
// threads that borrow it.
struct LoadedConfig {
    std::vector<Bind> binds;
    std::vector<WindowRule> rules;
    std::vector<ExecEntry> execs;
    bool startAtLogin = false;
};

// Move a parsed Config into the LoadedConfig the threads consume.
inline LoadedConfig toLoaded(Config&& c) {
    return {std::move(c.binds), std::move(c.rules), std::move(c.execs), c.startAtLogin};
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] hotkeys
// ─────────────────────────────────────────────────────────────────────────────

// Hotkey adapter — I/O adapter, the sole owner of the Win32 MOD_*/VK_* coupling.
//
// Turns parsed Binds into live OS hotkeys via RegisterHotKey (never WH_KEYBOARD_LL
// — RegisterHotKey is kernel-delivered with no input-hook timeout) and turns each
// WM_HOTKEY back into an Event. Including <windows.h> keeps this in the app TU
// only, so the parser and Reducer it depends on stay linker-provably pure.
//
// A HotkeyTable must live on the Hotkey thread: RegisterHotKey binds each combo to
// the calling thread's queue, WM_HOTKEY is delivered there (with a null window, so
// the loop handles it inline), and the destructor unregisters on that same thread.




namespace winspace::io {

// ── Mod/Key → Win32 translation (the sole coupling point) ───────────────────

// Mod flags → RegisterHotKey's MOD_* bitmask. MOD_NOREPEAT so a held combo yields
// one Event, not an autorepeat storm at the Worker.
inline UINT toWin32Mods(Mod mods) {
    UINT out = MOD_NOREPEAT;
    if (contains(mods, Mod::Super)) out |= MOD_WIN;
    if (contains(mods, Mod::Alt)) out |= MOD_ALT;
    if (contains(mods, Mod::Ctrl)) out |= MOD_CONTROL;
    if (contains(mods, Mod::Shift)) out |= MOD_SHIFT;
    return out;
}

// Key → virtual-key code. Digit/letter/function runs are contiguous in both the
// Key enum and the VK space, so they map by offset; named keys use the switch.
inline UINT toWin32Vk(Key key) {
    const auto u = std::to_underlying(key);
    if (u >= std::to_underlying(Key::N0) && u <= std::to_underlying(Key::N9))
        return static_cast<UINT>('0' + (u - std::to_underlying(Key::N0)));
    if (u >= std::to_underlying(Key::A) && u <= std::to_underlying(Key::Z))
        return static_cast<UINT>('A' + (u - std::to_underlying(Key::A)));
    if (u >= std::to_underlying(Key::F1) && u <= std::to_underlying(Key::F24))
        return static_cast<UINT>(VK_F1 + (u - std::to_underlying(Key::F1)));
    switch (key) {
        case Key::Left: return VK_LEFT;
        case Key::Right: return VK_RIGHT;
        case Key::Up: return VK_UP;
        case Key::Down: return VK_DOWN;
        case Key::Home: return VK_HOME;
        case Key::End: return VK_END;
        case Key::PageUp: return VK_PRIOR;
        case Key::PageDown: return VK_NEXT;
        case Key::Insert: return VK_INSERT;
        case Key::Delete: return VK_DELETE;
        case Key::Return: return VK_RETURN;
        case Key::Space: return VK_SPACE;
        case Key::Tab: return VK_TAB;
        case Key::Escape: return VK_ESCAPE;
        case Key::Backspace: return VK_BACK;
        default: return 0;  // unreachable for a parsed Key; 0 fails registration loudly
    }
}

// Translate the config semantic Direction to the reducer's own Direction — the
// one place the two distinct-but-mirrored enums meet, exactly like the Mod→MOD_*
// and Dispatcher→Event translations above/below (ADR-0008).
inline reducer::Direction toReducerDir(config::Direction d) {
    switch (d) {
        case config::Direction::Left: return reducer::Direction::Left;
        case config::Direction::Right: return reducer::Direction::Right;
        case config::Direction::Up: return reducer::Direction::Up;
        case config::Direction::Down: return reducer::Direction::Down;
    }
    return reducer::Direction::Left;  // unreachable for a parsed Direction
}

// ── Bind → Event ─────────────────────────────────────────────────────────────
// The dispatcher picks the alternative; a workspace / move Bind's target is in
// arg, a focus Bind's direction is in dir. The two move forms differ only in the
// follow bit (plain → follow, silent → stay).
inline Event toEvent(const Bind& bind) {
    switch (bind.dispatcher) {
        case Dispatcher::Workspace: return WorkspaceSwitch{bind.arg};
        case Dispatcher::Quit: return Quit{};
        case Dispatcher::Focus: return FocusMove{toReducerDir(bind.dir)};
        case Dispatcher::MoveToWorkspace: return MoveToWorkspace{bind.arg, true};
        case Dispatcher::MoveToWorkspaceSilent: return MoveToWorkspace{bind.arg, false};
        case Dispatcher::Reload: return Reload{};
    }
    return Quit{};  // unreachable
}

// ── human-readable combo naming (for diagnostics) ───────────────────────────
inline std::string describeKey(Key key) {
    const auto u = std::to_underlying(key);
    if (u >= std::to_underlying(Key::N0) && u <= std::to_underlying(Key::N9))
        return std::string(1, static_cast<char>('0' + (u - std::to_underlying(Key::N0))));
    if (u >= std::to_underlying(Key::A) && u <= std::to_underlying(Key::Z))
        return std::string(1, static_cast<char>('A' + (u - std::to_underlying(Key::A))));
    if (u >= std::to_underlying(Key::F1) && u <= std::to_underlying(Key::F24))
        return std::format("F{}", 1 + (u - std::to_underlying(Key::F1)));
    switch (key) {
        case Key::Left: return "Left";
        case Key::Right: return "Right";
        case Key::Up: return "Up";
        case Key::Down: return "Down";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::Insert: return "Insert";
        case Key::Delete: return "Delete";
        case Key::Return: return "Return";
        case Key::Space: return "Space";
        case Key::Tab: return "Tab";
        case Key::Escape: return "Escape";
        case Key::Backspace: return "Backspace";
        default: return "?";
    }
}

// e.g. "SUPER+ALT+2" — names the combo in registration diagnostics.
inline std::string describeCombo(const Bind& bind) {
    std::string out;
    const auto add = [&](Mod flag, const char* name) {
        if (!contains(bind.mods, flag)) return;
        if (!out.empty()) out += "+";
        out += name;
    };
    add(Mod::Super, "SUPER");
    add(Mod::Alt, "ALT");
    add(Mod::Ctrl, "CTRL");
    add(Mod::Shift, "SHIFT");
    if (!out.empty()) out += "+";
    return out + describeKey(bind.key);
}

// ── the registration + delivery table ───────────────────────────────────────

// Registers Binds as live OS hotkeys and maps each WM_HOTKEY back to its Event.
class HotkeyTable {
    // One live OS hotkey owned by a unique_ptr over its heap id; destroying it
    // calls UnregisterHotKey once, on the registering thread (where the table lives).
    using RegisteredHotkey = std::unique_ptr<int, decltype([](int* id) {
                                                 UnregisterHotKey(nullptr, *id);
                                                 delete id;
                                             })>;

public:
    // Registers each Bind on the CURRENT thread, so WM_HOTKEY lands in this thread's
    // queue. Every return value is checked; a failure names the combo and the rest
    // still register. Hotkey id == Bind index, so a WM_HOTKEY wParam indexes m_binds.
    explicit HotkeyTable(const std::vector<Bind>& binds) : m_binds(binds) {
        for (int id = 0; id < static_cast<int>(m_binds.size()); ++id) {
            const Bind& bind = m_binds[static_cast<size_t>(id)];
            const auto registered =
                ok(RegisterHotKey(nullptr, id, toWin32Mods(bind.mods), toWin32Vk(bind.key)));
            if (registered) {
                m_registered.emplace_back(new int(id));
            } else {
                // The boundary consumer for the table: inspect the Win32 code to keep
                // the "already registered by another app" case a warn-and-skip, distinct
                // from a genuine failure, then fold the combo name in at the log site.
                // Either way we skip-and-continue so the remaining binds still register.
                const std::string combo = describeCombo(bind);
                const auto* win32 = std::get_if<Win32Error>(&registered.error().code);
                if (win32 && win32->code == ERROR_HOTKEY_ALREADY_REGISTERED) {
                    lg::warn(
                        "hotkey {} is already registered by another app — skipping", combo);
                } else {
                    lg::error("failed to register hotkey {}: {}", combo, registered.error());
                }
            }
        }
    }

    // Move-only via the RegisteredHotkey member; destroying m_registered
    // unregisters every hotkey. No explicit destructor needed.

    // Map a delivered WM_HOTKEY id (its wParam) to the Event to post; nullopt for
    // an out-of-range id (a stray message that never named one of our Binds).
    std::optional<Event> eventFor(int id) const {
        if (id < 0 || id >= static_cast<int>(m_binds.size())) return std::nullopt;
        return toEvent(m_binds[static_cast<size_t>(id)]);
    }

    // How many Binds became live hotkeys (vs. skipped on a registration failure).
    size_t registeredCount() const { return m_registered.size(); }

private:
    std::vector<Bind> m_binds;                   // index == hotkey id
    std::vector<RegisteredHotkey> m_registered;  // RegisterHotKey accepted these
};

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] probe
// ─────────────────────────────────────────────────────────────────────────────

// Window Probe — I/O adapter (owns <windows.h>). The reactive sweep behind the
// `focus` dispatcher: on a keypress the Worker runs it once, reads
// every top-level window's live attributes+rect and the foreground Origin into
// plain data, and hands that to the pure Reducer, which decides. Nothing is
// persisted and no timer runs — the adapter is idle until the next keypress
// (ADR-0006's reactive Probe, ADR-0007's stateless focus).
//
// This is the Probe half of the Probe/policy split: it gathers facts, it never
// decides. The Eligibility gate and the directional resolution both live in the
// Reducer (reducer.cpp), so this file has no policy to unit-test — its only job
// is to translate an HWND into a WindowAttrs faithfully.
//
// The WindowId ⇄ HWND mint lives here too: WindowId is an opaque uint64 minted
// from the HWND's pointer bits, and toHwnd reverses it for the
// SetForegroundWindow Effect (the named reverse-mint helper).





namespace winspace::io {

// ── WindowId ⇄ HWND (and MonitorId ← HMONITOR) mint ──────────────────────────
// The core stores these opaque identities and hands them back on Effects; only
// this adapter ever converts to/from the live handle.
inline WindowId toWindowId(HWND h) {
    return static_cast<WindowId>(reinterpret_cast<uintptr_t>(h));
}
inline HWND toHwnd(WindowId id) {
    return reinterpret_cast<HWND>(static_cast<uintptr_t>(std::to_underlying(id)));
}
inline MonitorId toMonitorId(HMONITOR m) {
    return static_cast<MonitorId>(reinterpret_cast<uintptr_t>(m));
}
inline HMONITOR toHmonitor(MonitorId id) {
    return reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(std::to_underlying(id)));
}

// Read one window's live attributes into plain data — the eligibility facts the
// Reducer's gate ANDs together, plus the rect and monitor the directional
// resolution reasons over. One read per window; no decision is made here.
inline WindowAttrs probeWindow(HWND h) {
    WindowAttrs a{};
    a.id = toWindowId(h);
    a.topLevel = GetAncestor(h, GA_ROOT) == h;
    a.visible = IsWindowVisible(h) != FALSE;

    const LONG style = GetWindowLongW(h, GWL_STYLE);
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    a.thickFrame = (style & WS_THICKFRAME) != 0;
    a.caption = (style & WS_CAPTION) == WS_CAPTION;  // WS_CAPTION = WS_BORDER|WS_DLGFRAME
    a.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0;

    int cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    a.cloaked = cloaked != 0;

    RECT r{};
    GetWindowRect(h, &r);
    a.rect = Rect{r.left, r.top, r.right, r.bottom};

    const HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    a.monitor = toMonitorId(mon);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        // Fullscreen ⇔ the window covers its monitor's full bounds (rcMonitor, not
        // the work area) — a maximized app that leaves the taskbar visible is not
        // fullscreen. Skipped as a focus target by isEligible.
        a.fullscreen = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                       r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
    }
    return a;
}

// The sweep: every top-level window, probed into plain data. EnumWindows walks
// only genuine top-level windows, so a message-only HWND (winspace's own) is
// never seen. The Reducer filters this unfiltered set through isEligible.
inline std::vector<WindowAttrs> probeTopLevelWindows() {
    std::vector<WindowAttrs> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<WindowAttrs>*>(lp)->push_back(probeWindow(h));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// Every Display, as the opaque MonitorId the core reasons over — the substrate for
// Spread's Empty-Display decision (ADR-0015). EnumDisplayMonitors walks the live
// monitor set; the callback stamps each HMONITOR into a MonitorId. No occupancy is
// read here (that is the window sweep's job); this is only the set of Displays the
// Reducer chooses among.
inline std::vector<MonitorId> enumerateDisplays() {
    std::vector<MonitorId> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<MonitorId>*>(lp)->push_back(toMonitorId(m));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// The string half of a window Probe — exe basename, window class, and title,
// narrowed to UTF-8. Called ONLY on the Appeared path (window rules
// need it); the focus sweep never calls it, so these string reads stay off the
// focus hot path. Each read degrades to an empty string on failure — a rule
// simply won't match on a field it couldn't read, never a crash.
inline WindowIdentity probeIdentity(HWND h) {
    WindowIdentity id{};
    id.id = toWindowId(h);

    // exe: the process image basename via QueryFullProcessImageNameW (works for
    // foreign processes with PROCESS_QUERY_LIMITED_INFORMATION, unlike the
    // module-handle APIs).
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        std::array<wchar_t, MAX_PATH> buf;
        DWORD len = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(proc, 0, buf.data(), &len)) {
            const std::wstring_view full(buf.data(), len);
            const size_t slash = full.find_last_of(L"\\/");
            id.exe = toUtf8(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
        }
        CloseHandle(proc);
    }

    // class: GetClassNameW into a fixed buffer (class names are short).
    std::array<wchar_t, 256> cls;
    if (const int n = GetClassNameW(h, cls.data(), static_cast<int>(cls.size())); n > 0)
        id.windowClass = toUtf8(std::wstring_view(cls.data(), static_cast<size_t>(n)));

    // title: sized by GetWindowTextLengthW, then filled (GetWindowTextW writes at
    // most size-1 chars + a null, so the buffer needs the extra slot).
    if (const int len = GetWindowTextLengthW(h); len > 0) {
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        const int got = GetWindowTextW(h, title.data(), len + 1);
        id.title = toUtf8(std::wstring_view(title.data(), static_cast<size_t>(got)));
    }
    return id;
}

// The Origin: the current foreground window as plain data, or nullopt when the
// desktop has no foreground window (e.g. only the shell is active) — which the
// Reducer treats as a no-op. The Origin's own Eligibility is never checked; it is
// only a reference rect and is excluded from its own Candidate set by WindowId.
inline std::optional<WindowAttrs> probeForeground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return std::nullopt;
    return probeWindow(fg);
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] vd_bridge
// ─────────────────────────────────────────────────────────────────────────────

// COM Virtual Desktop bridge — I/O adapter, sole owner of the undocumented
// IVirtualDesktopManagerInternal ABI. All COM lives behind IVirtualDesktopBridge
// (winspace vocabulary: logical workspace in, switch out); no COM type escapes
// this file. The Worker (sole STA owner) runs the SwitchToWorkspace Effect via
// switchTo(), oblivious to COM. See docs/adr/0002 (COM VD bridge, IID-probe
// selection) and docs/adr/0003 (sparse GUID-anchored workspaces).
//
// Variant selection is a self-validating IID probe: QI each known IID
// newest→oldest, first S_OK wins (QI-success ⟺ correct vtable). Interfaces are
// hand-declared MIDL_INTERFACE structs from the community RE lineage
// (MScholtes/VirtualDesktop VirtualDesktop11-24H2.cs, cross-checked against
// Ciantic/VirtualDesktopAccessor). Only 24H2 (build 26100) is implemented; older
// builds and any unknown IID are stubbed with a loud diagnostic — a future OS
// fails LOUDLY rather than calling through a wrong vtable.






namespace winspace::io {

// ── the seam: pure winspace vocabulary, no COM type in sight ─────────────────

// The abstraction the rest of winspace sees. Logical workspace numbers in,
// switches out. The Worker holds one of these and never learns it is COM.
class IVirtualDesktopBridge {
public:
    virtual ~IVirtualDesktopBridge() = default;

    // Switch to the Logical workspace, materializing it on demand (sparse model,
    // ADR-0003): hit → resolve the stored GUID to its live desktop → SwitchDesktop;
    // miss → create exactly one desktop (appended), bind logical→GUID, switch.
    // Returns true iff the OS desktop is now the requested workspace.
    virtual bool switchTo(int logical) = 0;

    // The Logical workspace active at startup, seeded from the OS active desktop
    // during adoption. Lets the Worker align its State with reality on boot.
    virtual int currentWorkspace() const = 0;

    // Move a window to the Logical workspace, materializing the target on demand
    // WITHOUT switching to it (ADR-0010, revised). Implemented with the INTERNAL
    // IApplicationViewCollection::GetViewForHwnd + MoveViewToDesktop — the public
    // IVirtualDesktopManager::MoveWindowToDesktop returns E_ACCESSDENIED for windows
    // the caller does not own, which is precisely winspace's case (a foreground app
    // window from another process). Returns true iff the move landed.
    virtual bool moveWindowToWorkspace(WindowId window, int logical) = 0;
};

// ── hand-declared undocumented COM ABI (RE lineage; see file header) ─────────
//
// Namespaced so these names can't collide with SDK headers. Methods are
// stdcall-returning-HRESULT; the C# sources' by-value returns become trailing
// out-params here. Params we never call through (IApplicationView*, HSTRING) are
// pointer-width `void*` — ABI-identical, keeps the vtable slots aligned.
namespace vd {

// CLSIDs — stable across all Windows 11 builds.
inline constexpr GUID k_clsidImmersiveShell = {
    0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
inline constexpr GUID k_clsidVirtualDesktopManagerInternal = {
    0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};

// IID_IApplicationViewCollection — the service id AND interface id (QueryService
// uses the same GUID for both). Stable across Win10/11 builds. GetViewForHwnd on
// this turns an HWND into the IApplicationView that MoveViewToDesktop consumes,
// which is how a FOREIGN window is moved (the public MoveWindowToDesktop returns
// E_ACCESSDENIED for windows the caller does not own — see ADR-0010 revision).
inline constexpr GUID k_iidApplicationViewCollection = {
    0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

// IID_IVirtualDesktop — 24H2 / build 26100 (VirtualDesktop11-24H2.cs). Passed to
// IObjectArray::GetAt to fetch each desktop as this element type.
inline constexpr GUID k_iidVirtualDesktop24H2 = {
    0x3F07F4BE, 0xB107, 0x441A, {0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C}};

// IID_IVirtualDesktopManagerInternal by variant, newest→oldest (the probe order).
// NOTE: {53F5CA0B-...} is shared by 24H2 (26100) AND post-KB5034204 23H2 (22631)
// — same IID, DIFFERENT vtable (24H2 inserts SwitchDesktopAndMoveForegroundView
// at slot 10). IID-probe therefore cannot tell them apart; the factory gates on
// the OS build number to fail closed on 23H2 rather than call a shifted vtable.
inline constexpr GUID k_iidVDMInternal_53F5CA0B = {  // 24H2 (26100) & 23H2-KB (22631)
    0x53F5CA0B, 0x158F, 0x4124, {0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27}};
inline constexpr GUID k_iidVDMInternal_A3175F2D = {  // 22H2 / pre-KB 23H2
    0xA3175F2D, 0x239C, 0x4BD2, {0x8A, 0xA0, 0xEE, 0xBA, 0x8B, 0x0B, 0x13, 0x8E}};
inline constexpr GUID k_iidVDMInternal_B2F925B9 = {  // 21H2 (22000)
    0xB2F925B9, 0x5A0F, 0x4D2E, {0x9F, 0x4D, 0x2B, 0x15, 0x07, 0x59, 0x3C, 0x10}};

MIDL_INTERFACE("FF72FFDD-BE7E-43FC-9C03-AD81681E88E4")
IApplicationView : public IUnknown{};  // opaque — only ever passed as a pointer

// IVirtualDesktop — 24H2 vtable (VirtualDesktop11-24H2.cs). We call only GetID.
MIDL_INTERFACE("3F07F4BE-B107-441A-AF0F-39D82529072C")
IVirtualDesktop : public IUnknown {
    STDMETHOD(IsViewVisible)(void* pView, int* pfVisible) = 0;  // slot 3
    STDMETHOD(GetID)(GUID* pGuid) = 0;                          // slot 4
    STDMETHOD(GetName)(void* pName) = 0;                        // slot 5 (HSTRING)
    STDMETHOD(GetWallpaperPath)(void* pPath) = 0;               // slot 6 (HSTRING)
    STDMETHOD(IsRemote)(int* pfRemote) = 0;                     // slot 7
};

// IObjectArray — stable. We call GetCount and GetAt.
MIDL_INTERFACE("92CA9DCD-5622-4BBA-A805-5E9F541BD8C9")
IObjectArray : public IUnknown {
    STDMETHOD(GetCount)(UINT* pcObjects) = 0;                              // slot 3
    STDMETHOD(GetAt)(UINT uiIndex, REFIID riid, void** ppv) = 0;          // slot 4
};

// IVirtualDesktopManagerInternal — 24H2 vtable (VirtualDesktop11-24H2.cs),
// transcribed verbatim slot-for-slot. We call GetCurrentDesktop, GetDesktops,
// SwitchDesktop, and CreateDesktop; the rest are declared only to keep the
// vtable layout exact. On 24H2, SwitchDesktop/CreateDesktop take NO leading
// monitor/view parameter.
MIDL_INTERFACE("53F5CA0B-158F-4124-900C-057158060B27")
IVirtualDesktopManagerInternal : public IUnknown {
    STDMETHOD(GetCount)(UINT* pCount) = 0;                                       // 3
    STDMETHOD(MoveViewToDesktop)(void* pView, IVirtualDesktop* pDesktop) = 0;    // 4
    STDMETHOD(CanViewMoveDesktops)(void* pView, int* pfCanMove) = 0;             // 5
    STDMETHOD(GetCurrentDesktop)(IVirtualDesktop** ppDesktop) = 0;               // 6
    STDMETHOD(GetDesktops)(IObjectArray** ppDesktops) = 0;                       // 7
    STDMETHOD(GetAdjacentDesktop)(IVirtualDesktop* pFrom, int direction,
                                  IVirtualDesktop** ppDesktop) = 0;              // 8
    STDMETHOD(SwitchDesktop)(IVirtualDesktop* pDesktop) = 0;                     // 9
    STDMETHOD(SwitchDesktopAndMoveForegroundView)(IVirtualDesktop* pD) = 0;      // 10
    STDMETHOD(CreateDesktop)(IVirtualDesktop** ppNewDesktop) = 0;                // 11
    STDMETHOD(MoveDesktop)(IVirtualDesktop* pDesktop, int nIndex) = 0;           // 12
    STDMETHOD(RemoveDesktop)(IVirtualDesktop* pRemove,
                             IVirtualDesktop* pFallback) = 0;                    // 13
    STDMETHOD(FindDesktop)(GUID* pId, IVirtualDesktop** ppDesktop) = 0;          // 14
    STDMETHOD(GetDesktopSwitchIncludeExcludeViews)(IVirtualDesktop* pDesktop,
                                                   IObjectArray** ppIn,
                                                   IObjectArray** ppOut) = 0;    // 15
    STDMETHOD(SetDesktopName)(IVirtualDesktop* pDesktop, void* name) = 0;        // 16
    STDMETHOD(SetDesktopWallpaper)(IVirtualDesktop* pDesktop, void* path) = 0;   // 17
    STDMETHOD(UpdateWallpaperPathForAllDesktops)(void* path) = 0;                // 18
    STDMETHOD(CopyDesktopState)(void* pView0, void* pView1) = 0;                 // 19
    STDMETHOD(CreateRemoteDesktop)(void* path, IVirtualDesktop** ppDesktop) = 0; // 20
    STDMETHOD(SwitchRemoteDesktop)(IVirtualDesktop* pDesktop, void* type) = 0;   // 21
    STDMETHOD(SwitchDesktopWithAnimation)(IVirtualDesktop* pDesktop) = 0;        // 22
    STDMETHOD(GetLastActiveDesktop)(IVirtualDesktop** ppDesktop) = 0;            // 23
    STDMETHOD(WaitForAnimationToComplete)() = 0;                                 // 24
};

// IApplicationViewCollection — the undocumented HWND→IApplicationView resolver
// (MScholtes lineage). Only GetViewForHwnd (slot 6) is called; slots 3–5 are
// declared solely to fix its vtable offset (params we never pass are void*,
// ABI-identical). This is the second internal interface ADR-0010 was revised to
// re-admit: MoveViewToDesktop needs an IApplicationView, and only this yields one
// from an HWND — the price of moving windows the caller does not own.
MIDL_INTERFACE("1841C6D7-4F9D-42C0-AF41-8747538F10E5")
IApplicationViewCollection : public IUnknown {
    STDMETHOD(GetViews)(IObjectArray** ppViews) = 0;                             // 3
    STDMETHOD(GetViewsByZOrder)(IObjectArray** ppViews) = 0;                     // 4
    STDMETHOD(GetViewsByAppUserModelId)(void* id, IObjectArray** ppViews) = 0;   // 5
    STDMETHOD(GetViewForHwnd)(void* hwnd, IApplicationView** ppView) = 0;        // 6
};

}  // namespace vd

// ── variant identity + diagnostics ──────────────────────────────────────────

namespace bridge_detail {

// Which RE-captured variant an IID probe (plus build gate) resolves to.
enum class VdVariant {
    None,     // no known IID matched — fail closed
    W24H2,    // 24H2 / build 26100 — the one implemented + verified here
    W23H2KB,  // post-KB5034204 23H2 — shares 24H2's IID, different vtable: stubbed
    W22H2,    // 22H2 / pre-KB 23H2 — stubbed
    W21H2,    // 21H2 / build 22000 — stubbed
};

// Format a GUID as the canonical "{XXXXXXXX-...}" string for diagnostics.
inline std::wstring guidToWString(const GUID& g) {
    std::wstring buf(40, L'\0');  // "{8-4-4-4-12}" is 38 chars + braces + null
    const int n = StringFromGUID2(g, buf.data(), static_cast<int>(buf.size()));
    buf.resize(n > 0 ? static_cast<size_t>(n) - 1 : 0);  // n counts the null terminator
    return buf;
}

// Read HKLM CurrentBuildNumber (e.g. 26100); 0 if unreadable. Diagnostic-only
// per ADR-0002, plus the build gate that separates 24H2 from 23H2-KB.
inline DWORD readBuildNumber() {
    std::wstring buf(32, L'\0');
    DWORD cb = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buf.data(),
                     &cb) == ERROR_SUCCESS) {
        return static_cast<DWORD>(wcstoul(buf.c_str(), nullptr, 10));
    }
    return 0;
}

// Read HKLM UBR (the .NNNN update-build-revision); 0 if unreadable. Diagnostic-only.
inline DWORD readUbr() {
    DWORD ubr = 0;
    DWORD cb = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
                 RRF_RT_REG_DWORD, nullptr, &ubr, &cb);
    return ubr;
}

// Optional smoke-test hook: WINSPACE_FORCE_VD_VARIANT=21h2|22h2|
// 23h2-kb forces selection of a stubbed variant so its "not yet implemented"
// diagnostic can be observed on the 24H2 dev machine. Unset → normal probing.
inline std::wstring forcedVariantOverride() {
    std::wstring buf(32, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"WINSPACE_FORCE_VD_VARIANT", buf.data(),
                                            static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};  // unset, or too long for the buffer
    buf.resize(n);
    return buf;
}

}  // namespace bridge_detail

// ── the 24H2 implementation ──────────────────────────────────────────────────

// GUID-anchored sparse bridge (ADR-0003). Owns the logical→GUID map, adoption,
// create-on-demand, and live GUID→desktop resolution — none of which the pure
// reducer ever sees. Constructed on (and destroyed on) the Worker's STA thread,
// so every COM pointer here is touched only by its apartment owner.
class VirtualDesktop24H2Bridge final : public IVirtualDesktopBridge {
public:
    // Takes the already-QI'd manager (its S_OK proved the vtable) and runs
    // adoption immediately, binding pre-existing desktops to logical 1..N by GUID.
    // Takes the already-QI'd internal manager (its S_OK proved the vtable) plus the
    // ImmersiveShell service provider used to acquire the view collection. Runs
    // adoption immediately, binding pre-existing desktops to logical 1..N by GUID.
    VirtualDesktop24H2Bridge(
        Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> manager,
        Microsoft::WRL::ComPtr<IServiceProvider> shell)
        : m_manager(std::move(manager)) {
        adopt();

        // Acquire the internal IApplicationViewCollection (ADR-0010, revised): the
        // move path needs an IApplicationView for the target HWND, and only
        // GetViewForHwnd yields one. QueryService takes the same GUID for the service
        // and the interface. A failure degrades move-to-workspace to a no-op (loud
        // diagnostic); switch / enumerate are unaffected.
        auto views = ok([&](vd::IApplicationViewCollection** pp) {
            return shell->QueryService(vd::k_iidApplicationViewCollection,
                                       vd::k_iidApplicationViewCollection,
                                       reinterpret_cast<void**>(pp));
        });
        if (views) {
            m_viewCollection = std::move(*views);
        } else {
            lg::error("virtual desktop bridge: QueryService(IApplicationViewCollection) failed "
                      "— move-to-workspace disabled: {}",
                      views.error());
        }
    }

    bool switchTo(int logical) override {
        if (!m_manager) return false;

        // Hit path as an and_then chain: resolve the binding to its live desktop
        // (matched by identity, so it survives a Task View reorder), then switch.
        // A vanished — or not-yet-created — binding surfaces as NotFound, which the
        // .or_else recovers by re-materializing the workspace (ADR-0003 sparse
        // model). Only genuine OS failures reach the terminal consumer, the sole
        // logging site; NotFound is consumed silently by the recovery.
        const std::expected<Success, Error> result =
            resolveBinding(logical)
                .and_then([&](const Microsoft::WRL::ComPtr<vd::IVirtualDesktop>& desktop) {
                    return doSwitch(logical, desktop.Get());
                })
                .or_else([&](const Error& e) -> std::expected<Success, Error> {
                    if (!std::holds_alternative<NotFound>(e.code))
                        return std::unexpected(e);  // real OS failure — let it log
                    return materialize(logical);
                });

        if (!result) {
            lg::error("{}", result.error());
            return false;
        }
        return true;
    }

    int currentWorkspace() const override { return m_current; }

    // Move `window` to Logical workspace `logical` (ADR-0010, revised): resolve the
    // HWND to its IApplicationView, resolve the target desktop — materializing
    // (create + bind, NO switch) on a miss — then MoveViewToDesktop. The internal
    // path is what moves a FOREIGN window; the public HWND API cannot. Null view
    // collection (acquisition failed) → no-op.
    bool moveWindowToWorkspace(WindowId window, int logical) override {
        if (!m_manager || !m_viewCollection) return false;

        auto view = ok([&](vd::IApplicationView** pp) {
            return m_viewCollection->GetViewForHwnd(toHwnd(window), pp);
        });
        if (!view) {
            lg::error("move window to workspace {}: GetViewForHwnd: {}", logical, view.error());
            return false;
        }
        const std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> desktop =
            resolveMoveTarget(logical);
        if (!desktop) {
            lg::error("{}", desktop.error());
            return false;
        }
        if (const auto moved = ok(m_manager->MoveViewToDesktop((*view).Get(), (*desktop).Get()));
            !moved) {
            lg::error("move window to workspace {}: {}", logical, moved.error());
            return false;
        }
        return true;
    }

private:
    // Bind every pre-existing desktop to logical 1..N by GUID and seed the current
    // workspace from the active desktop (ADR-0003 startup adoption). Best-effort:
    // an enumeration failure logs once and leaves the bridge with no bindings; a
    // single unreadable desktop is skipped, as the pre-refactor filtered pipeline did.
    void adopt() {
        auto desktops = ok(
            [&](vd::IObjectArray** pp) { return m_manager->GetDesktops(pp); });
        if (!desktops) {
            lg::error("{}", desktops.error());
            return;
        }
        UINT count = 0;
        if (const auto r = ok((*desktops)->GetCount(&count)); !r) {
            lg::error("{}", r.error());
            return;
        }

        // Bind each readable desktop's GUID to its 1-based logical slot.
        const auto readBinding = [&](UINT i) -> std::optional<std::pair<int, GUID>> {
            auto desktop = ok([&](vd::IVirtualDesktop** pp) {
                return (*desktops)->GetAt(i, vd::k_iidVirtualDesktop24H2,
                                          reinterpret_cast<void**>(pp));
            });
            if (!desktop) return std::nullopt;
            GUID id{};
            if (!ok((*desktop)->GetID(&id))) return std::nullopt;
            return std::pair{static_cast<int>(i) + 1, id};
        };
        std::ranges::for_each(
            std::views::iota(0u, count) | std::views::transform(readBinding) |
                std::views::filter([](const auto& b) { return b.has_value(); }),
            [&](const auto& b) { m_logicalToGuid[b->first] = b->second; });

        auto active = ok(
            [&](vd::IVirtualDesktop** pp) { return m_manager->GetCurrentDesktop(pp); });
        GUID activeId{};
        if (active.has_value() && ok((*active)->GetID(&activeId)).has_value()) {
            const auto match = std::ranges::find_if(
                m_logicalToGuid,
                [&](const auto& entry) { return IsEqualGUID(entry.second, activeId); });
            if (match != m_logicalToGuid.end()) m_current = match->first;
        }

        lg::info(
            "virtual desktop bridge: adopted {} desktop(s); current workspace = {}", count,
            m_current);
    }

    // Map a logical workspace to its live desktop. A logical with no binding yet,
    // or one whose bound desktop has vanished, surfaces as NotFound so switchTo's
    // .or_else re-materializes both cases uniformly.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveBinding(int logical) {
        const auto it = m_logicalToGuid.find(logical);
        if (it == m_logicalToGuid.end())
            return std::unexpected(Error{NotFound{}, std::source_location::current()});
        return resolveLiveDesktop(it->second);
    }

    // Resolve a stored GUID to the live IVirtualDesktop that currently holds it, by
    // identity — not by remembered position. An enumeration failure is an Hr error;
    // a GUID that no longer names any live desktop is NotFound (routine, recoverable).
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveLiveDesktop(
        const GUID& target) {
        auto desktops = ok(
            [&](vd::IObjectArray** pp) { return m_manager->GetDesktops(pp); });
        if (!desktops) return std::unexpected(desktops.error());
        UINT count = 0;
        if (const auto r = ok((*desktops)->GetCount(&count)); !r)
            return std::unexpected(r.error());

        // Snapshot each desktop once, then match by identity; a genuine enumeration
        // error exits early rather than masquerading as the absence that would
        // spuriously re-materialize.
        const auto snapshot = std::views::iota(0u, count) |
                              std::views::transform([&](UINT i) {
                                  return ok([&](vd::IVirtualDesktop** pp) {
                                      return (*desktops)->GetAt(
                                          i, vd::k_iidVirtualDesktop24H2,
                                          reinterpret_cast<void**>(pp));
                                  });
                              }) |
                              std::ranges::to<std::vector>();
        for (const auto& desktop : snapshot) {
            if (!desktop) return std::unexpected(desktop.error());
            GUID id{};
            if (const auto r = ok((*desktop)->GetID(&id)); !r)
                return std::unexpected(r.error());
            if (IsEqualGUID(id, target)) return *desktop;
        }
        // The target GUID no longer names a live desktop — routine, recoverable.
        return std::unexpected(Error{NotFound{}, std::source_location::current()});
    }

    // Create exactly ONE desktop (appended at the tail) and bind logical→GUID,
    // WITHOUT switching. No intermediate filling, no clamp (ADR-0003). The shared
    // root of materialize (which then switches) and the move path (which needs the
    // target to exist but must not steal focus unless following) — ADR-0010.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> createAndBind(int logical) {
        auto created = ok(
            [&](vd::IVirtualDesktop** pp) { return m_manager->CreateDesktop(pp); });
        if (!created) return std::unexpected(created.error());
        GUID id{};
        if (const auto r = ok((*created)->GetID(&id)); !r)
            return std::unexpected(r.error());
        m_logicalToGuid[logical] = id;
        return *created;
    }

    // Create-and-bind, then switch — the switchTo recovery path. Recovers a NotFound
    // from switchTo by materializing the workspace and landing on it.
    std::expected<Success, Error> materialize(int logical) {
        return createAndBind(logical).and_then(
            [&](const Microsoft::WRL::ComPtr<vd::IVirtualDesktop>& desktop) {
                return doSwitch(logical, desktop.Get());
            });
    }

    // Resolve the live target desktop for a move: a live binding hands back its
    // desktop; a missing or vanished one is re-materialized (create + bind, no
    // switch). A genuine OS/enumeration error propagates unchanged.
    std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>, Error> resolveMoveTarget(
        int logical) {
        return resolveBinding(logical).or_else(
            [&](const Error& e) -> std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktop>,
                                                 Error> {
                if (!std::holds_alternative<NotFound>(e.code))
                    return std::unexpected(e);
                return createAndBind(logical);
            });
    }

    // Call SwitchDesktop and, on success, record the new current workspace. The
    // .transform maps the Success through untouched while letting any Error skip
    // straight past it to the caller's boundary consumer.
    std::expected<Success, Error> doSwitch(int logical, vd::IVirtualDesktop* desktop) {
        return ok(m_manager->SwitchDesktop(desktop)).transform([&](Success s) {
            m_current = logical;
            return s;
        });
    }

    Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal> m_manager;
    Microsoft::WRL::ComPtr<vd::IApplicationViewCollection> m_viewCollection;  // HWND→view (move)
    std::unordered_map<int, GUID> m_logicalToGuid;  // logical workspace → desktop identity
    int m_current = 1;
};

// ── acquisition + IID-probe variant selection (the factory) ──────────────────

// Build the bridge, or return null (with a loud diagnostic) if no known variant
// resolves. Must run on the STA thread owning the COM apartment (the Worker).
// Sequence per ADR-0002: CoCreateInstance(ImmersiveShell, IServiceProvider) →
// QueryService(VirtualDesktopManagerInternal, <probed IID>) newest→oldest → first
// S_OK wins (QI-success ⟺ correct vtable).
inline std::unique_ptr<IVirtualDesktopBridge> makeVirtualDesktopBridge() {
    using bridge_detail::VdVariant;
    const DWORD build = bridge_detail::readBuildNumber();
    const DWORD ubr = bridge_detail::readUbr();
    const std::string buildStr = std::format("build {}.{}", build, ubr);

    auto shell = ok([&](IServiceProvider** pp) {
        return CoCreateInstance(vd::k_clsidImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_IServiceProvider, reinterpret_cast<void**>(pp));
    });
    if (!shell) {
        lg::error("virtual desktop bridge: CoCreateInstance(ImmersiveShell) failed — "
                  "COM VD switching disabled: {}",
                  shell.error());
        return nullptr;
    }

    // The probe table, newest→oldest: each row is an IID to QI and the variant it
    // denotes. QI succeeds only when the vtable matches, so the first S_OK is
    // self-validating (ADR-0002).
    struct Probe {
        const GUID& iid;
        VdVariant variant;
    };
    static constexpr std::array probes = {
        Probe{vd::k_iidVDMInternal_53F5CA0B, VdVariant::W24H2},  // resolved by build below
        Probe{vd::k_iidVDMInternal_A3175F2D, VdVariant::W22H2},
        Probe{vd::k_iidVDMInternal_B2F925B9, VdVariant::W21H2},
    };

    const std::wstring forced = bridge_detail::forcedVariantOverride();

    // Resolve a probe to its QI result plus the variant it denotes. The shared
    // 53F5CA0B IID is 24H2 only on build ≥ 26100; on 22631 it is 23H2-KB with a
    // different vtable and must not drive 24H2. WINSPACE_FORCE_VD_VARIANT overrides
    // the variant to test a stub.
    struct Resolved {
        const GUID* iid;
        std::expected<Microsoft::WRL::ComPtr<vd::IVirtualDesktopManagerInternal>, Error>
            manager;
        VdVariant variant;
    };
    const auto resolve = [&](const Probe& probe) -> Resolved {
        auto manager = ok([&](vd::IVirtualDesktopManagerInternal** pp) {
            return (*shell)->QueryService(vd::k_clsidVirtualDesktopManagerInternal,
                                          probe.iid, reinterpret_cast<void**>(pp));
        });
        VdVariant variant = probe.variant;
        if (IsEqualGUID(probe.iid, vd::k_iidVDMInternal_53F5CA0B))
            variant = (build >= 26100) ? VdVariant::W24H2 : VdVariant::W23H2KB;
        if (forced == L"21h2") variant = VdVariant::W21H2;
        else if (forced == L"22h2") variant = VdVariant::W22H2;
        else if (forced == L"23h2-kb") variant = VdVariant::W23H2KB;
        return {&probe.iid, std::move(manager), variant};
    };

    // Probe newest→oldest; the first IID whose vtable matches (QI succeeds) wins.
    auto resolved = probes | std::views::transform(resolve);
    const auto found = std::ranges::find_if(
        resolved, [](const Resolved& r) { return r.manager.has_value(); });

    // No known IID matched: a newer Windows bumped the interface. Fail LOUDLY
    // (null + diagnostic) — never call through a mismatched vtable (ADR-0002).
    if (found == std::ranges::end(resolved)) {
        lg::error(
            "virtual desktop bridge: NO known IVirtualDesktopManagerInternal IID matched ({}) "
            "— a new OS variant needs capturing; COM VD switching disabled",
            buildStr);
        return nullptr;
    }

    Resolved winner = *found;
    const std::string iidName = toUtf8(bridge_detail::guidToWString(*winner.iid));
    switch (winner.variant) {
        case VdVariant::W24H2:
            lg::info("virtual desktop bridge: matched IID {} ({}) — 24H2 variant", iidName,
                     buildStr);
            // The bridge needs the shell service provider too, to QueryService the
            // IApplicationViewCollection for the move path (ADR-0010, revised).
            return std::make_unique<VirtualDesktop24H2Bridge>(std::move(*winner.manager),
                                                              std::move(*shell));

        case VdVariant::W23H2KB:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 23H2-KB5034204 variant ({}) — "
                "NOT YET IMPLEMENTED (its vtable differs from 24H2); COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::W22H2:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 22H2 / pre-KB 23H2 variant ({}) — "
                "NOT YET IMPLEMENTED; COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::W21H2:
            lg::warn(
                "virtual desktop bridge: IID {} resolved to 21H2 variant ({}) — NOT YET "
                "IMPLEMENTED; COM VD switching disabled",
                iidName, buildStr);
            return nullptr;
        case VdVariant::None:
            break;  // unreachable — a matched probe always names a variant
    }
    return nullptr;  // unreachable — the None arm never triggers on a matched probe
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] control
// ─────────────────────────────────────────────────────────────────────────────

// Control channel + single-instance discovery — I/O adapter (owns <windows.h>).
//
// The out-of-band surface ADR-0019 adds beside the Reducer's Event stream. It is
// deliberately NOT part of window management: a Control message crosses a process
// boundary into the Orchestrator's message-only window carrying a SCALAR request
// (never the in-process Event* pointer), and mutates OS artifacts (the Logon
// task) or lifecycle (quit) outside `reduce`. The install / uninstall commands
// (the app section) are the only senders; the Worker's wndProc (below) is the
// only receiver.
//
// Discovery is by the Worker window's class name under HWND_MESSAGE — the same
// message-only window that receives in-process Event posts is what a second
// process finds to prove an Orchestrator is live. The named mutex is the
// race-free single-instance gate; the window is the addressable control target.

namespace winspace::io {

// The Worker's message-only window class. Shared so the Worker registers/creates
// under it AND another process can FindWindowEx it to locate the Orchestrator.
inline constexpr const wchar_t* k_workerClassName = L"winspace.worker";

// The single-instance mutex. `Local\` scopes it to the interactive session, so
// two logged-in accounts on one machine each get their own Orchestrator (matching
// the per-user Logon task) and never collide.
inline constexpr const wchar_t* k_orchestratorMutexName = L"Local\\winspace.orchestrator";

// The three cross-process Control requests, carried as the SCALAR wParam of the
// registered Control message. Values are explicit so the on-the-wire meaning is
// stable regardless of enum layout.
enum class Control : WPARAM {
    SyncAutostart = 1,    // make the Logon task match the Orchestrator's live start_at_login
    RemoveAutostart = 2,  // delete the Logon task (uninstall)
    Quit = 3,             // graceful shutdown via the clean Shift+Q path
};

// The registered window message that carries a Control request. RegisterWindowMessage
// returns the SAME id for the same string in every process, so sender and receiver
// agree without sharing a header constant; the id is well above WM_APP, so it never
// collides with k_wmEvent / k_wmSetHotkeyThread / k_wmReloadBinds. Cached in a
// function-local static — registered once per process, reused thereafter.
inline UINT controlMessage() {
    static const UINT id = RegisterWindowMessageW(L"winspace.control");
    return id;
}

// Discover a live Orchestrator: the Worker's message-only window, found by class
// under HWND_MESSAGE. WINEVENT_SKIPOWNPROCESS is irrelevant here — a bare
// `winspace` command runs in its OWN process, so the window it finds (if any) is
// genuinely the separate Orchestrator's. Null when none is running.
inline HWND findOrchestrator() {
    return FindWindowExW(HWND_MESSAGE, nullptr, k_workerClassName, nullptr);
}

// How long a command waits on a graceful quit before force-killing a wedged
// Orchestrator (ADR-0019). Long enough for the RAII teardown of the Hotkey/Hook
// threads to run, short enough that a stuck instance never blocks scoop update.
inline constexpr DWORD k_quitTimeoutMs = 5000;
// The per-message SendMessageTimeout budget for the autostart mutations, so a hung
// Orchestrator's UI thread cannot wedge the command indefinitely.
inline constexpr DWORD k_controlSendTimeoutMs = 5000;

// Send a Control request to a live Orchestrator and wait for it to be handled.
// SendMessageTimeout blocks until the Worker's wndProc returns — i.e. the
// autostart mutation completed — but abandons the wait if the target hangs
// (SMTO_ABORTIFHUNG) so a wedged Orchestrator degrades rather than blocks. Returns
// true iff the message was delivered and processed within the budget.
inline bool sendControl(HWND orchestrator, Control request) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(orchestrator, controlMessage(), static_cast<WPARAM>(request), 0,
                               SMTO_ABORTIFHUNG, k_controlSendTimeoutMs, &result) != 0;
}

// Stop a running Orchestrator so its exe unlocks (ADR-0019). Ask for a graceful
// quit down the clean Shift+Q path, then wait on the process handle; if it does
// not exit within the timeout it is wedged — force-kill as the last-resort
// fallback (the one place this codebase resorts to OS-level force). Resolving the
// PID BEFORE requesting quit avoids a race where the window is gone before we look.
inline void stopOrchestrator(HWND orchestrator) {
    DWORD pid = 0;
    GetWindowThreadProcessId(orchestrator, &pid);
    const HANDLE proc = pid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;

    // Post (don't Send) quit: the window is about to be destroyed, so we do not
    // want to block inside its wndProc — we wait on the process handle instead.
    PostMessageW(orchestrator, controlMessage(), static_cast<WPARAM>(Control::Quit), 0);

    if (!proc) return;  // no handle to wait on — best effort, the quit was posted
    if (WaitForSingleObject(proc, k_quitTimeoutMs) != WAIT_OBJECT_0) {
        lg::warn("uninstall: orchestrator did not exit within {}ms; force-killing", k_quitTimeoutMs);
        TerminateProcess(proc, 1);
    }
    CloseHandle(proc);
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] worker
// ─────────────────────────────────────────────────────────────────────────────

// Worker thread — I/O adapter (owns <windows.h>, COM, and all State).
//
// The single STA thread at winspace's center. It:
//   * CoInitializeEx(COINIT_APARTMENTTHREADED) — sole apartment owner of the
//     COM Virtual Desktop bridge;
//   * creates a message-only (HWND_MESSAGE) window in its constructor, so the
//     HWND is valid before anything can post to it;
//   * runs a GetMessage loop whose WndProc feeds each incoming Event through the
//     pure `reduce` and executes the emitted Effects on this thread;
//   * owns the authoritative State and the Reducer.
//
// Transport contract: producers (the Hotkey thread) hand an Event to
// the Worker by PostMessage-ing a heap-allocated `Event*` in LPARAM to this
// window (never PostThreadMessage — a thread-queue post has no HWND to own the
// message and races the loop's realization). The Worker takes ownership and
// deletes the Event after reducing.




namespace winspace::io {

// The custom window message that carries an Event* to the Worker. WM_APP is the
// first id reserved for private application use, so it never collides with
// system or common-control messages.
inline constexpr UINT k_wmEvent = WM_APP + 0;

// Tells the Worker the Hotkey thread's id (WPARAM), so a live reload can hand that
// thread the new Binds. Posted to the Worker's HWND once by the spine after the
// Hotkey thread publishes its id (the Worker is constructed first, so it cannot
// know the id at construction). ADR-0012.
inline constexpr UINT k_wmSetHotkeyThread = WM_APP + 1;

// Carries a freshly-parsed std::vector<Bind>* to the HOTKEY thread's queue on a
// reload (PostThreadMessage, so it has no HWND and is handled inline in that
// thread's loop, like WM_HOTKEY). The Hotkey thread takes ownership and rebuilds
// its HotkeyTable; on a failed post the sender deletes the vector (mirroring
// postEvent's delete-on-failure ownership). ADR-0012.
inline constexpr UINT k_wmReloadBinds = WM_APP + 2;

// Hand an Event to the Worker across threads. Ownership transfers to the Worker
// on a successful post (it deletes after reducing); on failure — a full queue
// or a dead HWND — we delete here so a dropped Event never leaks.
inline void postEvent(HWND workerHwnd, Event* ev) {
    if (!PostMessageW(workerHwnd, k_wmEvent, 0, reinterpret_cast<LPARAM>(ev))) {
        delete ev;
    }
}

// The Worker thread's window and behavioral core. Constructed on the Worker
// thread so its COM apartment and its message-only window share that thread's
// affinity; destroyed on the same thread so teardown (DestroyWindow /
// CoUninitialize) is likewise thread-correct.
class Worker {
public:
    // Takes the parsed window rules and seeds m_state.rules BEFORE anything can
    // reach this Worker: the ctor runs before workerThreadMain publishes the HWND,
    // so rules are in place before any Appeared (synthetic-adoption or live) can
    // arrive (startup ordering). The rule list is wrapped in a shared handle so the
    // per-event State copy stays O(1) (ADR-0009).
    explicit Worker(std::vector<WindowRule> rules, std::vector<ExecEntry> execs,
                    bool startAtLogin) {
        m_state.rules = std::make_shared<const std::vector<WindowRule>>(std::move(rules));
        // Launch entries are seeded alongside rules and behind the same O(1)-copy
        // handle, so they precede the Started{} the spine posts right after the
        // HWND publishes.
        m_state.execs = std::make_shared<const std::vector<ExecEntry>>(std::move(execs));
        // The start_at_login flag is seeded here beside rules/execs. It emits no
        // Effect and is reseeded on reload.
        m_state.startAtLogin = startAtLogin;

        // This thread is the single STA that will own the COM bridge.
        // COINIT_APARTMENTTHREADED demands a message loop, which run() supplies.
        // A failed init is tolerated (the bridge factory degrades to a null bridge);
        // the derived bool is what gates the matching CoUninitialize in the dtor.
        m_comInitialized = ok(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)).has_value();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Worker::wndProc;
        wc.hInstance = instance;
        wc.lpszClassName = k_className;
        RegisterClassExW(&wc);  // harmless if already registered; CreateWindow still succeeds

        // HWND_MESSAGE → a message-only window: no pixels, no taskbar button, no
        // Alt-Tab entry. It exists solely to receive posted Events. Created in
        // the ctor so hwnd() is valid before run() (and before any producer posts).
        m_hwnd = CreateWindowExW(0, k_className, nullptr, 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, nullptr, instance, this);
        // A null HWND is the fatal signal the spine acts on (via workerThreadMain);
        // checkWin32 here just names WHY it failed. Snapshot GetLastError immediately,
        // before any later call can clobber it. RegisterClassExW above stays unchecked:
        // its only failure is the benign already-registered case, which CreateWindowExW
        // shrugs off — a genuine problem resurfaces here as the null HWND.
        if (const auto created = ok(static_cast<BOOL>(m_hwnd != nullptr)); !created) {
            lg::error("worker: message-only window creation failed: {}", created.error());
        }

        // Build the COM Virtual Desktop bridge on this STA thread — its sole owner.
        // Null on an unsupported OS variant (a loud diagnostic is already logged);
        // switch Effects then no-op rather than calling through a wrong vtable.
        // Adoption ran in the factory, so seed State from the active desktop.
        m_bridge = makeVirtualDesktopBridge();
        if (m_bridge) m_state.currentWorkspace = m_bridge->currentWorkspace();
    }

    ~Worker() {
        // Release the COM bridge BEFORE CoUninitialize: a member is destroyed
        // only after this body runs, so an implicit release would fire on a
        // torn-down apartment. Reset here to keep COM teardown thread-correct.
        m_bridge.reset();
        if (m_hwnd) DestroyWindow(m_hwnd);
        if (m_comInitialized) CoUninitialize();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Valid immediately after construction (null only if window creation failed).
    HWND hwnd() const { return m_hwnd; }

    // The message pump. Returns when an Exit Effect has posted WM_QUIT. Posted
    // Events are routed to wndProc by DispatchMessage. GetMessage returns 0 on
    // WM_QUIT and -1 on error, so `> 0` exits cleanly on both.
    void run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

private:
    // The message-only window class, shared with the control section so another
    // process can FindWindowEx this exact window to reach the Orchestrator.
    static constexpr const wchar_t* k_className = k_workerClassName;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            // Stash the Worker* handed in via CreateWindowEx's lpParam so later
            // messages can reach the instance.
            const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        auto* self = reinterpret_cast<Worker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && msg == k_wmEvent) {
            return self->onEvent(reinterpret_cast<Event*>(lparam));
        }
        if (self && msg == k_wmSetHotkeyThread) {
            self->m_hotkeyThreadId = static_cast<DWORD>(wparam);
            return 0;
        }
        // Cross-process Control message (ADR-0019). The registered id is resolved
        // at runtime, so it cannot sit in a `case`; compared last, after the
        // fixed WM_APP ids. Handled OUTSIDE the Reducer — it mutates OS artifacts
        // or lifecycle, not window-management State.
        if (self && msg == controlMessage()) {
            self->onControl(static_cast<Control>(wparam));
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // Execute a cross-process Control request on the Worker's STA thread (the COM
    // apartment the autostart executor needs). This is the receiving half of the
    // control channel — the install / uninstall commands are the senders.
    void onControl(Control request) {
        switch (request) {
            case Control::SyncAutostart:
                // Make the Logon task match the Orchestrator's LIVE flag — the
                // authoritative single source of truth (possibly just reloaded),
                // so a running WM never needs its config re-read from disk. Same
                // declarative executor as the SyncAutostart Effect; degrade-and-log.
                if (const auto synced = syncAutostart(m_state.startAtLogin); !synced)
                    lg::warn("control: sync-autostart (enabled={}) failed: {}",
                             m_state.startAtLogin, synced.error());
                break;
            case Control::RemoveAutostart:
                // Unconditionally delete the Logon task (uninstall). ERROR_FILE_NOT_FOUND
                // is counted as success inside, so an absent task is a clean no-op.
                if (const auto removed = syncAutostart(false); !removed)
                    lg::warn("control: remove-autostart failed: {}", removed.error());
                break;
            case Control::Quit:
                // Route into the EXISTING clean-shutdown path (the one a Shift+Q
                // `quit` bind uses): post the Quit Event to ourselves so it flows
                // through reduce -> Exit -> PostQuitMessage, ending run() and
                // letting the spine's RAII tear the Hotkey/Hook threads down. Not
                // TerminateProcess — that is only the command's timeout fallback.
                postEvent(m_hwnd, new Event{Quit{}});
                break;
        }
    }

    // Take ownership of the posted Event, reduce it against current State, adopt
    // the new State, and execute each emitted Effect — all on the Worker thread.
    LRESULT onEvent(Event* raw) {
        const std::unique_ptr<Event> ev(raw);  // delete after reducing
        ReduceResult result = reduce(m_state, *ev);
        m_state = std::move(result.state);
        for (const Effect& effect : result.effects) execute(effect);
        return 0;
    }

    void execute(const Effect& effect) {
        std::visit(
            detail::overload{
                [&](const SwitchToWorkspace& s) {
                    // The bridge (sole owner: this thread) resolves the Logical
                    // number to a Virtual Desktop GUID and calls SwitchDesktop,
                    // materializing the workspace on demand. Null bridge (an
                    // unsupported OS variant) → the switch is a no-op.
                    if (m_bridge) m_bridge->switchTo(s.logical);
                },
                [&](const Exit&) {
                    // End run()'s loop; the process then unwinds cleanly.
                    PostQuitMessage(0);
                },
                [&](const ResolveFocus& rf) {
                    // Phase one of the focus round-trip (ADR-0008): run the Probe
                    // sweep and post the rects back to ourselves as a FocusResolve
                    // Event, which the Reducer then resolves. The Worker stays a
                    // mechanical executor — it never decides which window to focus.
                    postEvent(m_hwnd, new Event{FocusResolve{rf.dir, probeTopLevelWindows(),
                                                             probeForeground()}});
                },
                [&](const SetForegroundWindow& sf) {
                    // Bare call, degrade-and-log: a just-fired hotkey
                    // usually satisfies Win32's foreground-lock, so this succeeds;
                    // on failure we log and move on — never crash (null-bridge
                    // precedent). The scoped AttachThreadInput recovery is deferred
                    // until the Smoke seam proves the bare call insufficient.
                    if (const auto set = ok(::SetForegroundWindow(toHwnd(sf.id))); !set)
                        lg::warn("focus: SetForegroundWindow failed: {}", set.error());
                },
                [&](const MoveForegroundWindowToWorkspace& m) {
                    // Ungated by Eligibility: the user aimed at the
                    // foreground window explicitly. Resolve it inline — no probe
                    // round-trip, since there is no pure decision to make. No bridge
                    // or no foreground window → degrade-and-log, never crash. The
                    // internal MoveViewToDesktop reassigns the window's desktop
                    // without ever painting it on the current one, so no DWM cloak
                    // is needed (and DWMWA_CLOAK is same-process-only anyway — it
                    // returns E_ACCESSDENIED on a foreign window; ADR-0010 revised).
                    if (!m_bridge) return;
                    const HWND fg = GetForegroundWindow();
                    if (!fg) {
                        lg::warn("move to workspace {}: no foreground window", m.logical);
                        return;
                    }
                    m_bridge->moveWindowToWorkspace(toWindowId(fg), m.logical);
                },
                [&](const MoveWindowToWorkspace& m) {
                    // The WindowRule move: a SPECIFIC window that just
                    // Appeared, by id — no GetForegroundWindow, no cloak. The
                    // internal MoveViewToDesktop reassigns the desktop without ever
                    // painting on the current one (ADR-0010 revised), so a
                    // cross-Workspace pin never flashes here. Null bridge → no-op.
                    if (m_bridge) m_bridge->moveWindowToWorkspace(m.id, m.logical);
                },
                [&](const ResolveSpread& rs) {
                    // Phase one of the Spread round-trip (ADR-0015), mirroring
                    // ResolveFocus: probe Display occupancy and post SpreadResolve back
                    // to ourselves, which the Reducer then reduces into the target
                    // choice. The Worker decides nothing — it only enumerates.
                    //
                    // Start from every Display, all unoccupied; then mark a Display
                    // occupied when an Eligible window on the current Workspace maps to
                    // it. isEligible already excludes DWM-cloaked windows, so windows
                    // parked on other Virtual Desktops never count (Empty is
                    // same-Workspace by construction). The subject is excluded so its
                    // own opening Display never reads as occupied and defeats placement.
                    const auto monitors = enumerateDisplays();
                    std::vector<DisplayOccupancy> displays(monitors.size());
                    std::ranges::transform(monitors, displays.begin(), [](MonitorId id) {
                        return DisplayOccupancy{.id = id, .occupied = false};
                    });

                    const auto windows = probeTopLevelWindows();
                    auto occupiedMonitors =
                        windows | std::views::filter([&](const WindowAttrs& a) {
                            return isEligible(a) && a.id != rs.subject;
                        }) | std::views::transform([](const WindowAttrs& a) {
                            const RECT rc{a.rect.left, a.rect.top, a.rect.right, a.rect.bottom};
                            return toMonitorId(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST));
                        });
                    for (const MonitorId mon : occupiedMonitors)
                        if (const auto d = std::ranges::find_if(
                                displays, [&](const DisplayOccupancy& e) { return e.id == mon; });
                            d != displays.end())
                            d->occupied = true;

                    postEvent(m_hwnd, new Event{SpreadResolve{rs.subject, std::move(displays)}});
                },
                [&](const PositionWindow& pw) {
                    // winspace's ONLY geometry write (ADR-0015) — the single bounded
                    // reversal of ADR-0007's no-geometry ban, emitted only by Spread.
                    // One SetWindowPos onto the target Display's WORK area (rcWork, so
                    // the taskbar is respected), at the window's NATURAL size (its
                    // current width/height preserved — never resized or snapped). It
                    // happens once, at placement; winspace never touches the rect again.
                    // Degrade-and-log throughout (ADR-0004): a bad monitor, HWND, or a
                    // failed SetWindowPos logs and returns, never crashes the WM.
                    const HWND h = toHwnd(pw.id);
                    if (MONITORINFO mi{.cbSize = sizeof(MONITORINFO)};
                        !GetMonitorInfoW(toHmonitor(pw.target), &mi)) {
                        lg::warn("spread: GetMonitorInfo failed for target display");
                    } else if (RECT r{}; !GetWindowRect(h, &r)) {
                        lg::warn("spread: GetWindowRect failed; leaving window in place");
                    } else if (const auto moved = ok(SetWindowPos(
                                   h, nullptr, mi.rcWork.left, mi.rcWork.top, r.right - r.left,
                                   r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE));
                               !moved) {
                        lg::warn("spread: SetWindowPos failed: {}", moved.error());
                    }
                },
                [&](const LaunchApp& l) {
                    // Launch-only (ADR-0011): start a detached child and
                    // forget it — no PID kept, no Workspace assigned (placement is a
                    // paired `windowrule`). CreateProcessW itself parses exe + args
                    // from the command line and searches %PATH%; it WRITES to
                    // lpCommandLine, so the widened command goes into a mutable
                    // buffer. cwd/env are inherited (nullptr). On success both handles
                    // are closed immediately, fully detaching the child so it outlives
                    // winspace; on failure we degrade-and-log and continue — one bad
                    // entry never takes down the WM or blocks the others.
                    std::wstring cmdline = toWide(l.command);
                    STARTUPINFOW si{};
                    si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    if (const auto launched =
                            ok(CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                              0, nullptr, nullptr, &si, &pi))) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    } else {
                        lg::warn("exec: launch failed for '{}': {}", l.command,
                                 launched.error());
                    }
                },
                [&](const ReloadConfig&) {
                    // Live reload (ADR-0012), executed on THIS thread. Re-read
                    // + re-parse via the shared helper, then apply ATOMICALLY: any
                    // Diagnostic — or an unreadable file — keeps the currently-running
                    // config live and changes nothing (the fallback is the user's own
                    // working config, so reload can afford to reject the whole file).
                    const std::optional<std::filesystem::path> path = configPath();
                    ConfigReadResult load =
                        path ? readAndParseConfig(*path, SeedPolicy::NoSeed)
                             : ConfigReadResult{};
                    if (!load.read) {
                        lg::warn("reload: config unreadable; keeping the running config");
                        return;
                    }
                    if (!load.parsed.diagnostics.empty()) {
                        for (const Diagnostic& d : load.parsed.diagnostics)
                            lg::warn("reload: config line {}: {}", d.line, d.message);
                        lg::warn("reload: {} diagnostic(s); keeping the running config",
                                 load.parsed.diagnostics.size());
                        return;
                    }

                    // Clean parse — fan out to the three owners of config-derived state
                    // (ADR-0012). (1) Reseed the Worker's own State behind fresh O(1)-copy
                    // handles; the flag is a plain reseed.
                    Config& cfg = load.parsed.config;
                    m_state.rules =
                        std::make_shared<const std::vector<WindowRule>>(std::move(cfg.rules));
                    m_state.execs =
                        std::make_shared<const std::vector<ExecEntry>>(std::move(cfg.execs));
                    m_state.startAtLogin = cfg.startAtLogin;

                    // (2) Hand the new Binds to the Hotkey thread to rebuild its
                    // HotkeyTable on that thread (RegisterHotKey is thread-affine). Heap
                    // pointer with delete-on-failed-post ownership, mirroring postEvent.
                    if (m_hotkeyThreadId != 0) {
                        auto* binds = new std::vector<Bind>(std::move(cfg.binds));
                        if (!PostThreadMessageW(m_hotkeyThreadId, k_wmReloadBinds, 0,
                                                reinterpret_cast<LPARAM>(binds))) {
                            delete binds;
                            lg::warn("reload: could not hand new binds to the hotkey thread; "
                                     "hotkeys unchanged");
                        }
                    } else {
                        lg::warn("reload: hotkey thread id unknown; hotkeys unchanged");
                    }

                    // (3) Post Reloaded{} to ourselves so the existing exec-relaunch path
                    // runs unchanged — `exec` entries re-launch, `exec-once` do not — and
                    // the Reloaded handler additionally emits SyncAutostart, so the logon
                    // task is re-synced to the freshly-reseeded flag on this same reload.
                    postEvent(m_hwnd, new Event{Reloaded{}});
                    lg::info("reload: applied new config");
                },
                [&](const SyncAutostart& s) {
                    // Delegate the whole ITaskService interaction to the adapter
                    // (ADR-0013): create-or-update the `\winspace\<username>` logon task when
                    // enabled, delete-if-exists when not. Degrade-and-log on any failure
                    // (ADR-0004) — autostart never blocks or crashes the WM.
                    if (const auto synced = syncAutostart(s.enabled); !synced)
                        lg::warn("autostart: sync (enabled={}) failed: {}", s.enabled,
                                 synced.error());
                },
            },
            effect);
    }

    State m_state{};
    HWND m_hwnd = nullptr;
    bool m_comInitialized = false;
    DWORD m_hotkeyThreadId = 0;  // set by k_wmSetHotkeyThread; target of reload's new Binds
    std::unique_ptr<IVirtualDesktopBridge> m_bridge;  // COM VD bridge; null if unsupported
};

// Worker thread entry. Constructs the Worker (COM + message-only HWND), publishes
// the HWND to the spine, then pumps until Exit. On return the Worker destructor
// tears COM and the window down on this same thread. A null published HWND
// signals a construction failure the spine treats as fatal.
inline void workerThreadMain(std::promise<HWND> ready, std::vector<WindowRule> rules,
                             std::vector<ExecEntry> execs, bool startAtLogin) {
    Worker worker(std::move(rules), std::move(execs), startAtLogin);
    const HWND hwnd = worker.hwnd();
    ready.set_value(hwnd);
    if (hwnd) worker.run();
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] window_hook
// ─────────────────────────────────────────────────────────────────────────────

// Window-hook adapter — I/O adapter (owns <windows.h>). The SetWinEventHook
// lifecycle stream behind window rules, on its own dedicated thread, mirroring
// the Hotkey thread: it owns no State and makes no decision — its callback runs
// the noise gate, Probes the window, and posts an Appeared / Vanished Event to
// the Worker, which reduces it.
//
// Kept off the Worker thread so hook delivery never queues behind a blocking
// Effect (a SwitchDesktop or a move round-trip). Two tight SetWinEventHook ranges
// share one WINEVENTPROC, both WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS.
// The callback reaches the Worker HWND through a thread_local (a WINEVENTPROC
// takes no user context), so it must be set on this thread before the hooks fire.




namespace winspace::io {

// The Worker HWND the callback posts to. A WINEVENTPROC carries no user pointer,
// so the target is stashed thread-locally on the hook thread before registration.
inline thread_local HWND t_hookWorkerHwnd = nullptr;

// One callback for both ranges. The noise gate keeps only genuine top-level
// window events (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd):
// SHOW / UNCLOAKED → Appeared (probe BOTH attrs and identity); DESTROY / HIDE /
// CLOAKED → Vanished (id only — never probe a possibly-dead HWND).
inline void CALLBACK winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                                  LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    const HWND worker = t_hookWorkerHwnd;
    if (!worker) return;

    switch (event) {
        case EVENT_OBJECT_SHOW:
        case EVENT_OBJECT_UNCLOAKED:
            postEvent(worker, new Event{Appeared{probeWindow(hwnd), probeIdentity(hwnd)}});
            break;
        case EVENT_OBJECT_DESTROY:
        case EVENT_OBJECT_HIDE:
        case EVENT_OBJECT_CLOAKED:
            postEvent(worker, new Event{Vanished{toWindowId(hwnd)}});
            break;
        default:
            break;  // an event inside a range we don't act on — ignore
    }
}

// Hook thread entry. Mirrors hotkeyThreadMain: force the message queue into
// existence, publish the thread id (so the spine can WM_QUIT it), then register
// the hooks and run. Register-then-enumerate closes the create-in-the-gap race —
// the hooks are live before EnumWindows posts a synthetic Appeared per top-level
// window (startup adoption); double-sightings are deduped for free by `placed`.
// On WM_QUIT the loop returns and both hooks are unhooked on THIS thread (the
// only thread allowed to unhook them), so quit leaves no dangling hook.
inline void hookThreadMain(HWND workerHwnd, std::promise<DWORD> tidReady) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    t_hookWorkerHwnd = workerHwnd;

    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const HWINEVENTHOOK showHide = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, winEventProc, 0, 0, flags);
    const HWINEVENTHOOK cloak = SetWinEventHook(
        EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, winEventProc, 0, 0, flags);
    if (!showHide || !cloak)
        lg::error("window hook: SetWinEventHook failed — window rules disabled");

    // Startup adoption: post a synthetic Appeared for every top-level window,
    // through the same path a live SHOW takes (EnumWindows never sees winspace's
    // own message-only HWND).
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            postEvent(reinterpret_cast<HWND>(lp),
                      new Event{Appeared{probeWindow(h), probeIdentity(h)}});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(workerHwnd));

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (showHide) UnhookWinEvent(showHide);
    if (cloak) UnhookWinEvent(cloak);
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [io section] app
// ─────────────────────────────────────────────────────────────────────────────

// Process spine — I/O adapter (owns <windows.h> and thread lifecycle).
//
// The backbone the whole product plugs into: it spawns the Worker, Hotkey, and Hook
// threads, wires the producer→Worker transport, and tears all three down on a clean
// exit. No domain logic lives here — behavior is the Reducer's, executed by the
// Worker. This file carries only the thread wiring.
//
//   Hotkey / Hook thread ─PostMessage(Event*)─▶ Worker's message-only HWND
//                                               Worker: reduce → execute Effects
//                                               Exit Effect → PostQuitMessage → unwind




namespace winspace::io {

// Load the config for STARTUP. Shares the read+parse mechanism with the Worker's
// reload path (io/config_io.cpp: readAndParseConfig) but passes SeedPolicy::Seed so
// a first run creates the file, and layers startup's own fallback on top: if
// %USERPROFILE% is unset or the file is unreadable, fall back to the built-in
// default text. Startup degrades PER-LINE (each diagnostic is logged and the good
// lines still apply) because its only fallback is the destructive built-in default
// — the deliberate asymmetry with reload's atomic keep-last-good (ADR-0012).
inline LoadedConfig loadConfig() {
    const std::optional<std::filesystem::path> path = configPath();
    if (!path)
        lg::warn("config: %USERPROFILE% is unset; using built-in defaults");
    ConfigReadResult load =
        path ? readAndParseConfig(*path, SeedPolicy::Seed) : ConfigReadResult{};
    ParseResult parsed =
        load.read ? std::move(load.parsed) : parse(std::string(k_defaultConfig));
    if (path && !load.read)
        lg::warn("config: file unreadable at startup; using built-in defaults");
    for (const Diagnostic& d : parsed.diagnostics)
        lg::warn("config line {}: {}", d.line, d.message);
    return toLoaded(std::move(parsed.config));
}

// A non-interactive exit path: setting WINSPACE_SELFTEST_QUIT drives one Quit
// through the real transport so clean shutdown is provable without a live desktop.
// Unset (the default), winspace runs windowless until a bound `quit` hotkey.
inline bool selftestQuitEnabled() {
    return GetEnvironmentVariableW(L"WINSPACE_SELFTEST_QUIT", nullptr, 0) != 0;
}

// Hotkey thread entry. Registers the Binds on THIS thread (so WM_HOTKEY lands in
// its queue), publishes its thread id so the spine can unwind it with a WM_QUIT,
// then pumps: each WM_HOTKEY becomes an Event posted to the Worker.
inline void hotkeyThreadMain(HWND workerHwnd, const std::vector<Bind>& binds,
                             std::promise<DWORD> tidReady) {
    MSG msg;
    // Force this thread's message queue into existence before publishing the id,
    // so the spine's later PostThreadMessage can't race queue creation.
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    tidReady.set_value(GetCurrentThreadId());

    // The live table, held in an optional so a reload can tear it down (destroying
    // it unregisters every hotkey on THIS thread) BEFORE building the new one —
    // registering the new set first would collide with a still-registered old combo
    // (ERROR_HOTKEY_ALREADY_REGISTERED) and silently drop the re-bound hotkey.
    std::optional<HotkeyTable> hotkeys;
    hotkeys.emplace(binds);

    if (selftestQuitEnabled()) {
        postEvent(workerHwnd, new Event{Quit{}});
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // A hotkey has a null window, so it never dispatches to a WndProc — handle
        // it here. wParam is the hotkey id, i.e. the Bind's index in the table.
        if (msg.message == WM_HOTKEY) {
            if (!hotkeys)
                continue;
            if (auto ev = hotkeys->eventFor(static_cast<int>(msg.wParam)))
                postEvent(workerHwnd, new Event{std::move(*ev)});
            continue;
        }
        // A live reload: the Worker handed us a heap-allocated Bind
        // vector to re-register. Take ownership, drop the old table (unregister-all),
        // then build the new one (register-all) — in that order (see above).
        if (msg.message == k_wmReloadBinds) {
            const std::unique_ptr<std::vector<Bind>> newBinds(
                reinterpret_cast<std::vector<Bind>*>(msg.lParam));
            hotkeys.reset();
            hotkeys.emplace(*newBinds);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Narrow wWinMain's wide command tail (the arguments AFTER the program name —
// wWinMain's lpCmdLine excludes it) to UTF-8 tokens and hand them to the pure
// parseCommand. Whitespace-split is sufficient: the two verbs are single bare
// words with no paths or quoting, and any quoted/multi-token line is Other anyway
// (handled by parseCommand's size check). Keeping the argv extraction here leaves
// the core wchar_t-free and shell32-free — no CommandLineToArgvW needed.
inline Command commandFromCmdLine(const wchar_t* cmdLine) {
    const auto isSep = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    std::vector<std::string> args;
    const std::wstring line = cmdLine ? cmdLine : L"";
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isSep(line[i])) ++i;      // skip separators
        const size_t start = i;
        while (i < line.size() && !isSep(line[i])) ++i;     // take one token
        if (i > start) args.push_back(toUtf8(std::wstring_view(line).substr(start, i - start)));
    }
    return parseCommand(args);
}

// The headless `winspace install` command (ADR-0019). Make OS autostart match the
// config's start_at_login — NEVER enabling it unbidden: the flag stays the single
// source of truth. Hybrid: a live Orchestrator holds the authoritative (possibly
// reloaded) flag, so ask IT to sync via a Control message; with none running,
// load/self-seed the config exactly as the WM does and perform the one-shot sync
// directly. The Scoop post_install hook invokes this to re-stamp the Logon task to
// the new versioned location on every update (the moved-binary self-heal) without
// launching the WM. Returns 0 on success, non-zero so Scoop can read a failure.
inline int runInstall() {
    if (const HWND orchestrator = findOrchestrator()) {
        return sendControl(orchestrator, Control::SyncAutostart) ? 0 : 1;
    }
    // No Orchestrator: read start_at_login from config (self-seeding a first-run
    // default) and sync the task directly. A false flag deletes/leaves-absent the
    // task, so a fresh install never starts seizing logon hooks.
    const LoadedConfig cfg = loadConfig();
    if (const auto synced = syncAutostart(cfg.startAtLogin); !synced) {
        lg::warn("install: direct autostart sync (enabled={}) failed: {}", cfg.startAtLogin,
                 synced.error());
        return 1;
    }
    return 0;
}

// The headless `winspace uninstall` command (ADR-0019). Remove the
// `\winspace\<user>` Logon task UNCONDITIONALLY and stop any running Orchestrator
// so the exe unlocks for deletion. Hybrid: a live Orchestrator removes the task
// (remove-autostart) and then is asked to quit gracefully (with a force-kill
// timeout fallback); with none running, remove the task directly. The Scoop
// pre_uninstall hook invokes this so uninstall never orphans the task nor fails on
// the running-process file lock. An absent task counts as success (clean no-op).
inline int runUninstall() {
    if (const HWND orchestrator = findOrchestrator()) {
        // Remove the task FIRST (while the STA COM thread is still alive to run it),
        // then stop the process so its image unlocks.
        const bool removed = sendControl(orchestrator, Control::RemoveAutostart);
        stopOrchestrator(orchestrator);
        return removed ? 0 : 1;
    }
    if (const auto removed = syncAutostart(false); !removed) {
        lg::warn("uninstall: direct autostart removal failed: {}", removed.error());
        return 1;
    }
    return 0;
}

// Bring up all three threads, run until Exit, then tear down cleanly. Returns the
// process exit code. Called by wWinMain.
inline int runApp() {
    // Single-instance guard (ADR-0019). The WM owns EXCLUSIVE OS resources — the
    // global hotkeys and the COM Virtual Desktop bridge — so a second `winspace`
    // must not raise a competing Orchestrator. The named mutex is the race-free
    // gate: if it already exists another instance owns the session, so exit 0
    // (starting winspace twice is a successful no-op, not an error — user story 4).
    // The Orchestrator's message-only window is the addressable control target the
    // install/uninstall commands find; the mutex closes the two-launches-at-once
    // race the window discovery alone cannot.
    const HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, k_orchestratorMutexName);
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        lg::info("app: an orchestrator is already running; exiting (single-instance)");
        CloseHandle(instanceMutex);  // release our reference; the live owner keeps the mutex
        return 0;
    }
    // Owner (or CreateMutex failed — degrade to unguarded rather than refuse to
    // start). Held for the process lifetime; the OS destroys the mutex when this
    // last handle closes at exit, so a later relaunch is unguarded again as intended.
    const auto releaseMutex = [&] {
        if (instanceMutex) CloseHandle(instanceMutex);
    };

    // Per-Monitor-V2 DPI awareness, set before any window exists (the Worker's
    // message-only HWND included), so it leads runApp. This puts the process and
    // every window rect in ONE physical-pixel coordinate space — the precondition
    // for correct spatial-focus resolution across scaled displays. Best
    // effort: a failure (an OS predating V2, or awareness already pinned by a
    // manifest) leaves the prior awareness in place and is logged, never fatal.
    if (const auto aware =
            ok(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        !aware) {
        lg::warn("app: Per-Monitor-V2 DPI awareness not set: {}", aware.error());
    }

    // One parse yields all three halves. The Binds are owned here and passed to the
    // Hotkey thread by const reference (this declaration outlives hotkeyThread, which
    // is torn down at scope exit); the WindowRules and the launch entries are moved
    // into the Worker thread, which seeds both into State before publishing its HWND.
    LoadedConfig cfg = loadConfig();
    const std::vector<Bind> binds = std::move(cfg.binds);
    const bool startAtLogin = cfg.startAtLogin;

    // Start the Worker and wait for its message-only HWND before anyone posts. The
    // rules, execs, and start_at_login flag are seeded in the Worker ctor, so they
    // precede any Appeared and the Started{} below.
    std::promise<HWND> hwndReady;
    std::future<HWND> hwndFuture = hwndReady.get_future();
    std::jthread workerThread(workerThreadMain, std::move(hwndReady), std::move(cfg.rules),
                              std::move(cfg.execs), startAtLogin);

    const HWND workerHwnd = hwndFuture.get();
    if (!workerHwnd) {
        workerThread.join();
        releaseMutex();
        return 1;  // Worker could not create its window — nothing can proceed
    }

    // The Worker HWND exists and its execs are seeded: post Started{} so the launch
    // entries fire. Done before the hotkey/hook threads wire up — ordering
    // vs. the hook thread is not correctness-critical, since the
    // register-then-EnumWindows adoption places a launched window whether it appears
    // before or after the hooks register.
    postEvent(workerHwnd, new Event{Started{}});

    // Start the Hotkey thread once the Worker HWND exists to post to. It registers
    // the Binds as OS hotkeys on its own thread.
    std::promise<DWORD> tidReady;
    std::future<DWORD> tidFuture = tidReady.get_future();
    auto* rawHotkeyThread = new std::jthread(hotkeyThreadMain, workerHwnd, std::cref(binds),
                                             std::move(tidReady));
    const DWORD hotkeyTid = tidFuture.get();
    auto quitHotkey = [hotkeyTid](std::jthread* t) {
        PostThreadMessageW(hotkeyTid, WM_QUIT, 0, 0);
        delete t;
    };
    std::unique_ptr<std::jthread, decltype(quitHotkey)> hotkeyThread(rawHotkeyThread,
                                                                     std::move(quitHotkey));

    // Tell the Worker the Hotkey thread's id so a live `reload` can hand that thread
    // the freshly-parsed Binds to re-register (ADR-0012). Posted after the
    // id is known — the Worker was constructed before this thread existed.
    PostMessageW(workerHwnd, k_wmSetHotkeyThread, static_cast<WPARAM>(hotkeyTid), 0);

    // Start the Hook thread (third jthread, mirroring the Hotkey thread). It owns
    // the SetWinEventHook adapter, registers its hooks then runs startup adoption,
    // and posts Appeared / Vanished Events to the Worker.
    std::promise<DWORD> hookTidReady;
    std::future<DWORD> hookTidFuture = hookTidReady.get_future();
    auto* rawHookThread = new std::jthread(hookThreadMain, workerHwnd, std::move(hookTidReady));
    const DWORD hookTid = hookTidFuture.get();
    auto quitHook = [hookTid](std::jthread* t) {
        PostThreadMessageW(hookTid, WM_QUIT, 0, 0);
        delete t;
    };
    std::unique_ptr<std::jthread, decltype(quitHook)> hookThread(rawHookThread,
                                                                 std::move(quitHook));

    // Block until an Exit Effect posts WM_QUIT to the Worker and its loop returns.
    workerThread.join();
    releaseMutex();
    return 0;
}

}  // namespace winspace::io

// ─────────────────────────────────────────────────────────────────────────────
// [entry point] windowless wWinMain
// ─────────────────────────────────────────────────────────────────────────────
// Defining wWinMain makes the linker select the Unicode windows CRT startup
// (wWinMainCRTStartup) automatically; /SUBSYSTEM:WINDOWS gives us no console.
// The command line is parsed BEFORE any window/thread exists (ADR-0019): no args
// runs the WM (unchanged), `install`/`uninstall` run the headless commands, and
// an unknown verb or stray arguments exit 2 (a usage error) rather than silently
// starting the WM. All wiring lives in the I/O spine (the [io app section] above).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR lpCmdLine, int) {
    switch (winspace::io::commandFromCmdLine(lpCmdLine)) {
        case winspace::Command::Run:       return winspace::io::runApp();
        case winspace::Command::Install:   return winspace::io::runInstall();
        case winspace::Command::Uninstall: return winspace::io::runUninstall();
        case winspace::Command::Other:     return 2;
    }
    return 2;  // unreachable — the switch is exhaustive
}
