# 10 — Autostart via Task Scheduler logon task

**Labels:** `ready-for-agent`

## What to build

Make winspace start with the user session, the Windows-blessed way. The `start_at_login`
config flag registers a Task Scheduler **logon** task that launches winspace windowless at
sign-in, configured to restart on failure. Disabling the flag removes the task. This is
explicitly **not** a Windows service (a Session-0 service cannot touch the interactive
desktop, virtual desktops, or hotkeys).

## Acceptance criteria

- [ ] Enabling `start_at_login` registers a logon task that launches winspace windowless at sign-in
- [ ] The task is configured to restart on failure
- [ ] Disabling `start_at_login` removes the task
- [ ] Registration is idempotent — no duplicate tasks on repeated enable

## Blocked by

- 01
