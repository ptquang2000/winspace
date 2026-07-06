# 05 — Spatial directional focus

**Labels:** `ready-for-agent`

## What to build

Directional keyboard focus across the whole virtual screen. A `focus left|right|up|down`
dispatcher moves keyboard focus to the spatially nearest window in the requested direction.

The resolution is **stateless** (see [ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md)):
on the keypress the I/O adapter enumerates the Eligible windows and Probes each one's live
rect (plus the current foreground window); the pure Reducer picks the nearest window in the
requested direction from those rectangles in virtual-screen coordinates — so traversal
crosses monitor boundaries naturally — and emits a single `SetForegroundWindow` Effect. No
focus order is persisted and no geometry is stored; rects are read fresh each keypress.

winspace owns no geometry here: `focus` never moves or sizes a window. The old `movewindow`
tile-swap half of this slice died with tiling.

## Acceptance criteria

- [ ] `focus left/right/up/down` moves the foreground to the spatially nearest Eligible window in that direction
- [ ] Direction resolution traverses across monitors using virtual-screen coordinates
- [ ] Candidate windows are filtered by the Eligibility gate (`isEligible`); tool windows, cloaked UWP hosts, dialogs are skipped
- [ ] A request with no Eligible window in that direction is a no-op
- [ ] The resolution reads rects live at keypress (stateless) — nothing is persisted between presses
- [ ] Reducer tests assert the chosen target for synthetic multi-window / multi-monitor rect sets

## Blocked by

- (none — needs only the Eligibility gate, already landed and renamed)
