# Window rules reintroduce bounded per-window state (place-once)

**Status:** Accepted (2026-07-09)

[ADR-0007](0007-drop-tiling-no-window-geometry.md) stripped the continuous
window-tracking machinery and declared the window side of `State` **stateless** —
Spatial focus resolves from rects Probed live at keypress and persists nothing.
PRD 06's `windowrule` app→workspace pinning cannot be fully stateless: it must
place a matching window **exactly once in its lifetime** and never yank back a
window the user later moved. This ADR records the smallest reintroduction of
window state that buys that guarantee, and why it does not resurrect tiling's
tracking.

## Context

Rules match at the **`Appeared`** edge (`EVENT_OBJECT_SHOW` / `EVENT_OBJECT_UNCLOAKED`),
not raw `EVENT_OBJECT_CREATE` — the same edge [ADR-0006](0006-window-tracking-probe-decide-seam.md)
chose, because at CREATE the window is half-born (styles/visibility not final,
title usually empty, UWP host still cloaked) and title-regex matching would fail.
The issue text and `DESIGN.md §5` said "CREATE"; they were wrong and are corrected
to SHOW/UNCLOAKED.

The catch of that edge: `UNCLOAKED` fires *every time* a window returns from
another Virtual Desktop. A memoryless matcher would therefore re-pin a window each
time it came back — exactly the "yank back a window the user moved" behavior
place-once forbids.

## Decision

- `State` gains **`std::unordered_set<WindowId> placed`** and
  **`std::vector<WindowRule> rules`** (seeded once from config; replaced on reload,
  PRD 09). Nothing else about windows is stored — no rects, no monitor model, no
  focus order. The geometry ban of ADR-0007 stands untouched.
- On `Appeared`: if `id ∈ placed`, emit nothing. Otherwise run the matcher (gated
  on `isEligible`) and, **whether or not a rule matched**, insert `id` into
  `placed`. A non-matching window is thus never re-evaluated on later uncloaks
  either.
- On `Vanished` (`DESTROY` / `HIDE` / `CLOAKED`): erase `id` from `placed`. This
  bounds the set to live windows and — because Windows recycles `HWND`s — stops a
  reused handle from being wrongly treated as already-placed.
- Rules **place on startup adoption**: the hook adapter's `EnumWindows` sweep posts
  synthetic `Appeared`s through the same path, so already-open matching apps are
  pinned exactly as newly-opened ones (consistent with Virtual Desktop Adoption's
  "inherit the session").

## Considered Options

- **Match at raw CREATE (once per window, no `placed` set needed).** Rejected:
  the window is half-born, so exe/class are racy and the title is almost always
  empty — title-regex rules would silently never fire. ADR-0006 already rejected
  CREATE for the same reason.
- **Track only windows a rule actually placed** (smaller set). Rejected: it
  re-runs the full regex matcher on every uncloak of every *unmatched* window for
  the life of the session. Inserting unconditionally is cheaper and the set is the
  same order of magnitude.
- **Continuous enforcement (re-assert the rule if the user moves the window).**
  Rejected: not place-once; it would fight the user and require the location-change
  hooks ADR-0007 deliberately removed.

## Consequences

- The stateless-window-side property of ADR-0007 is **partially reversed, by
  design** — but bounded to a set of opaque ids and a rule list, both rebuilt on
  start, never persisted. A future reader who finds `State.placed` after reading
  ADR-0007 should read this ADR.
- Place-once is a lifetime property keyed on `WindowId`; close-and-reopen is a new
  lifetime and re-pins, which is the intended behavior.
- The matcher stays a pure function of `(rules, WindowIdentity, isEligible)` in the
  Reducer; only the `placed` bookkeeping is new control flow, still pure.
