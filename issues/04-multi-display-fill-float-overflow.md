# 04 — Multi-display fill order + float overflow

**Labels:** `ready-for-agent`

## What to build

Extend the reducer to treat all displays as one continuous fill order and to fall back to
floating when tiling would become unusable. Placement for the Nth new window:

1. Any display with zero tiled windows → place there, fill work area.
2. All displays occupied → BSP-split the focused display's focused tile.
3. A resulting tile would drop below the min floor → the window opens floating instead.

Min floor = each window's `WM_GETMINMAXINFO` min-size (hard) plus an optional higher global
`min_tile_width` / `min_tile_height`.

## Acceptance criteria

- [ ] New windows fill an empty display before splitting an occupied one
- [ ] When all displays are occupied, the focused display's focused tile splits
- [ ] A window that would create a sub-min tile opens floating instead of tiling
- [ ] Min floor honors per-window `WM_GETMINMAXINFO` and the optional global config value
- [ ] A monitor layout-change event re-tiles correctly
- [ ] Reducer tests cover 2-display fill / overflow / float sequences

## Blocked by

- 03
