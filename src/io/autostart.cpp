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
#pragma once

#include <windows.h>

#include <lmcons.h>    // UNLEN — the max account-name length for GetUserNameW
#include <objbase.h>   // CoCreateInstance
#include <taskschd.h>  // ITaskService and the Task Scheduler 2.0 COM ABI
#include <wrl/client.h>  // Microsoft::WRL::ComPtr — RAII for the COM pointers

#include <expected>
#include <source_location>
#include <string>
#include <variant>

#include "io/error.cpp"  // io::Error vocabulary (ok() wrappers, formatter) + lg:: levels

namespace winspace::io {

namespace autostart_detail {

using Microsoft::WRL::ComPtr;

// The dedicated task folder and its logon-task settings (ADR-0013). The folder is
// per-machine (`\winspace`) but holds one task PER USER, named by the account, so
// two accounts on one machine never collide.
inline constexpr const wchar_t* k_folderPath = L"\\winspace";
inline constexpr const wchar_t* k_rootPath = L"\\";
inline constexpr const wchar_t* k_author = L"winspace";
inline constexpr const wchar_t* k_noTimeLimit = L"PT0S";   // ExecutionTimeLimit: unbounded
inline constexpr const wchar_t* k_restartEvery = L"PT1M";  // RestartInterval: one minute
inline constexpr LONG k_restartCount = 3;

// The Task Scheduler 2.0 signature returns an HRESULT of ERROR_FILE_NOT_FOUND when
// a task or folder is absent — the routine, expected shape for "already removed".
inline constexpr HRESULT k_fileNotFound = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

// RAII BSTR — the Task Scheduler ABI speaks BSTR for every string in/out. Frees on
// scope exit so a build with a dozen strings leaks none.
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t* s) : value(SysAllocString(s)) {}
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
    wchar_t buf[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (GetUserNameW(buf, &size)) return std::wstring(buf, size - 1);  // size counts the null
    const DWORD code = GetLastError();
    return std::unexpected(Error{Win32Error{code}, std::source_location::current()});
}

// domain\user for the logon trigger's UserId, so the task fires at THIS user's
// logon rather than any user's. Built from %USERDOMAIN% + the account name; falls
// back to the bare account name if the domain is unavailable.
inline std::wstring currentUserId(const std::wstring& user) {
    wchar_t domain[256];
    const DWORD n = GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    if (n == 0 || n >= 256) return user;
    return std::wstring(domain, n) + L'\\' + user;
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
    record(ok((*logon)->put_UserId(Bstr(userId.c_str()))));

    auto actions = ok([&](IActionCollection** pp) { return def->get_Actions(pp); });
    if (!actions) return std::unexpected(actions.error());
    auto action = ok([&](IAction** pp) { return (*actions)->Create(TASK_ACTION_EXEC, pp); });
    if (!action) return std::unexpected(action.error());
    auto exec = ok([&](IExecAction** pp) { return (*action)->QueryInterface(IID_PPV_ARGS(pp)); });
    if (!exec) return std::unexpected(exec.error());
    record(ok((*exec)->put_Path(Bstr(exePath.c_str()))));

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
    Bstr taskName(user->c_str());

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
