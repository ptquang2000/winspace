# 01 — Walking skeleton: config-driven workspace switch

**Labels:** `ready-for-agent`

## What to build

The end-to-end walking skeleton that every later slice plugs into: a windowless
user-session process that parses a minimal config, registers a hotkey, and routes it
through the pure reducer to a virtual-desktop switch.

This slice establishes the core seam. From a prototype, the shape is:

```
reduce(state, event) -> (newState, effects)
  event  : { WorkspaceSwitch { n } } | { Quit } | ...
  effect : { SwitchToWorkspace { logical } } | { Exit } | ...
```

The hotkey thread (`RegisterHotKey` + message loop) translates `WM_HOTKEY` into events
and posts them to the worker; the worker owns the reducer and executes emitted effects.
The `SwitchToWorkspace` effect is executed by the COM Virtual Desktop bridge, which must
select the correct vtable variant at runtime for the running Win11 build
(21H2 / 23H2-KB5034204+ / 24H2+).

**Workspace model — sparse/virtual (i3-style).** A workspace is a *logical number* the
bridge maps to a Virtual Desktop **by GUID**, not by array position. The reducer reasons
only in logical numbers (stays free of `windows.h`); the `logical→GUID` map, startup
adoption, create-on-demand, and live GUID→position resolution all live inside the bridge.
- **Startup adoption:** the bridge enumerates existing desktops and binds logical `1..N`
  to `desktop[0..N-1]` by GUID; `current_workspace` is read from the active desktop.
- **Switch:** hit → resolve stored GUID to its *current* array index, `SwitchDesktop`.
  Miss → `CreateDesktop` (one desktop, appended), bind the logical number, switch. **No
  intermediate filling** (`workspace 5` does not create 1..4) and **no clamp** (`workspace
  500` makes one desktop labeled 500).
- **Reaping** (destroy empty, unfocused workspaces) is **deferred** — it needs window
  tracking (issue 02) to detect "empty". The sparse/GUID-anchored model chosen here is
  what makes that later reaping safe (reaping a middle desktop renumbers OS positions, so
  logical identity must be GUID-anchored). Mid-run reconciliation of desktops created
  outside winspace (Task View) is likewise deferred.

## Acceptance criteria

- [ ] Process runs windowless (no console, no taskbar button) and exits cleanly on a `quit` dispatcher
- [ ] Parses a minimal config: a global modifier and `bind = MODS, KEY, workspace, N`
- [ ] `RegisterHotKey` registers the bound keys (no `WH_KEYBOARD_LL`); `WM_HOTKEY` posts a command event to the worker
- [ ] Reducer emits a `SwitchToWorkspace N` effect for a workspace-switch event — unit tested with no Windows calls
- [ ] COM VD bridge switches the active virtual desktop; correct vtable variant resolved at runtime (24H2+ wires `GetDesktops`, `IVirtualDesktop::GetID`, `CreateDesktop`, `SwitchDesktop`; 21H2 / 23H2-KB variants declared + loud stub)
- [ ] `workspace N` switches to **logical** workspace N — adopts existing desktops as `1..N` at startup; a miss creates a single new desktop (no intermediate filling, no clamp)
- [ ] Pressing the bound hotkey visibly switches the OS virtual desktop
- [ ] Reducer + config-parser tests pass with no live-desktop dependency

## Manual verification (I/O layer — not unit-testable)

Reducer + config parser get Catch2 unit tests; everything Win32/COM is smoke-tested by hand.
Definition of done for the I/O layer:

1. **Windowless:** launch → no console, no taskbar button, no alt-tab entry; visible only in Task Manager.
2. **Adoption:** with 3 desktops already open, start winspace, press `$mod+2` → lands on the *existing* 2nd desktop (no new desktop spawned).
3. **Create-on-demand:** press `$mod+5` (only 3 exist) → exactly one new desktop appears at the tail and is activated; `$mod+4` after creates one more (no intermediate filling, no clamp explosion).
4. **GUID-anchored stability:** reorder desktops in Task View, press `$mod+5` again → lands on the *same* desktop created earlier (live position resolution, GUID identity).
5. **Quit:** `quit` bind → process exits cleanly; no orphan desktops beyond those the test created.
6. **Variant diagnostic:** on the 24H2 dev machine the log names the resolved 24H2+ IID; forcing a stubbed variant emits the loud "not yet implemented" line.

## Blocked by

None — can start immediately.
