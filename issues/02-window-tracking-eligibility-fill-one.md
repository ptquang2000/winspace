# 02 — Window tracking + eligibility + fill-one-window

**Labels:** `ready-for-agent`

## What to build

Teach winspace about windows. An out-of-context `SetWinEventHook` adapter feeds window
create/show/destroy into the reducer as plain-data events. The reducer maintains the set
of tileable windows using the eligibility gate, and positions a single tileable window to
fill the focused monitor's work area. Ineligible windows are tracked as floating and left
untouched.

Eligibility gate (all must hold): top-level and unowned · `WS_VISIBLE` · `WS_THICKFRAME` +
`WS_CAPTION` · not `WS_EX_TOOLWINDOW` · not DWM-cloaked · not fullscreen.

## Acceptance criteria

- [ ] Event-hook adapter runs on its own thread and translates create/show/destroy into reducer events
- [ ] Eligibility gate classifies windows; UWP cloaked host windows and tool windows are excluded
- [ ] A single eligible window is positioned to fill the focused monitor work area (`rcWork`, DPI-correct)
- [ ] Dialogs / non-`WS_THICKFRAME` / cloaked windows remain floating and are never moved
- [ ] Fill is visually flush via `DWMWA_EXTENDED_FRAME_BOUNDS` compensation
- [ ] Reducer tests assert position effects for synthetic window-create/destroy events

## Blocked by

- 01
