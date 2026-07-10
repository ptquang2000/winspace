# 08 — Launcher: exec / exec-once with PID-match placement

**Labels:** `ready-for-agent`

> **"Placement" = Workspace, not geometry.** Per
> [ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md), winspace owns no window
> geometry. A launched app's first window is matched by PID and assigned to a target
> *Workspace* (via 06's cloak-move path); it is never moved or sized on screen. Depends on the
> hook adapter that 07 reintroduces (to catch the app's first window on `Appeared`).

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
- 06 (the cloak-move / VD move path)
- 07 (the hook adapter + `Appeared` stream, to catch the first window by PID)
