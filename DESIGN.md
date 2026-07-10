# winspace — Project Draft

> A Windows 11 workspace + focus manager that makes windows easier to use by
> leaning on OS-native facilities, staying responsive even under heavy machine load.
> Inspired by i3/hyprland, but built to work *with* Windows, not fight it.

*Status: concept draft. Implementation & internal architecture intentionally deferred.*

> **Tiling was dropped.** winspace owns **no window geometry** — it never moves or
> sizes a window. It switches Workspaces and switches keyboard focus. See
> [ADR-0007](docs/adr/0007-drop-tiling-no-window-geometry.md) for the pivot and its
> rationale.

---

## 1. North stars

1. **Make windows easier — don't replace how Windows works.** Lean on OS-native
   facilities wherever they exist; only own what Windows won't give us. winspace
   owns *no* window geometry — windows land where Windows puts them.
2. **Seamless under load.** Switching workspaces and focus must feel instant even
   during a heavy compile. Achieved by delegating to the OS and never running on the
   input critical path (no low-level input hooks, no elevated priority).

---

## 2. Core model

- **Workspaces = OS Virtual Desktops** (`IVirtualDesktopManagerInternal`). A winspace
  workspace *is* a Windows virtual desktop, so taskbar / alt-tab / focus all "just work."
- **Global switch across all monitors.** All displays switch together (native VD
  behavior); each window stays on its own physical monitor across the switch.
- **Dynamic workspaces** — created on demand.
- **No geometry ownership.** winspace never tiles, moves, or resizes a window. It
  *reads* window positions (to resolve directional focus) and *assigns* windows to
  Workspaces (move-to-workspace), but it never writes a window's on-screen rect.

---

## 3. Focus

- **Spatial directional focus.** `focus left|right|up|down` moves keyboard focus to the
  nearest window in that direction, computed from window rectangles in virtual-screen
  coordinates, so traversal crosses monitors naturally.
- **Stateless.** Rects are Probed live on the keypress; nothing is persisted between
  presses, so focus is never resolved against a stale layout. Emits a
  `SetForegroundWindow` Effect — a read of geometry, never a write.

---

## 4. Window eligibility

A window is **Eligible** (a focus candidate, and — later — a rule-match target) only if all
hold: top-level & unowned · `WS_VISIBLE` · `WS_THICKFRAME` + `WS_CAPTION` · not
`WS_EX_TOOLWINDOW` · **not DWM-cloaked** (filters UWP ghost windows) · not fullscreen · not
excluded by a rule.

Everything else is **Ineligible** — skipped by focus and left entirely undisturbed. A
`windowrule` can force `ignore` per app. *(The `WS_THICKFRAME` requirement is a hold-over
from tiling and may be loosened for focus candidacy later.)*

---

## 5. Rules (app → workspace pinning) — *future, PRD 06*

- Match order: fixed field precedence **exe → class → title (regex)**, first match wins.
  **Place-once**, no continuous enforcement; assigns a Workspace only — never geometry.
- Detect new windows on the **`Appeared` edge** (`EVENT_OBJECT_SHOW` / `UNCLOAKED`, never raw
  `CREATE` — the window is half-born there, per ADR-0006/0009), then move them to the target
  Workspace. The internal move reassigns a window's desktop without painting it on the current
  one, so there is no cross-desktop placement flash (an earlier DWM `DWMWA_CLOAK` wrap was
  dropped — `DWMWA_CLOAK` is same-process-only and `E_ACCESSDENIED` on a foreign window).
- The move uses the *internal* `IApplicationViewCollection::GetViewForHwnd` +
  `IVirtualDesktopManagerInternal::MoveViewToDesktop` — see ADR-0010 — because the public
  `IVirtualDesktopManager::MoveWindowToDesktop` returns `E_ACCESSDENIED` for windows winspace
  does not own (proven on the VM), which is every window it actually moves.
- PRD 06 reintroduces the `SetWinEventHook` adapter removed with tiling, on its own thread.

---

## 6. Launcher — *future, PRD 08*

- `exec` (runs on every config reload) and `exec-once` (startup only) — hyprland idiom.
- An entry can specify a target **workspace**; winspace launches via `CreateProcess` and
  assigns the app's first window by **PID match**. Workspace assignment only — no geometry.

---

## 7. Autostart

- winspace runs as a **normal windowless user-session process**, autostarted via a
  **Task Scheduler logon task** with restart-on-failure. *(Not a Windows service — a
  Session-0 service cannot touch the interactive desktop, VDs, or hotkeys.)*
- Opt-in `start_at_login` config flag registers the task.

---

## 8. Hotkeys & dispatchers

- **`RegisterHotKey` only** (kernel-delivered, survives load). **No `WH_KEYBOARD_LL`,
  no elevated priority.** Hyprland-style modifier.
- Dispatchers (v1):
  - `workspace N`, `movetoworkspace N`, `movetoworkspacesilent N`
  - `focus left|right|up|down` — **spatial**, computed from window rects, traverses across
    monitors in virtual-screen coordinates
  - `exec` / `exec-once`, `reload`, `quit`
- **Removed with tiling:** `movewindow`, `movetomonitor`, `focusmonitor`, `maximize`,
  `resizeactive`, `togglefloat` — all either move/size a window (forbidden) or are
  redundant with spatial focus.

---

## 9. Configuration

- **Hyprland-style DSL**: `key = value`, `section { ... }`, `bind = MODS, KEY, dispatcher, args`,
  `exec` / `exec-once`, `windowrule` (workspace assignment / `ignore`), workspace rules.
- Reloadable via the `reload` dispatcher.

---

## 10. Deferred / removed

- **Tiling — removed** (ADR-0007). All BSP/dwindle layout, min-tile floors, drag-to-float,
  and geometry ownership are gone, not deferred.
- Internal threading / process architecture (load-resilience mechanics).
- COM Virtual Desktop bridge versioning across Windows builds.
- IPC / CLI (`hyprctl`-equivalent) surface.
- Loosening the eligibility gate (`WS_THICKFRAME`) for focus candidacy.
