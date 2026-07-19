# Spread: a bounded one-shot geometry write to place a window on an empty display

**Status:** Accepted (2026-07-18)

[ADR-0007](0007-drop-tiling-no-window-geometry.md) declared that winspace owns **no
window geometry** — "never moves or sizes a window: no `layout()`, no `PositionWindow`
Effect" — and the `Effect` variant enforces it by comment to this day. This ADR
records the single, deliberate exception: a new **Spread** WindowRule action that, on a
matching window's `Appeared`, relocates it once onto an **Empty Display** with a
geometry *write*. It is the geometry twin of [ADR-0009](0009-window-rules-place-once-state.md),
which reintroduced bounded per-window *state* against the same ADR-0007 — same shape of
carve-out (one-shot, place-once, never re-asserted), applied to geometry instead of state.

## Context

The ask was "move a launched window to an empty display." Per
[ADR-0011](0011-launcher-launch-only-placement-via-windowrule.md) the launcher keeps
**nothing** about its child (no PID, no handle), so the only handle winspace has on an
appearing window is its `WindowIdentity`. The feature therefore *must* be a **WindowRule
action** keyed on `exe`, fired on `Appeared`, exactly like **Place** and **Ignore** — not
a launcher capability. It becomes a third `RuleAction`, **Spread**.

Two mechanisms could reach OS-native placement without owning geometry, and both were
rejected for *this* behavior:

- **Synthesized `Win`+`Shift`+arrow** (the input-synthesis path the VM harness's **Trigger**
  already uses). It acts on the **foreground** window, so a background window that just
  `Appeared` would have to be foregrounded first — a racy two-step on the burst path. And it
  moves to the *adjacent* monitor, cycling, so hitting a *specific* Empty Display means
  modelling the OS monitor-cycle order and synthesizing a counted number of presses. Wrong
  tool for programmatic per-window placement.
- **Keeping winspace geometry-free by doing nothing** — i.e. not building the feature. The
  product wants the behavior.

## Decision

- **`RuleAction` gains `Spread`.** Config spelling: `windowrule = exe:foo.exe, spread`. It
  carries no Workspace number (unlike Place). It fires on the **first Eligible `Appeared`**
  and is **place-once**: recorded in the existing `placed` set (ADR-0009), erased on
  `Vanished`, never re-asserted on uncloak or reload. A window the user later drags is **not**
  yanked back.
- **A new geometry Effect** — `PositionWindow{ WindowId id, MonitorId target }` — is added to
  the `Effect` variant. The Worker executes it with a single `SetWindowPos` onto `target`'s
  work area. This is winspace's **only** geometry write; it happens **once**, at placement,
  and winspace never touches the rect again. There is no `layout()`, no continuous
  maintenance, no location-change hook. The ADR-0007 ban is reduced from "never" to "never,
  except one bounded write emitted only by Spread."
- **Empty Display is computed statelessly, via a two-phase Probe round-trip** mirroring
  Spatial focus (ADR-0008) — the pure Reducer cannot enumerate windows, so when a Spread rule
  matches it emits a `ProbeDisplays`-style request; the adapter enumerates top-level windows,
  maps each Eligible one's rect to its `MonitorId` (`MonitorFromRect`), and posts the set of
  **occupied** Displays back as an Event; the pure Reducer then picks a `MonitorId` **not** in
  that set and emits `PositionWindow`. **No occupancy is persisted** — the deferred
  per-Display occupancy model (the one Reaping still waits on) is **not** built here.
- **Exclude self.** The subject window has already appeared *somewhere*, so the enumeration
  drops its own `WindowId`; otherwise its opening monitor reads as occupied.
- **Overflow → no placement.** If every Display already carries an Eligible window, Spread
  emits **no** Effect — the window is left Eligible and untouched wherever the OS opened it.
  It is *not* moved to the focused Display; the no-op path writes no geometry at all.

## Considered Options

- **Synthesized `Win`+`Shift`+arrow (stay literally geometry-free).** Rejected: acts on the
  foreground window only and cycles adjacent monitors, so it is racy and requires modelling
  monitor-cycle order to hit a specific Empty Display. Dressing a relocation up as keystrokes
  to dodge this ADR is less honest than writing the ADR.
- **A persisted per-(Display × Workspace) occupancy model** for guaranteed, serialized spread.
  Rejected: it is the exact stored-geometry staleness ADR-0007 fled — a user drag to another
  monitor invalidates it and would force the location-change hooks ADR-0007 deleted. It is
  also the deferred model Reaping waits on; this feature does not justify building it.
- **Fatten every `Appeared` with an occupancy snapshot** instead of a lazy round-trip.
  Rejected: it probes an `EnumWindows` sweep on every window show even when no Spread rule
  exists. The round-trip probes only when a Spread rule actually matched (ADR-0008's laziness).

## Consequences

- **Best-effort spread under bursts, accepted.** With no persisted occupancy, several matching
  apps opening in a burst (e.g. `exec-once` launching three) can each probe before the others
  land and pick the *same* lone Empty Display — two share a Display, one stays empty. Since
  overflow is already a benign no-op, this is accepted; guaranteed spread would require the
  rejected persisted model.
- **The geometry ban of ADR-0007 is partially reversed, by design and bounded.** A future
  reader who finds `PositionWindow` after reading ADR-0007's "no `PositionWindow` Effect" should
  read this ADR. The reversal is one Effect, emitted only by Spread, one-shot, place-once —
  categorically not the continuous auto-tiling ADR-0007 killed.
- **The Reducer stays pure.** The occupancy read is an adapter Probe; picking the target from
  the occupied set is a pure function of `(occupied Displays, subject id)`. Only `PositionWindow`
  is new I/O, executed by the Worker.
- **Spread is not re-asserted on reload** — consistent with Place-once and the Ignore-set: a
  Spread rule added at reload takes effect for a matching window only when it next `Appears`.
