# Bounded window geometry: rule-targeted, place-once, on-demand

**Status:** Accepted (2026-07-19). Reopens the geometry ban of
[ADR-0007](0007-drop-tiling-no-window-geometry.md) in the **narrowest** form that
delivers auto-placement; refines [ADR-0015](0015-fancyzones-evaluated-coexistence-over-integration.md)
(FancyZones stays a coexistence peer for *manual* zoning); extends
[ADR-0009](0009-window-rules-place-once-state.md) (the `WindowRule` Place-once
machinery this rides on). On acceptance it supersedes ADR-0007 *only* on the point
below — every other reason ADR-0007 gave still stands.

## Context

The recurring "bring back tiling" want (ADR-0015) surfaced again, but sharpened by
the user into a shape neither ADR-0007 nor ADR-0015 evaluated:

> *Automatically place a window into a layout, but **only** windows that match a
> configured rule — leave everything else as stock Windows (natural float /
> fullscreen), so users are never surprised by a window they didn't ask winspace
> to manage.*

Grilled to a decision, this is **not** dynamic auto-tiling and **not** FancyZones:

- **Tile-allowlist, not float-allowlist.** A matched window gets placed; an
  unmatched window is touched by nothing — the exact "won't confuse users"
  property. This is the inverse of an i3/komorebi auto-tiler, where *tiled* is the
  default and float is the exception.
- **Fixed per-rule target, not a computed layout across the matched set.** Each
  rule names *where* independently (`Slack → right half`). Window X's placement
  never depends on windows Y, Z — so there is **no rebalancing**, hence no
  continuous tracking and no re-writes. (A layout that splits the matched set 50/50,
  thirds, BSP is a genuinely different, larger project — see Considered Options.)
- **Place-once, plus an explicit on-demand re-tile.** A matched window is placed
  once on `Appeared` and then left alone forever (drag it away — winspace honors
  it). A new `tile` dispatcher lets the user say "apply my rules to everything open
  *now*" on demand.

FancyZones cannot serve this even as a delegate. Its only automatic-placement path
is `AppZoneHistory` — **executable → last zone**, populated by the user manually
snapping once (ADR-0015). There is no supported programmatic "place window X in
zone Y" call in FancyZones, and Windows 11 Snap exposes none for arbitrary targets
either. So a delegate design ("winspace orchestrates, someone else writes
geometry") has **no seam to attach to**. Delivering the want means **winspace owns
the `SetWindowPos` write** — the one thing ADR-0007 removed.

## Decision

winspace reintroduces window geometry as **one further bounded write Effect**, driven
only by rule matches and an explicit command, never continuously. (This ADR was drafted
against a base without [ADR-0015](0015-spread-bounded-geometry-write-to-empty-display.md)'s
Spread; on integration both landed, so `PositionWindow` is one of **two** bounded geometry
Effects — the Slot writer here, and Spread's `SpreadWindow` onto an Empty Display.)

- **`WindowRule` Place gains an optional geometry target — a `Slot`.** A Place rule
  becomes `exe: → workspace N [+ slot S]`. A `Slot` is a **symbolic named fraction
  of a Display work area** — `left-half`, `right-half`, `top-left-quarter`,
  `maximized`, … — *not* a stored rect. winspace computes the rect from the target
  Display's work area at write time. (A `Slot` is winspace's config-declared,
  computed placement; deliberately a different word from a FancyZones *zone*, which
  is user-drawn and geometry winspace never owns — see ADR-0015.)

- **New Effect: `PositionWindow{ WindowId, Slot }`.** The Reducer stays symbolic —
  it emits the `Slot`, never a rect. The adapter resolves `Slot` → rect against the
  window's current monitor work area and calls `SetWindowPos`. It is the only geometry
  Effect this ADR adds; it coexists with ADR-0015's `SpreadWindow` (Spread moves a
  window to a whole Display at natural size — this writes a computed fraction of the
  window's own monitor).

- **Two triggers, both place-once in posture:**
  1. **On `Appeared`** — reuses ADR-0009's path verbatim. When an Eligible window's
     first-match is a Place rule carrying a `Slot`, emit `PositionWindow` alongside
     the existing workspace placement. The `placed` set already guarantees
     once-per-lifetime; no new state.
  2. **On the `tile` dispatcher** — a new Bind → the adapter runs an `EnumWindows`
     Probe sweep (reactive Probe, ADR-0006, exactly like the Spatial-focus sweep)
     → for every live window whose first-match is a Slot-bearing Place rule, emit
     `PositionWindow`. Stateless: it consults no `placed` set (it is an *explicit*
     user re-tile) and stores nothing.

- **No stored rects — nothing can go stale.** State holds only the symbolic `Slot`
  on the rule (parsed once from config, per ADR-0009). The rect is computed at the
  moment of the write and immediately forgotten. This is the direct answer to
  ADR-0007's stale-rect rejection: there is no rect to keep fresh.

- **No continuous enforcement.** After a write winspace never re-touches the window
  until it relaunches (a new lifetime, re-placed per place-once) or the user fires
  `tile`. A window dragged away stays where the user put it. The
  `EVENT_OBJECT_LOCATIONCHANGE` follow-hook FancyZones uses is **not** adopted.

- **DPI / extended-frame compensation stays in the adapter.** ADR-0007 called out
  drop-shadow / DPI math as a cost of geometry. It returns, but bounded to the
  adapter: it resolves the visual-vs-window-frame delta via
  `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` when computing the rect. The
  Reducer never sees a pixel.

- **Input path untouched (ADR-0014).** `tile` is a `RegisterHotKey` Bind like every
  other Dispatcher — no `WH_KEYBOARD_LL`, no mouse hook, no drag-follow.

## Considered Options

- **Delegate the write to FancyZones / Windows Snap** (keep ADR-0007 literally
  intact). *Rejected — no seam.* FancyZones' automatic placement is exe-keyed
  `AppZoneHistory` populated by manual snaps; there is no programmatic "snap window
  X to zone Y" API, and Windows Snap exposes none for arbitrary targets. The
  delegate design has nothing to call. ADR-0015's coexistence answer remains correct
  for *manual* zoning, but it cannot deliver *rule-targeted automatic* placement.

- **Computed dynamic layout across the matched set** (matched windows share and
  rebalance the screen — 50/50, thirds, BSP). *Rejected (out of scope).* Window X's
  rect would depend on the live set of matched windows, so it must re-run on every
  matched `Appeared` / `Vanished` / move — resurrecting continuous `SetWinEventHook`
  location tracking, a live layout model, and repeated writes: ADR-0007 issues 03
  (BSP) and 04 (fill) in full. This ADR is deliberately the fixed-target subset.

- **Store computed rects in `State`.** *Rejected.* ADR-0007's stale-rect problem —
  a stored rect goes stale on the next drag or DPI change. Store the symbolic `Slot`
  and compute the rect live instead.

- **Continuous enforcement (snap back when the user drags a placed window away).**
  *Rejected.* Not place-once; it fights the user and needs the location-change hooks
  ADR-0007 removed and ADR-0009 declined. Consistent with ADR-0009's place-once.

- **A zone-overlay UI (drag-to-snap highlighting).** *Rejected — unchanged from
  ADR-0015.* winspace is windowless; an always-present render surface serves a
  geometry-picking interaction this feature does not have (placement is declared in
  config, not dragged).

## Consequences

- ADR-0007's geometry ban is reversed **narrowly and by construction**: exactly one
  write Effect (`PositionWindow`), fired only on a rule-matched place-once
  `Appeared` or an explicit `tile`, never continuously, never storing a rect. Every
  *reason* ADR-0007 gave — stale rects, the cost of continuous tracking, staying off
  the input path — is preserved, none resurrected. A reader who finds
  `PositionWindow` after ADR-0007 should read this ADR.

- ADR-0015 is **refined, not contradicted.** "Coexist with FancyZones" stands for
  arbitrary *manual* zoning; winspace additionally owns *config-declared, rule-
  targeted, place-once* geometry — a thing FancyZones structurally cannot do
  (exe-keyed history vs. per-rule declared target). The two still cannot fight:
  winspace writes only on a matched `Appeared` or explicit `tile`, and only for
  allowlisted windows.

- **`CONTEXT.md` revives geometry vocabulary deliberately** (not ad hoc): introduce
  `Slot` and `PositionWindow`; extend the `WindowRule` and `Place-once` entries to
  carry the geometry target; loosen the `Tileable (dead)` / "owns no geometry"
  markers to "owns geometry only via rule-targeted place-once `PositionWindow`." The
  `tile` Dispatcher joins the Dispatcher list. This is a `domain-modeling` pass, done
  as part of the work.

- **New VM smoke seams** (ADR-0005; the tiling seams ADR-0007 retired are
  re-authored, not revived verbatim): `place-on-appear-with-slot` (a matched window
  lands in its Slot on open), `tile-command` (the on-demand sweep places all matched
  live windows), `drag-away-not-yanked` (a placed window the user moves is not
  re-placed — proves no continuous enforcement), `unmatched-untouched` (a
  non-matching window is never moved — proves the allowlist).

- Input-path posture (ADR-0014) and windowless design are untouched — the only new
  surfaces are one Effect, one Dispatcher, and one config token (`slot`).

- **If a future decision wants the rebalancing layout** (the rejected computed-set
  option), it reopens ADR-0007's continuous-tracking half as its own unit — it does
  not arrive as an extension of this ADR.
