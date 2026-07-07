# Directional focus resolution: band-first, center-ahead, nearest-center

A `focus left|right|up|down` press resolves to a single target window through a
pure rule in the Reducer over rects Probed live at keypress (stateless, per
[ADR-0007](0007-drop-tiling-no-window-geometry.md)). Given the Origin rect (the
current foreground window) and the Eligible Candidates in virtual-screen
coordinates, the target is the `argmin` over Candidates of the lexicographic tuple:

1. **forward** — keep only Candidates whose *center* is strictly ahead of the
   Origin's center on the Direction's axis (`right`: `c.cx > o.cx`; `down`:
   `c.cy > o.cy`; Win32 y grows down, so `down` is +y and `up` is −y). Nothing
   forward → no target (a no-op).
2. **in-band** (`0` if the Candidate's cross-axis range overlaps the Origin's,
   else `1`) — a window in the same row/column band beats a diagonal one.
3. **Euclidean center-to-center distance** — nearest wins.
4. **`WindowId`** — deterministic final tie-break so the result never depends on
   enumeration order (this is what the reducer tests pin).

Monitor-crossing needs no special case: everything is one virtual-screen
coordinate space, so a window on the next display is just a farther rect.

## Considered Options

- **Pure nearest-center** (drop the in-band tier — just `forward` then min
  distance). Rejected: it jumps past a window directly beside the Origin to grab a
  slightly-closer diagonal one, which reads as wrong. The in-band tier costs one
  boolean and matches intuition.
- **Edge-ahead forward test** (`right`: `c.left >= o.right`). Rejected: it drops
  Candidates that *overlap* the Origin but sit mostly ahead — common with
  overlapping windows. Center-ahead keeps them reachable. Known simplification:
  a small window fully *contained* within a much larger Origin's span (its center
  not past the Origin's center) is unreachable — accepted as a rare case.
- **A single cross-penalized scalar score** (`primaryGap + K·crossOffset`).
  Rejected: the weight `K` is arbitrary and produces surprising results at
  boundaries; the in-band boolean is a hard, explainable pre-sort instead.

- **How the two phases connect.** An Effect cannot hand data back to the pure
  Reducer, so the Probed rects must re-enter as a second Event. We chose to let
  the Reducer *orchestrate* it: `reduce(FocusMove{dir})` emits a `ResolveFocus{dir}`
  Effect; the Worker executes it by running the Probe sweep and posting a
  `FocusResolve{dir, candidates, origin}` Event back to itself (reusing the
  existing `Event*` transport); `reduce(FocusResolve)` then emits
  `SetForegroundWindow`. Rejected alternative: the Worker probes *inline* on
  `FocusMove` and calls `reduce` with the enriched event directly — fewer types,
  but it teaches the adapter *when* to probe, leaking policy out of the Reducer.
  Keeping the Worker a mechanical executor (as it already is for
  `SwitchToWorkspace`) was worth one extra Effect type.

## Consequences

- The whole rule is pure and reducer-tested over synthetic multi-window /
  multi-monitor rect sets — no live desktop needed. Cross-monitor resolution is
  covered here, not in the VM smoke seam (the VM is single-display).
- The Origin's Eligibility is never checked — it is only a reference rect and is
  excluded from its own Candidate set by `WindowId`. No foreground Origin → no-op.
- `WindowAttrs` gains a `rect`, Probed together with the Eligibility facts in one
  read per window; the Reducer applies `isEligible` (never the adapter, per
  [ADR-0006](0006-window-tracking-probe-decide-seam.md)) before resolving.
