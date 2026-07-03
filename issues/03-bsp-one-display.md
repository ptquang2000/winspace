# 03 — BSP tiling on one display (split + reclaim)

**Labels:** `ready-for-agent`

## What to build

Turn single-window fill into a real BSP/dwindle layout on one display. Each new window
splits the focused tile, alternating orientation; destroying or minimizing a window
reclaims its space and reflows the subtree. Reflow bursts are coalesced and committed as a
single batched `BeginDeferWindowPos`/`DeferWindowPos`/`EndDeferWindowPos` pass using
`SWP_ASYNCWINDOWPOS`.

## Acceptance criteria

- [ ] Opening additional windows produces a dwindle BSP layout on one display
- [ ] Closing or minimizing a window reflows siblings to reclaim the space
- [ ] A burst of window events coalesces into a single batched position commit
- [ ] Positioning uses `SWP_ASYNCWINDOWPOS` so a hung/busy app cannot stall reflow
- [ ] Reducer tests assert full window→rect maps for add/remove sequences

## Blocked by

- 02
