# 01.05 — Hotkey adapter + Mod/Key → Win32 translation

**Labels:** `ready-for-agent`
**Blocked by:** 01.02, 01.04

## What to build

The I/O adapter that turns parsed Binds into live OS hotkeys and turns presses into Events.
This is the sole place that knows Win32 `MOD_*`/`VK_*`.

- **Translation** — map core `Mod` flags → `MOD_*` and core `Key` → `VK_*` via two small
  tables. Core stays pure; this adapter owns the coupling.
- **Registration** — on the Hotkey thread, call `RegisterHotKey` for each Bind (never
  `WH_KEYBOARD_LL`). Check every return value; on `ERROR_HOTKEY_ALREADY_REGISTERED` emit a
  clear diagnostic naming the combo and continue with the rest.
- **Delivery** — translate `WM_HOTKEY` → the corresponding Event (a `workspace N` Bind →
  `WorkspaceSwitch{N}`, a `quit` Bind → `Quit{}`) and post it to the Worker (task 04's
  transport).

## Acceptance criteria

- [ ] Each parsed Bind is registered via `RegisterHotKey` on the Hotkey thread
- [ ] `WM_HOTKEY` for a `workspace N` Bind posts `WorkspaceSwitch{N}`; a `quit` Bind posts `Quit{}`
- [ ] A combo already owned by another app → diagnostic naming the combo; other Binds still register
- [ ] `Mod`/`Key` → Win32 translation lives only in this adapter; core references no Win32 constants
- [ ] Matched combo is consumed by the system (foreground app never sees it); unbound combos pass through
- [ ] Manual: pressing a bound `$mod+N` reaches the Worker as the expected Event (observable via task 06's switch)
