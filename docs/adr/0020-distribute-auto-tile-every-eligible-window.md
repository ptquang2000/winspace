# Distribute: auto-place every eligible window, place-once, explicit rebalance

**Status:** Accepted (2026-07-19). **Supersedes [ADR-0016](0016-bounded-window-geometry-rule-targeted-place-once.md)**
on its two core promises — the *tile-allowlist* ("only rule-matched windows are touched")
and the *fixed per-rule target* — reversing both. **Retires
[ADR-0021](0021-spread-bounded-geometry-write-to-empty-display.md)** (the Spread action, the
`SpreadWindow` Effect, the `spread` token, and the "overflow → no placement" behavior).
**Refines [ADR-0007](0007-drop-tiling-no-window-geometry.md)**: the computed-set layout ADR-0007
banned is now adopted, but bounded to place-once + an explicit `tile` — so ADR-0007's ban on
*continuous* tracking still holds. Every other reason ADR-0007 and ADR-0016 gave still stands.

## Context

ADR-0016 shipped a **tile-allowlist**: winspace owned geometry *only* for windows a rule
explicitly targeted (a Slot-bearing Place rule, or ADR-0021's `spread` onto an Empty Display).
Everything else stayed wherever the OS opened it — the deliberate "never surprise the user with
a window they didn't ask winspace to manage" property.

The user then asked for the **opposite default**:

> *For **any** new eligible window, spread it out first, then position it, and keep windows on
> all displays evenly — winspace should be an auto-tiler that manages every eligible window, not
> just an allowlisted few.*

This was grilled at length before this ADR. It is a deliberate reversal of ADR-0016's core
promise, not a silent extension — hence a supersession ADR rather than an edit to 0016. The
settled shape:

- **Manage by default, opt out by rule.** Eligible = every manageable window (the `isEligible`
  gate), not the ADR-0016 rule-allowlist. Placement no longer requires a rule.
- **"Spread first, then position" = pick the Display, then apply a fixed default Slot
  (`maximized`).** The Display choice is the real work; the position step is dumb.
- **"Evenly" = per-Display window *count*, not equal screen area.** Two windows sharing a
  Display both maximize and overlap; counts are even, pixels are not. Pixel-even sharing would
  require resizing the window already there — which place-once forbids.

## Decision

- **Distribute is the no-rule path on `Appeared`.** For each Eligible window, exactly once in
  its lifetime (place-once, ADR-0009's `placed` set, unchanged):
  1. **Matches an `Ignore` rule** → left entirely alone. `Ignore` widens to **"don't touch at
     all"**: not a focus target (ADR-0009) and now also **never placed**.
  2. **Matches a `Place` rule** → pinned to its Workspace, and its Slot if the rule carries one
     (ADR-0016). A rule match is treated as explicit user intent and **opts the window out of
     Distribute** — *any* match, not only a Slot rule.
  3. **Matches no rule** → **Distribute**: move it to the **least-occupied Display** (fewest
     Eligible windows on the current Workspace) and **maximize** it. Never resized again.

- **One unified geometry Effect.** ADR-0016's `PositionWindow{ WindowId, Slot }` and ADR-0021's
  `SpreadWindow{ WindowId, MonitorId }` collapse into a single
  **`PositionWindow{ WindowId id, std::optional<MonitorId> target, Slot slot }`**. `target ==
  nullopt` means the window's current monitor (a Slot rule on its own Display, ADR-0016
  behavior, and the Distribute tie-break "keep it put"); a set target is Distribute's chosen
  Display. `SpreadWindow` and the `spread` config token are deleted; a config still carrying
  `spread` earns a targeted diagnostic naming that distribution is now automatic.

- **Occupancy carries a count, not a bool.** `DisplayOccupancy{ MonitorId id; int count; }`
  (was `bool occupied`), so "least-occupied" balances when windows outnumber displays.

- **The decision is a pure, unit-tested function.**
  `pickDistributeTarget(counts, current) -> std::optional<MonitorId>` returns the least-occupied
  Display, **preferring `current` when it ties** (nullopt → keep it put, no pointless
  cross-monitor jump); otherwise the **first** least-occupied Display in enumeration order
  (deterministic tie-break). Tested directly like `rectForSlot`, no Win32.

- **Two postures, deliberately split:**
  - **`Appeared` is place-once.** Distribute never moves an already-placed window; it only
    places the newly-appeared one. Evenness *drifts* as windows come and go — accepted. No
    continuous tracking, no location hooks, no live layout model in State. ADR-0007's core
    survives.
  - **`tile` is the only rebalance.** The `tile` dispatcher re-runs the full pipeline (rules
    win, the rest Distribute) across every Eligible window on the current Workspace,
    re-placeable (it ignores `placed`), so it is the one path that may move already-placed
    windows back into an even distribution. It is explicit and user-invoked — winspace never
    rebalances on its own.

- **The round-trip is renamed to the house `Resolve*` / `*Resolve` convention** (mirroring
  `ResolveFocus`/`FocusResolve`): `ResolveSpread` → **`ResolveDistribute{ WindowId subject }`**;
  `SpreadResolve` → **`DistributeResolve{ WindowId subject, MonitorId subjectMonitor,
  std::vector<DisplayOccupancy> displays }`** (now also carrying the subject's current Display so
  the tie-break can prefer keeping it put). `TileResolve` gains a
  `std::vector<MonitorId> displays` so the reducer can balance the free windows across all
  displays — including empty ones — in one pure pass, without a per-window probe round-trip.

- **`RuleAction` loses `Spread`** (`enum { Place, Ignore }`).

- **No overflow no-op.** Distribute *always* places; when every Display already carries windows
  it still picks the least-occupied and maximizes there (overlap accepted). ADR-0021's
  "overflow → leave untouched" behavior is gone.

- **Workspace untouched by Distribute.** Auto-placed windows stay on whatever Workspace the OS
  opened them; only a `Place` rule moves Workspace. Distribute is Display + geometry only.

## Considered Options

- **Continuous rebalancing / a live layout model that re-layouts on every lifecycle edge.**
  Rejected — the exact stored-geometry / location-hook machinery ADR-0007 fled. Evenness drift
  between explicit `tile` presses is the accepted price.
- **Pixel-even sharing of a Display** (resize the existing window so two truly split a screen).
  Rejected: it requires the continuous rebalancing above, and it violates place-once (the
  window already there would be resized by a *later* window's arrival).
- **A configurable `default_slot` Setting / complementary-slot layout solver.** Deferred: the
  default slot is hard-coded `maximized`; add the knob only when a use case demands it.
- **A separate `float` opt-out action.** Deferred: `Ignore` is the single opt-out.
- **A persisted per-Display occupancy model.** Rejected — occupancy is probed reactively, no
  stored geometry to go stale. Consequence: every Eligible `Appeared` now triggers the occupancy
  probe round-trip (the reverse of [ADR-0015](0015-fancyzones-evaluated-coexistence-over-integration.md)'s
  "don't fatten every Appeared" — here accepted as the cost of managing every window).

## Consequences

- **ADR-0016's allowlist is reversed, by design.** A reader who finds Distribute after ADR-0016's
  "only rule-matched windows are touched" should read this ADR: the default flipped from
  *unmanaged unless matched* to *managed unless matched* (where a match is now an opt-*out*).
- **The Reducer stays pure.** The occupancy read is an adapter Probe; `pickDistributeTarget` and
  the `TileResolve` balancing are pure functions of `(counts, current)` and
  `(windows, displays, rules)`. Only `PositionWindow` is I/O, executed by the Worker.
- **Best-effort evenness, accepted.** Place-once means drift accumulates as windows come and go;
  `tile` is the on-demand fix. Guaranteed continuous evenness would require the rejected live
  model.
- **Every `Appeared` now probes occupancy.** Unlike ADR-0016 (probe only on a rule match), the
  no-rule path is the *common* case, so the `EnumWindows` + `EnumDisplayMonitors` sweep runs on
  essentially every window show — the deliberate cost of managing every window.
- **The geometry ban of ADR-0007 is now broadly (but still boundedly) reversed.** winspace
  writes geometry for most windows, but only place-once and on explicit `tile` — categorically
  still not the continuous auto-tiling ADR-0007 killed.
