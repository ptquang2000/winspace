# winspace — Project Draft

> A Windows 11 workspace + auto-tiling manager that makes windows easier to use by
> leaning on OS-native facilities, staying responsive even under heavy machine load.
> Inspired by i3/hyprland, but built to work *with* Windows, not fight it.

*Status: concept draft. Implementation & internal architecture intentionally deferred.*

---

## 1. North stars

1. **Make windows easier — don't replace how Windows works.** Lean on OS-native
   facilities wherever they exist; only own what Windows won't give us.
2. **Seamless under load.** Switching workspaces must feel instant even during a
   heavy compile. Achieved by delegating to the OS and never running on the input
   critical path (no low-level input hooks, no elevated priority).

---

## 2. Core model

- **Workspaces = OS Virtual Desktops** (`IVirtualDesktopManagerInternal`). A winspace
  workspace *is* a Windows virtual desktop, so taskbar / alt-tab / focus all "just work."
- **Global switch across all monitors.** All displays switch together (native VD
  behavior); each window stays on its own physical monitor across the switch.
- **Dynamic workspaces** — created on demand.
- **Place-once auto-tiler.** Windows auto-tile *when they open*; winspace does **not**
  continuously enforce. Drag a tiled window → it pops to floating. winspace never
  fights the user after initial placement.

---

## 3. Tiling

- **BSP / dwindle** layout, like i3/hyprland.
- winspace **owns geometry** via `SetWindowPos` / `DeferWindowPos`. *(There is no
  programmatic Snap Layout API in Windows — FancyZones/komorebi/GlazeWM all compute
  rectangles themselves; winspace does the same.)*
- **"Stay close to native snap" = visual, not API:** compute the same rectangles native
  snap would (halves/quarters of `rcWork`), respect per-monitor DPI, `rcWork` (taskbar),
  rounded corners (`DWMWA_WINDOW_CORNER_PREFERENCE`), and per-window min sizes.

### Multi-display fill / float lifecycle (the Nth new window)
1. Any display with **zero** tiled windows → place there, fill work area.
2. All displays occupied → **BSP-split** the focused display's focused tile.
3. Splitting would drop a tile below its min size → the window **opens floating**.

- **Min-tile floor:** hard constraint from each window's `WM_GETMINMAXINFO`; plus an
  optional higher global floor (`min_tile_width` / `min_tile_height`).
- Displays treated as one continuous fill order (fill each display, then split focused,
  then overflow, then float) — a deliberate single-canvas feel, unlike i3/hyprland's
  independent-per-monitor model.

---

## 4. Window eligibility

**Tileable** only if all hold: top-level & unowned · `WS_VISIBLE` · `WS_THICKFRAME` +
`WS_CAPTION` · not `WS_EX_TOOLWINDOW` · **not DWM-cloaked** (filters UWP ghost windows) ·
not fullscreen · not excluded by a rule.

Everything else **floats and is left undisturbed** (dialogs sit on top; tiles unchanged).
Fullscreen windows (games/video) auto-float and are never touched. A `windowrule` can
force `float | tile | ignore` per app. *(A bundled default exclusion list is deferred;
the rule mechanism ships.)*

---

## 5. Rules (app → workspace pinning)

- Match order: **exe → class → title (regex)**. **Place-once**, no continuous enforcement.
- Detect new windows at `EVENT_OBJECT_CREATE`, then **cloak → move → uncloak** (DWM
  `DWMWA_CLOAK`) to avoid the cross-desktop placement flash. *(Flash only occurs when the
  target is an inactive desktop; cloaking reduces it to near-zero.)*

---

## 6. Launcher

- `exec` (runs on every config reload) and `exec-once` (startup only) — hyprland idiom.
- An entry can specify a target workspace; winspace launches via `CreateProcess`, so it
  places the app's first window by **PID match** — cleanest, flash-free placement.

---

## 7. Autostart

- winspace runs as a **normal windowless user-session process**, autostarted via a
  **Task Scheduler logon task** with restart-on-failure. *(Not a Windows service — a
  Session-0 service cannot touch the interactive desktop, VDs, or hotkeys.)*
- Opt-in `start_at_login` config flag registers the task.

---

## 8. Hotkeys & dispatchers

- **`RegisterHotKey` only** (kernel-delivered, survives load). **No `WH_KEYBOARD_LL`,
  no elevated priority.** Hyprland-style modifier (e.g. `Win+Alt`).
- Dispatchers (v1):
  - `workspace N`, `movetoworkspace N`, `movetoworkspacesilent N`
  - `focus left|right|up|down`, `movewindow left|right|up|down` — **spatial**, computed
    from window rects, traverse across monitors in virtual-screen coordinates
  - `movetomonitor` / `focusmonitor`
  - `maximize` (fill work area), `resizeactive`, `togglefloat`
  - `exec` / `exec-once`, `reload`, `quit`

---

## 9. Configuration

- **Hyprland-style DSL**: `key = value`, `section { ... }`, `bind = MODS, KEY, dispatcher, args`,
  `exec` / `exec-once`, `windowrule`, workspace rules.
- Reloadable via the `reload` dispatcher.

---

## 10. Deferred (discuss later)

- Internal threading / process architecture (load-resilience mechanics).
- COM Virtual Desktop bridge versioning across Windows builds.
- IPC / CLI (`hyprctl`-equivalent) surface.
- Tech stack, build system, tests.
- Phase-2 tiling: intra-display custom zones, tile-swap on drag, tabbed/stacked layouts.
