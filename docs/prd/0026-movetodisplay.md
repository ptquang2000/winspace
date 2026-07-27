# PRD 0026 — `movetodisplay`: send the focused window to the next Display

**Status:** Ready for agent
**ADR:** none (reuses the existing `PositionWindow` Effect and ADR-0016's bounded geometry
write; the directional-not-index rationale is recorded in `CONTEXT.md` → **MoveToDisplay**)
**Lands:** independent — may go any time after [PRD 0025](0025-fullscreen-is-not-maximized.md)

## Problem Statement

winspace can move a window between **Workspaces** (`movetoworkspace`) but not between
**Displays**. On a multi-monitor desk that is the more common motion by far: the window
opened on the wrong monitor, or **Distribute** put it on the least-occupied one and the user
wants it elsewhere. The only remedy today is dragging it with the mouse — in a keyboard-driven
window manager.

Worse, a user porting a Hyprland config writes `movetomonitor` and gets a diagnostic saying
it was **"removed with tiling"** — which reads as "this idea is dead" when in fact the
capability exists and is one keybind away.

## Solution

`movetodisplay left|right|up|down` — move the foreground window to the neighbouring Display
in that Direction and maximize it there.

The destination is named **directionally, never by index**. Display indices are not a stable
public thing: `EnumDisplayMonitors` order is not user-visible, does not match the numbers in
Windows Display Settings, and is not stable across a replug or a dock — so a bind to `2` would
silently mean a different monitor tomorrow. A Direction means the same thing every day.

The geometry write itself already exists: `PositionWindow{target, slot}` moves a window to a
named Display and maximizes it there, including the "move first, then maximize so the OS
reports it maximized *there*" handling.

## User Stories

1. As a multi-monitor user, I want `movetodisplay right` to send the focused window to the
   Display on my right, so that I can rearrange my desk without the mouse.
2. As a multi-monitor user, I want the moved window to keep focus, so that I can keep typing
   into it after moving it.
3. As a user, I want the window maximized on arrival, so that it looks the same as every
   other auto-placed window.
4. As a user with a vertically stacked monitor, I want `movetodisplay up`/`down` to work, so
   that the feature matches my physical layout rather than assuming a horizontal row.
5. As a user, I want a move with no Display in that Direction to do nothing, so that pressing
   the key at the edge of my desk is harmless.
6. As a single-Display user, I want the bind to be a silent no-op, so that a shared config
   works on my laptop and my desk without edits.
7. As a user, I want `movetodisplay` to move the window **only** between Displays and never
   between Workspaces, so that its effect is predictable and never loses a window to another
   desktop.
8. As a user, I want to move the same window repeatedly across Displays, so that the action is
   not spent after one use.
9. As a user, I want moving a window not to consume its one **place-once** placement, so that
   an explicit action of mine never disables an automatic behavior later.
10. As a user, I want `tile` afterwards to still rebalance normally, so that the two features
    compose rather than fight.
11. As a user porting a Hyprland config, I want `movetomonitor` to tell me the dispatcher was
    **renamed to `movetodisplay`**, so that I can fix my config in one step instead of
    concluding the feature does not exist.
12. As a user who binds an unknown Direction, I want a config Diagnostic naming the four valid
    ones, so that a typo is a message rather than a dead key.
13. As a user with an **Ignore**d window focused, I want `movetodisplay` to respect that the
    window is left alone, so that Ignore keeps meaning "don't touch at all".
14. As a maintainer, I want the destination resolved by a pure function over Display rects, so
    that multi-monitor behavior is testable on a single-monitor CI machine.

## Implementation Decisions

- **New Dispatcher `MoveToDisplay`**, taking a `Direction` — reusing the same
  `left|right|up|down` argument vocabulary and parsing path as `focus`. Config surface:
  `bind = $mod SHIFT, L, movetodisplay, right`.
- **`movetomonitor`'s diagnostic changes** from the "removed with tiling" family to a
  targeted **"renamed to `movetodisplay`"**. This is the glossary asserting itself on the
  config surface — `CONTEXT.md` fixes **Display** as the term and lists *Monitor* under
  `_Avoid_`. The old message is now factually wrong and must not survive.
- **A two-phase Probe round-trip**, mirroring Spatial focus and Distribute exactly, because
  the Reducer cannot enumerate Displays: `MoveToDisplay{dir}` Event → `ResolveMoveToDisplay`
  Effect → the Worker probes the live Display list **with their rects**, plus the foreground
  window and its current Display → `MoveToDisplayResolve{dir, …}` Event → the Reducer picks
  the target and emits `PositionWindow`.
- **A pure resolution**, shaped like `resolveFocus`: from the subject's current Display centre,
  the nearest Display whose centre lies strictly ahead in the Direction, ties broken
  deterministically by the opaque `MonitorId` so the result never depends on enumeration
  order. Nothing ahead → `nullopt` → no Effect. The single-Display no-op falls out of this
  with no special case.
- **Reuses `PositionWindow{target, Slot::Maximized}`.** No new geometry Effect, no widening of
  ADR-0007's ban beyond ADR-0016's existing reopening. Under ADR-0020 nearly every window is
  already maximized, so the fixed Slot is invisible in the common case; `tile` remains the
  button that restores a Slot rule.
- **No State is touched.** It neither consults nor writes `placed` — explicit and repeatable,
  exactly like `tile`. It **never** emits `MoveWindowToWorkspace`.
- **Ignore is honored.** A foreground window in the **Ignore-set** yields no Effect, matching
  the "left entirely alone" widening ADR-0020 gave the action.
- **No foreground window → no-op**, matching `focus`'s treatment of a missing Origin.

## Testing Decisions

A good test asserts **which Display the Reducer chooses, and what Effect it emits**, over
synthetic Display rects — never how the monitors were enumerated. The Display list arrives as
plain data, so the entire feature is decidable at the pure seam.

- **Seam 1 — the existing pure reducer seam** (`src/winspace_test.cpp`, Catch2), tagged
  `[reducer][movetodisplay]`:
  - `MoveToDisplay{dir}` emits exactly `ResolveMoveToDisplay` and leaves State untouched;
  - a two-Display horizontal layout resolves `right` to the right Display and emits
    `PositionWindow{that display, maximized}`;
  - `left` from the leftmost Display is a no-op;
  - a vertically stacked layout resolves `up`/`down` correctly;
  - a three-Display row resolves to the **adjacent** Display, not the farthest;
  - single Display → no-op;
  - no foreground window → no-op;
  - a foreground window in the **Ignore-set** → no-op;
  - State is unchanged in every case, and `placed` is neither read nor written;
  - no `MoveWindowToWorkspace` is ever emitted;
  - ties are broken deterministically — shuffling the input Display order does not change the
    result.
- **Seam 2 — the existing config seam** (`[config]` tests, already 55 cases):
  - `movetodisplay` parses with each of the four Directions;
  - an invalid Direction is a Diagnostic naming the valid four;
  - `movetomonitor` produces the **renamed-to** Diagnostic, and specifically **not** the
    "removed with tiling" one — assert the new message, or the rename is untested.
- **No Smoke seam.** ADR-0008 records that the VM harness is **single-display**, so this
  feature cannot be exercised live there; on a single Display it is a no-op by design. This is
  a deliberate omission with a stated reason, matching how ADR-0008 already leaves
  cross-monitor focus resolution to the reducer seam.
- **Prior art:** the `[reducer][focus]` cases for directional resolution over synthetic rects,
  and the `[reducer][distribute]` cases for the probe-round-trip Effect shape.

## Out of Scope

- `next`/`prev` cycling forms — rejected for the same instability that rules out indices.
- Preserving the window's relative size on arrival. Would need a non-symbolic geometry write
  and would go stale across a DPI change — the exact failure ADR-0007 rejected stored rects
  over.
- Re-running the rule match on arrival to reapply a **Slot** rule. `tile` already does this.
- Moving a **non**-foreground window, or moving multiple windows at once.
- A `focusdisplay` dispatcher (moving *focus* between Displays already works via `focus`,
  once [PRD 0025](0025-fullscreen-is-not-maximized.md) lands).
- Making the VM harness multi-display.

## Further Notes

This is the smallest of the four: one Dispatcher, one pure resolution function, one Effect
reused unchanged, and one corrected Diagnostic. It is listed last only because it is the least
urgent, not because it depends on anything but PRD 0025 — and even that dependency is soft.
It is worth landing near PRD 0025 regardless, since the two together are what make a
multi-monitor desk keyboard-navigable: 0025 moves focus across Displays, 0026 moves windows.
