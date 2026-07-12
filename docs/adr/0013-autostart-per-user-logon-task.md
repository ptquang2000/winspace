# Autostart is a per-user logon task, not a service or a watchdog

**Status:** Accepted (2026-07-11 — expands `DESIGN.md §7`, which states only the conclusion).

winspace's `start_at_login` flag makes it start with the user session. `DESIGN.md §7` names
the mechanism — a Task Scheduler logon task, "not a Windows service" — in one line. This ADR
records *why* the logon task beats the alternatives that a reader would reasonably expect for
an always-on daemon that must restart on failure, so the one-liner does not read as arbitrary.

## Context

winspace is a **per-user, per-session, unprivileged, interactive** window manager. It manages
Virtual Desktops, foreground focus, and hotkeys — none of which exist until a user is logged on
and an interactive session exists. It owns no geometry and does privileged nothing. The
autostart requirement (issue 10) adds two constraints beyond "start at logon":

1. **Restart on failure** — an unattended session daemon that dies mid-session and stays dead
   until the next logon is a bad failure mode.
2. **Driven by a config flag, live.** `start_at_login` is a per-user config setting the running
   (unelevated) winspace reads; toggling it and reloading must register/remove autostart without
   a restart and without a privilege prompt. This is the `SyncAutostart` Effect fired from
   `Started{}` and `Reloaded{}` — see [ADR-0012](0012-config-reload-worker-parsed-atomic.md) for
   the reload path it rides.

The obvious way to get restart-on-failure on Windows is a **service** (SCM recovery actions), or
a **userspace watchdog** that respawns a crashed worker. Both are more machinery than a logon
task, so the choice needs justifying against them, not just against "no autostart."

## Decision

Autostart is a single **Task Scheduler logon task**, registered **per-user** at
`\winspace\<username>`, launching the windowless `winspace.exe` with a logon trigger scoped to
the current user, **limited** (`TASK_RUNLEVEL_LUA`) run level, and restart-on-failure
(`RestartCount = 3`, `RestartInterval = PT1M`). There is **no service** and **no watchdog** — the
OS scheduler *is* the supervisor and winspace is the sole worker it launches into the session.

The task is synced declaratively by the Worker via COM `ITaskService`: create-or-update when the
flag is on, delete-if-exists when off. See the `SyncAutostart` and `Logon task` glossary entries
in `CONTEXT.md` for the full definition and settings.

## Considered Options

- **Per-user Task Scheduler logon task.** *Chosen.* Registerable and removable by the running
  **unelevated** process (so the `start_at_login` flag toggles it live — the key property), gives
  native restart-on-failure, is per-user by construction, and runs the worker directly in the
  interactive session with no session-crossing or token juggling. Logon triggers are **not**
  subject to the Startup-apps throttle that delays Run-key / Startup-folder items, so it reaches
  the first manageable moment as early as any user-session mechanism can.

- **Session-0 service supervising an interactive worker** (`WTSGetActiveConsoleSessionId` +
  `WTSQueryUserToken` + `CreateProcessAsUser`). *Rejected.* It defeats the `DESIGN.md §7`
  "can't touch the desktop" objection (the service only supervises), but at a cost that is
  specifically wrong here: (a) **installing a service needs admin**, so the flag can no longer be
  toggled live by the unelevated WM — it would fail or fire a UAC prompt every time; (b)
  session-crossing is a bug farm (logon/logoff, fast-user-switching, RDP, `SESSIONCHANGE`, getting
  the worker's token at *user* integrity and not SYSTEM/elevated — a direct hazard to the
  no-elevation posture); (c) it muddies the per-user model (one machine-wide service, which user's
  winspace? whose config?). And it buys nothing on latency: **the worker cannot manage windows
  before the session exists**, so the service still waits for `WTS_SESSION_LOGON` to launch it —
  the same moment the logon task fires, reached through *more* hops.

- **Userspace watchdog (two processes): a supervisor respawns a crashed worker.** *Rejected.* It
  **owns restart-on-failure — a thing Windows gives for free** (against North Star #1, "only own
  what Windows won't give us"), and it does not even remove the autostart problem: the watchdog
  *itself* must be autostarted, so it just relocates the single point of failure into a process
  that, unlike the kernel-backed scheduler, can crash with nothing to restart it.

- **Run key (`HKCU\…\Run`) / Startup folder.** *Rejected.* Simplest to write, but it **cannot
  restart on failure** (the one criterion that motivated the whole choice), is throttled by the
  Startup-apps manager, and the user can silently disable it in Task Manager — drifting the OS
  state away from `start_at_login`.

- **Windows service running the WM directly (no split).** *Rejected* and already excluded by
  `DESIGN.md §7`: a Session-0 service cannot touch the interactive desktop, Virtual Desktops, or
  hotkeys.

## Consequences

- The register/remove path is unprivileged and lives entirely in the running WM, so
  `start_at_login` toggles autostart **live on reload** — no installer, no elevation, no separate
  admin step.
- Autostart is per-user and self-contained under the `\winspace\` task folder: two accounts on one
  machine never collide, and the whole footprint is trivially discoverable and removable.
- winspace gains **no** boot-time / pre-logon / cross-session / privileged capability — it remains
  strictly a per-user interactive process. If a future need for any of those arises, this decision
  must be revisited (it does not compose with the logon-task model).
- A window that opens in the gap before winspace attaches at logon is not orphaned: startup
  **Adoption** inherits it. "Start ASAP" is therefore a tuning concern (trigger delay / task
  priority), not a correctness one.
- Restart-on-failure is the scheduler's, tuned by task settings — winspace itself carries no
  supervision, respawn, or health-check logic.
