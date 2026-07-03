# 05 — Spatial directional focus + move

**Labels:** `ready-for-agent`

## What to build

Directional navigation across the whole virtual screen. `focus left|right|up|down` and
`movewindow left|right|up|down` dispatchers; the reducer resolves the nearest window in the
requested direction from window rectangles in virtual-screen coordinates, so traversal
crosses monitor boundaries naturally. `focus` emits a `SetForegroundWindow` effect; `move`
swaps the focused tile with its neighbor and reflows.

## Acceptance criteria

- [ ] `focus left/right/up/down` moves foreground to the spatially nearest window in that direction
- [ ] Direction resolution traverses across monitors using virtual-screen coordinates
- [ ] `movewindow` swaps the focused tile with the neighbor and reflows the layout
- [ ] Requests with no window in that direction are a no-op
- [ ] Reducer tests assert the chosen target for synthetic multi-window / multi-monitor layouts

## Blocked by

- 03
