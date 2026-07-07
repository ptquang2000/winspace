# PRD 0005 — Spatial directional focus (`focus left/right/up/down`)

**Labels:** `ready-for-agent`
**References:** [ADR-0008](../adr/0008-directional-focus-resolution.md),
[ADR-0007](../adr/0007-drop-tiling-no-window-geometry.md),
[ADR-0006](../adr/0006-window-tracking-probe-decide-seam.md),
[ADR-0005](../adr/0005-vm-seam-test-harness.md), `CONTEXT.md` (Windows & focus),
`DESIGN.md`, `issues/05-spatial-focus.md`

## Problem Statement

winspace can switch Workspaces from a hotkey, but the only way to change which window has the
keyboard is the mouse or Alt-Tab's most-recently-used list. Neither is *spatial*: the user
looks at a window sitting to the right of the current one and has no keystroke that means "put
the keyboard over there." On a multi-monitor setup this is worse — moving focus to the next
display is a reach for the mouse every time. The user wants to steer keyboard focus by
direction, the way they already steer it in a tiling WM, without winspace ever moving or
resizing anything.

## Solution

A `focus left|right|up|down` Dispatcher moves keyboard focus to the spatially nearest Eligible
window in the requested Direction. On the keypress winspace Probes every top-level window's
live rectangle plus the current foreground window (the Origin), and the pure Reducer picks the
nearest Candidate in that Direction in virtual-screen coordinates — so traversal crosses
monitor boundaries naturally — then emits a single `SetForegroundWindow` Effect. The
resolution is **stateless** (ADR-0007): rects are read fresh every keypress, nothing is
persisted, and winspace never moves or sizes a window. Tool windows, dialogs, cloaked UWP
hosts, and fullscreen apps are skipped by the Eligibility gate; a request with no Eligible
window in that Direction does nothing.

## User Stories

1. As a keyboard-driven desktop user, I want to press a hotkey to move focus to the window
   directly to the left of the current one, so that I can switch windows without the mouse.
2. As a keyboard-driven desktop user, I want the same for right, up, and down, so that I can
   steer focus in any direction.
3. As a keyboard-driven desktop user, I want `focus right` to land on the window directly
   beside me rather than a diagonal one that happens to be a hair closer, so that the movement
   matches what I see.
4. As a multi-monitor user, I want `focus right` at the right edge of one display to jump to
   the nearest window on the next display, so that focus traversal spans my whole desk without
   a special "next monitor" key.
5. As a multi-monitor user, I want direction resolution to reason in one virtual-screen
   coordinate space, so that crossing a monitor boundary is not a special case that behaves
   differently from moving within a monitor.
6. As a desktop user, I want dialogs, tool windows, floating palettes, cloaked UWP host
   windows, and fullscreen apps skipped as focus targets, so that a directional press never
   dumps me onto a phantom or transient window.
7. As a desktop user, I want a directional press with no Eligible window in that Direction to
   do nothing, so that focus never jumps somewhere surprising or wraps around.
8. As a desktop user, I want the window I'm already on to never be its own target, so that a
   press always moves focus or does nothing — never a no-visible-change "move to self."
9. As a desktop user whose focus is currently on a tool window or a non-application window, I
   want a directional press to still measure *from* where I am and move to the nearest Eligible
   window, so that focus is recoverable from an odd starting point.
10. As a desktop user with nothing focused (e.g. only the shell desktop is active), I want a
    directional press to do nothing, so that focus movement without a reference point is not
    invented out of thin air.
11. As a desktop user, I want the target chosen from window positions read at the instant I
    press the key, so that a window I just dragged is considered where it is now, never where
    it used to be.
12. As a desktop user, I want directional focus to work regardless of the order Windows happens
    to enumerate my windows, so that repeated presses in a fixed layout are perfectly
    predictable.
13. As a desktop user, I want to bind the direction to whatever key I like — arrow keys,
    `H/J/K/L`, anything — independently of which Direction it means, so that my muscle memory
    from other tools carries over.
14. As a desktop user, I want a clear config error when I write a `focus` bind with a missing
    or misspelled direction, so that a typo is diagnosed on that line and my other binds still
    load.
15. As a battery-conscious laptop user, I want winspace to Probe window rectangles only on a
    `focus` keypress and never on a timer, so that idle winspace costs nothing.
16. As a keyboard-driven user, I want the focus change to actually take effect (the target
    window really comes to the foreground), so that the feature is not a silent no-op defeated
    by Windows' foreground-lock rules.
17. As a winspace maintainer, I want the entire directional-resolution rule to be a pure
    function tested from synthetic multi-window and multi-monitor rect sets, so that I can
    prove target selection without a live desktop.
18. As a winspace maintainer, I want the Eligibility gate applied inside the Reducer (not the
    adapter), so that candidate filtering stays unit-tested and out of the `windows.h` layer.
19. As a winspace maintainer, I want the pure core to stay `windows.h`-free, so that any stray
    OS call reachable from it is a link error, not a runtime surprise.
20. As a winspace maintainer, I want the live foreground-move behavior covered by the VM Smoke
    seam, so that the one thing a unit test cannot reach — the real `SetForegroundWindow`
    landing — is proven end-to-end on a real session.

## Implementation Decisions

Overarching discipline (ADR-0006, ADR-0007): every OS-touching concern splits into a **Probe**
(adapter gathers a window's live attributes into plain data) and a **policy** (the pure
Reducer decides over that plain data). No Effect moves or sizes a window — `focus` reads
geometry, never writes it.

**Reduce orchestrates a two-phase probe round-trip (ADR-0008).** An Effect cannot hand data
back to the pure Reducer, so the Probed rects re-enter as a second Event, and the Reducer — not
the adapter — decides to Probe:

1. The Hotkey thread posts `FocusMove{dir}` (it knows only which key fired).
2. `reduce(FocusMove{dir})` emits a `ResolveFocus{dir}` Effect; State is untouched.
3. The Worker executes `ResolveFocus` by running the Probe sweep — `EnumWindows`, Probe each
   top-level window's attrs+rect, read the foreground Origin — then posts
   `FocusResolve{dir, candidates, origin}` back to itself via the existing `Event*` transport.
4. `reduce(FocusResolve)` filters by `isEligible`, runs the resolution, and emits
   `SetForegroundWindow{id}` (or nothing).
5. The Worker executes `SetForegroundWindow`.

The Worker stays a mechanical executor, as it already is for `SwitchToWorkspace`. The rejected
alternative — the Worker probing *inline* on `FocusMove` and calling `reduce` directly — is one
type cheaper but leaks the "when to probe" decision into the adapter (ADR-0008, Considered
Options).

**Direction is a config semantic type, translated at the hotkey seam.** `config.cpp` gains
`enum class Direction { Left, Right, Up, Down }` (peer of `Mod`/`Key`/`Dispatcher`) and a
`Direction` field on `Bind`; the parser recognizes the `focus` dispatcher and a
`left|right|up|down` argument (case-insensitive, no aliases), diagnosing a missing or unknown
direction per-line exactly as it does an unknown key or workspace number. `reducer.cpp` defines
its own `Direction` for the focus Events; `hotkeys.cpp`'s `toEvent` translates
`config::Direction → reducer::Direction`, mirroring the existing `Dispatcher →` event and
`Mod → MOD_*` translations. The bind's *key* and its *direction* are independent fields
(`bind = SUPER, Left, focus, left` presses Super+Left to focus left, but any key may be bound).

**`WindowAttrs` gains a rect.** The `Rect` already defined in the core is added to
`WindowAttrs`, Probed together with the Eligibility facts in one read per window. The Reducer
reasons over these rects in virtual-screen coordinates (Win32 convention: x grows right, y
grows **down**).

**The resolution rule is pure and lives entirely in the Reducer (ADR-0008).** Filter Candidates
by `isEligible`, exclude the Origin by `WindowId`, keep only those lying **ahead** in the
Direction (center strictly past the Origin's center on that axis), then choose the `argmin` of
the lexicographic tuple `(inBand ? 0 : 1, euclideanCenterDistance, windowId)` — same-band
before diagonal, then nearest by center distance, then lowest `WindowId` for determinism.
Nothing ahead, or no Origin, → no Effect. Cross-monitor falls out for free from the shared
virtual-screen space.

**Core vocabulary (the decision-bearing shape):**

```cpp
struct WindowAttrs {                 // plain-data Probe result; Rect = {int l,t,r,b}
    WindowId id{}; MonitorId monitor{}; Rect rect{};
    bool topLevel, visible, thickFrame, caption, toolWindow, cloaked, fullscreen;
};
enum class Direction { Left, Right, Up, Down };   // reducer-side; config mirrors it

// Events (add to the existing variant)
struct FocusMove    { Direction dir; };                     // from the Hotkey thread
struct FocusResolve { Direction dir;                        // posted by the Worker
                      std::vector<WindowAttrs> candidates;  // full probed set, unfiltered
                      std::optional<WindowAttrs> origin; };  // foreground; nullopt => no-op

// Effects (add to the existing variant)
struct ResolveFocus        { Direction dir; };  // Worker: probe sweep, post FocusResolve
struct SetForegroundWindow { WindowId id; };    // Worker: bring the target foreground
```

**`SetForegroundWindow` execution: bare call, degrade-and-log.** The Worker converts
`WindowId → HWND` via a named reverse-mint helper and calls `SetForegroundWindow`. A just-fired
`RegisterHotKey` often counts as winspace receiving the last input event, satisfying one of
Win32's foreground-lock conditions, so the bare call frequently succeeds. On failure it
degrades and logs — never crashes (null-bridge precedent). The scoped `AttachThreadInput`
recovery is **deliberately deferred** until the VM Smoke seam proves the bare call insufficient
(own only what you must). Input-synthesis or global-foreground-lock-timeout hacks are rejected
outright — they contradict winspace's OS-integrated, scheduler-respecting philosophy.

**No State.** Spatial focus persists nothing; `State` gains no members. The whole feature is a
stateless read → decide → set-foreground.

## Testing Decisions

A good test exercises **external behavior, not implementation detail**: it feeds Events and
asserts on the emitted Effects, checking State only where an Effect already reveals it. This is
the house style in `reducer_test.cpp` and the `issues/README` seam contract.

**Seam (1) — the pure Reducer — carries the whole brain and is unit-tested:**
- Phase one: `FocusMove{dir}` emits exactly `ResolveFocus{dir}` and leaves State untouched.
- Simple resolution: with a neighbor in the Direction, `FocusResolve` emits
  `SetForegroundWindow{that id}`; the opposite Direction with nothing there emits no Effect.
- Band-first: a diagonal Candidate closer by center vs. an aligned Candidate slightly farther —
  the aligned (in-band) one is chosen.
- Center-ahead: a Candidate overlapping the Origin but center-forward is still reachable.
- Eligibility filtering: the geometrically nearest window is Ineligible → skipped, next
  Eligible chosen (the gate is exercised through the focus path, not only in isolation).
- Origin excluded: the Origin is never its own target.
- No forward Candidate → no Effect.
- No Origin (`nullopt`) → no Effect.
- Cross-monitor: the nearest right Candidate is a far-right rect (second display) in
  virtual-screen coordinates → chosen.
- Tie-break: two Candidates identical in band and distance → the lower `WindowId` wins.
- Y-down convention: `focus down` picks the +y Candidate, `focus up` the −y.

**Seam (2) — the config parser:** a `focus` bind parses to `Bind{Focus, dir}`; a missing
direction is diagnosed; an unknown direction is diagnosed; direction parsing is
case-insensitive; valid binds on other lines still load.

Prior art for both: `src/winspace/reducer_test.cpp` (`single_effect` helper, effect-equality
asserts) and `src/winspace/config_test.cpp` (bind + diagnostic assertions).

**Secondary — the existing VM Smoke seam (ADR-0005)** — one new Smoke seam for the sole
live-only behavior Seam (1) structurally cannot reach: on a real `focus` keypress Triggered by
`SendInput`, the foreground window actually changes to the spatial neighbor (Oracle:
`GetForegroundWindow` before/after). No new seam is introduced. The VM is single-display, so
this covers same-screen movement only; cross-monitor resolution stays reducer-tested.

## Out of Scope

- **`AttachThreadInput` foreground-lock recovery** — added only if the Smoke seam proves the
  bare `SetForegroundWindow` insufficient.
- **Cross-monitor *live* coverage** — the VM is single-display; monitor-crossing is pure
  arithmetic covered by Seam (1).
- **Direction aliases** (`h/j/k/l` as direction words) — users bind whatever *key* they like
  to the four canonical directions instead.
- **Any window movement** — `focus` never moves or sizes a window; the old `movewindow`
  tile-swap died with tiling (ADR-0007).
- **Focus-order / MRU history, wrap-around, or "focus next/prev"** — resolution is purely
  spatial and stateless.
- **The `SetWinEventHook` adapter and the `Appeared`/`Vanished` stream** — reintroduced by
  PRD 06, not needed here (the sweep is a one-shot `EnumWindows` at keypress).

## Further Notes

This slice reuses the substrate PRD 0002 left on master — the `WindowId`/`MonitorId`
identities, the `isEligible` gate, and the reactive Probe — and adds only a rect field, four
plain-data Event/Effect types, and one pure resolution function. It introduces no new thread,
no new seam, and no State. The only genuine risk is the Win32 foreground-lock behavior of
`SetForegroundWindow`, which is why the live path gets a Smoke seam despite the logic being
fully reducer-tested.
