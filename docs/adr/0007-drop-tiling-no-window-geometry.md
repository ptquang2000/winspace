# Drop auto-tiling: winspace owns no window geometry

winspace began as a workspace **+ auto-tiling** manager. We have dropped tiling
entirely. winspace now owns **no window geometry** — it never moves or sizes a
window: no `layout()`, no `PositionWindow` Effect, no drop-shadow / DPI
compensation. It remains a **workspace switcher** (OS Virtual Desktops) and gains
**spatial focus switching** (`focus left|right|up|down`), which resolves the
nearest window from window rectangles *probed live at keypress time* and emits a
`SetForegroundWindow` Effect — a read of geometry, never a write.

The continuous window-tracking machinery that existed only to serve tiling —
`State.focusOrder`, `State.windowMonitor`, the `SetWinEventHook` adapter, and the
`Appeared` / `Vanished` event stream — is removed from master. The Eligibility
gate survives (renamed **Tileable → Eligible**): the stateless focus sweep still
needs it to skip tool windows, cloaked UWP hosts, and other non-application
windows.

## Considered Options

- **Keep the hook adapter wired-but-dormant** (Reducer ignores `Appeared` /
  `Vanished` until PRD 06 adds handlers). Rejected: an out-of-context
  `SetWinEventHook` thread posting events into a Reducer that discards them is a
  running cost with zero current behavior — against winspace's "own only what you
  must, work with the scheduler" philosophy.
- **Store window rects in `State` to serve focus.** Rejected: winspace no longer
  owns geometry, so window positions are user-volatile — a stored rect goes stale
  on the next drag and would force location-change hooks just to stay fresh.
  Probe rects on demand at keypress instead (consistent with ADR-0006's reactive
  Probe).
- **Keep tiling.** Rejected — the product no longer includes it.

## Consequences

- Master carries exactly the live features. Issues 03 (BSP), 04 (multi-display
  fill / float overflow), and 07 (togglefloat / drag-to-float / fullscreen) are
  dropped; issue 05 is reduced to its spatial-**focus** half (`movewindow`, a
  tile-swap, dies with tiling).
- Focus is **stateless**: no persisted focus order, no dependency on
  `EVENT_SYSTEM_FOREGROUND` tracking; rects are read fresh each keypress and are
  never stale.
- Rules (PRD 06) and the launcher (PRD 08) remain planned as separate PRDs. PRD
  06 — the hook's first genuine consumer — reintroduces the `SetWinEventHook`
  adapter and the `Appeared` / `Vanished` stream when it lands. The design is
  preserved in git history and in PRD 0002.
