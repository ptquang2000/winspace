# 01.07 — End-to-end integration & manual smoke

**Labels:** `ready-for-agent`
**Blocked by:** 01.02, 01.03, 01.04, 01.05, 01.06

## What to build

Wire the full spine together and prove it on the live desktop: read config → register
hotkeys → press → Event → Reducer → `SwitchToWorkspace` Effect → bridge switches the OS
Virtual Desktop. Load the config from a known location at startup, run adoption, then serve
hotkeys until `quit`.

This task adds no new logic — it connects tasks 02–06 and executes the manual smoke script
that is the I/O-layer definition of done (nothing here is unit-testable).

## Manual smoke script (definition of done)

1. **Windowless:** launch → no console, no taskbar button, no Alt-Tab entry; visible only in Task Manager.
2. **Adoption:** with 3 desktops open, start winspace, press `$mod+2` → lands on the *existing* 2nd desktop (no new desktop spawned).
3. **Create-on-demand:** press `$mod+5` (only 3 exist) → exactly one new desktop appears at the tail and activates; `$mod+4` after creates one more (no intermediate filling, no clamp).
4. **GUID-anchored stability:** reorder desktops in Task View, press `$mod+5` again → lands on the *same* desktop created earlier.
5. **Quit:** `quit` Bind → process exits cleanly; no orphan desktops beyond those the test created.
6. **Variant diagnostic:** on the 24H2 machine the log names the resolved 24H2+ IID; forcing a stubbed variant emits the loud "not yet implemented" line.

## Acceptance criteria

- [ ] Config loaded at startup from a known location; parse diagnostics surfaced
- [ ] Pressing a bound `$mod+N` visibly switches the OS Virtual Desktop
- [ ] All six smoke steps pass on the 24H2 dev machine
- [ ] Reducer + config-parser unit tests still green with no live-desktop dependency
- [ ] Clean shutdown on `quit`: threads joined, COM uninitialized, no leaked desktops
