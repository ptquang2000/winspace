# 08 — Launcher: exec / exec-once with PID-match placement

**Labels:** `ready-for-agent`

## What to build

Launch apps from config and place them cleanly. `exec-once` entries run at startup; `exec`
entries also run on every reload. winspace `CreateProcess`-es each entry, then matches the
app's first top-level window **by PID** (more reliable than exe/title) and places it on the
entry's target workspace via the cloak-move path from slice 06. `exec-once` is idempotent
on reload (does not relaunch an already-running app).

## Acceptance criteria

- [ ] `exec-once` entries launch at startup; `exec` entries also run on reload
- [ ] A launched app's first window is matched by PID and placed on its target workspace flash-free
- [ ] `exec-once` does not relaunch an app that is already running on reload
- [ ] Launch entries run in config order
- [ ] Tests assert launch + placement effects for synthetic exec / exec-once config

## Blocked by

- 01
- 06
