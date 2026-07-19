# FancyZones is a coexistence peer, not a feature to integrate

**Status:** Accepted (2026-07-19). Reaffirms [ADR-0007](0007-drop-tiling-no-window-geometry.md)
against a concrete "bring back tiling via FancyZones" proposal; records the evaluation so the
question is not re-opened from scratch each time it recurs.

winspace deleted auto-tiling and now owns **no window geometry** (ADR-0007). "Can we integrate
PowerToys FancyZones into winspace's tiling feature?" recurs naturally — FancyZones is the most
popular Windows zone-snapper, and a reader who has not internalized ADR-0007 assumes winspace still
has a tiling surface to graft it onto. It does not. This ADR records what FancyZones actually does,
which of its ideas survive ADR-0007, and why the answer is **coexist, do not integrate**.

## Context

FancyZones' mechanism, verified against the PowerToys source (`src/modules/fancyzones/FancyZonesLib`):

- **Drag detection is `SetWinEventHook`, not a mouse hook.** `EVENT_SYSTEM_MOVESIZESTART` /
  `MOVESIZEEND` bracket the drag; `EVENT_OBJECT_LOCATIONCHANGE` (→ `WM_PRIV_LOCATIONCHANGE`) follows
  it. `HandleWinHookEvent` re-posts to a hidden `"SuperFancyZones"` window to leave the hook thread.
  This is the **same out-of-context `SetWinEventHook` family winspace already uses** for
  `Appeared` / `Vanished` — so drag *observation* is not the point of conflict.
- **The snap is a geometry write.** `MoveSizeStart()` builds a per-drag `WindowMouseSnap`;
  `DraggingState` tracks the Shift modifier and drives `ZonesOverlay` highlighting; an `IZoneSet`
  hit-tests cursor → zone(s); on release `WorkArea::Snap()` calls **`SetWindowPos`** to resize and
  reposition the window. This is the defining act of FancyZones and is exactly the `PositionWindow`
  Effect ADR-0007 removed.
- **Keyboard shortcuts sit on a low-level hook.** `FancyZones::OnKeyDown(PKBDLLHOOKSTRUCT)` is a
  `WH_KEYBOARD_LL` hook that returns `true` to *swallow* keys. winspace deliberately uses
  `RegisterHotKey` (ADR-0014) to stay **off the input critical path**.
- **App-zone-history** (`AppZoneHistory` singleton) keys **executable path → last zones**, persisted
  to JSON; `WindowCreated()` re-snaps a launching app to its remembered zone.

## Decision

**winspace does not integrate FancyZones. FancyZones is a coexistence peer.** winspace owns
Workspaces (OS Virtual Desktops) and spatial focus; PowerToys owns zone geometry. The two compose
cleanly and by construction cannot fight: **only FancyZones writes geometry** — winspace never does
(ADR-0007) — so there is no shared resource to contend over, and both happen to observe the window
world through the same `SetWinEventHook` primitive.

Of FancyZones' ideas, exactly one is borrowable under ADR-0007, and it is **not** FancyZones
integration: *remembered per-app placement*. winspace independently arrived at the same shape —
a Place-once **WindowRule** maps `exe: → workspace N` ([ADR-0009](0009-window-rules-place-once-state.md)).
The only borrowable delta is "remember where the user last put it" versus "place once from static
config" — and it stores a **Workspace**, never a rect, so it stays ADR-0007-clean. It is tracked as
a possible future WindowRule enhancement, not as anything FancyZones-shaped.

## Considered Options

- **Coexistence (chosen).** Let PowerToys own zones; winspace owns Workspaces + focus. Zero code,
  no conflict, and users get both. The `SetWinEventHook`-based drag observation and the strict
  no-double-geometry-owner property make the composition unusually robust.
- **Reintroduce zone snapping inside winspace.** *Rejected.* `WorkArea::Snap` → `SetWindowPos` is
  geometry ownership — it resurrects the `layout()` / `PositionWindow` Effect, the stale-rect
  problem, and the DPI/shadow compensation that ADR-0007 spent real effort deleting. This is
  reversing the project's single largest decision, not adding a feature.
- **Adopt the zone overlay UI.** *Rejected.* winspace is windowless by design — no render surface,
  no UI thread. An overlay is a new, always-present visual subsystem serving a geometry feature we
  do not want.
- **Adopt the `WH_KEYBOARD_LL` swallowing hook** (for zone-navigation chords). *Rejected.* It puts
  winspace back on the input critical path, against ADR-0014's `RegisterHotKey` stance.

## Consequences

- The recurring "just add FancyZones" question now has a written home beside ADR-0007; a reader is
  pointed at *why* the tiling surface it assumes does not exist.
- **Users who want zones run PowerToys FancyZones alongside winspace** — a supported, first-class
  configuration, not a workaround. winspace's docs can say so directly.
- The one surviving idea — remembered per-app *Workspace* placement — is captured as a WindowRule
  direction, so the evaluation is not pure negation: it fed exactly one bounded, ADR-0007-safe lead.
- **If a future decision does bring geometry back into winspace,** it must reopen ADR-0007 as a
  unit (geometry ownership, stale-rect strategy, DPI compensation, the input-path posture) — not
  arrive sideways as "we integrated FancyZones."
