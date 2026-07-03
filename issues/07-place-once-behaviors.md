# 07 — Place-once behaviors: togglefloat / drag-pops-to-float / fullscreen

**Labels:** `ready-for-agent`

## What to build

The behaviors that make winspace a place-once tiler rather than a strict enforcer.
`togglefloat` converts the focused window between tiled and floating. Dragging a tiled
window (`EVENT_SYSTEM_MOVESIZEEND`) pops it out of the layout to floating and reflows the
remaining tree. Fullscreen/borderless windows (games, video) are detected, auto-floated,
and excluded from reflow so they are never disturbed.

## Acceptance criteria

- [ ] `togglefloat` converts the focused window between tiled and floating, reflowing the tree
- [ ] Dragging a tiled window (`MOVESIZEEND`) pops it to floating; the tree reflows without it
- [ ] Fullscreen/borderless windows auto-float and are never repositioned
- [ ] Popped or floating windows are not re-tiled on later lifecycle events
- [ ] Reducer tests cover float-toggle and pop-to-float transitions

## Blocked by

- 03
