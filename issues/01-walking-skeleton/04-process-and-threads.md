# 01.04 — Windowless process + two-thread wiring

**Labels:** `ready-for-agent`
**Blocked by:** 01.01, 01.03

## What to build

The windowless process shell and the two-thread backbone the whole product plugs into.
No tiling, no window tracking — just the Hotkey thread → Worker thread spine carrying
Events and executing Effects.

- **Windowless `WinMain`** — `/SUBSYSTEM:WINDOWS`, no console, no taskbar button, no
  Alt-Tab entry. Visible only in Task Manager.
- **Worker thread** — `CoInitializeEx(COINIT_APARTMENTTHREADED)`; creates a message-only
  (`HWND_MESSAGE`) window **in its constructor** so the HWND is valid before anything
  posts; runs a `GetMessage` loop whose `WndProc` calls `reduce` on each incoming Event
  and executes the emitted Effects. Sole owner of State, the Reducer, and (later) the COM
  bridge. Executes `Exit{}` by ending the process cleanly.
- **Hotkey thread** — a thread with its own `GetMessage` loop, ready to receive `WM_HOTKEY`
  (actual `RegisterHotKey` wiring is task 05).
- **Transport** — Hotkey thread hands an Event to the Worker via `PostMessage` to the
  Worker's message-only HWND (`WM_APP`, heap-allocated `Event*` in `LPARAM`); the Worker
  takes ownership and `delete`s after reducing.

For this task, a temporary internal trigger (e.g. posting a `Quit` Event) is acceptable to
prove the clean-exit path end-to-end before hotkeys exist.

## Acceptance criteria

- [ ] Launch → windowless (no console/taskbar/Alt-Tab); appears only in Task Manager
- [ ] Worker `CoInitializeEx` STA; message-only HWND created in ctor, valid before the loop
- [ ] Posting an Event to the Worker HWND routes it through `reduce`; emitted Effects execute on the Worker thread
- [ ] A `Quit` Event → `Exit` Effect → process exits cleanly (threads joined, COM uninitialized)
- [ ] Transport uses `PostMessage` + heap-`Event*` ownership transfer (never `PostThreadMessage`)
- [ ] No `WH_KEYBOARD_LL`; no priority escalation
