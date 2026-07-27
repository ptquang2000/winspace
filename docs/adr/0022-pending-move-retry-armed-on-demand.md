# Pending move: a bounded, armed-on-demand retry — and where the no-polling boundary sits

A `windowrule = workspace N` move **intermittently does nothing** for a freshly launched app.
The Worker's `moveWindowToWorkspace` bails at `src\win32.cpp:1519` with

```
[ERROR] move window to workspace 2: GetViewForHwnd: (hr=0x8002802B): Element not found.
```

because the shell has not finished **View registration** for the brand-new HWND at the moment
winspace handles `Appeared`. Nothing retries: the `Appeared` handler inserted the id into
`placed` before emitting the Effect (place-once, [ADR-0009](0009-window-rules-place-once-state.md)),
so every later edge is ignored. This ADR records the fix — a **Pending move** set retried on a
timer that is **armed only while a retry is outstanding** — and, because a timer in this project
is surprising, restates exactly where [ADR-0007](0007-drop-tiling-no-window-geometry.md)'s
no-polling promise begins and ends.

## Context

**The failure is a third party's bookkeeping, not a broken window.** Per
[ADR-0010](0010-move-to-workspace-internal-move.md) (revised), the public
`IVirtualDesktopManager::MoveWindowToDesktop` returns `E_ACCESSDENIED` for a window winspace does
not own — which is every window it manages — so the move goes through the internal
`IApplicationViewCollection::GetViewForHwnd` → `IVirtualDesktopManagerInternal::MoveViewToDesktop`
pair. `GetViewForHwnd` is a **lookup in a shell-owned registry**, and the entry does not exist
yet. The window itself is fully created and visible.

**Measured lifecycle** (throwaway probe `prototype/event-order`, hooks `0x0001`–`0x80FF`, sampling
`GetViewForHwnd` at every event; 4 runs, pwsh on win11-24h2):

```
t+0ms       OBJECT_CREATE      view=no    vis=0
t+0..16ms   SYSTEM_FOREGROUND  view=no    vis=0    <- refutes FOREGROUND as a "ready" signal
t+31..63ms  OBJECT_SHOW        view=YES   vis=1    <- the flip lands in the CREATE->SHOW gap
t+~same     0x4005 (undocumented)         view=YES
```

Three facts follow, and they close off the alternatives:

- **`EVENT_OBJECT_SHOW` is already the best available trigger.** Registration normally precedes
  it, which is why the bug is intermittent rather than constant. Moving earlier is strictly worse.
- **No documented event marks registration.** The only event that always postdated it besides
  `SHOW` is the undocumented `0x4005`. There is nothing to subscribe to, so *something* must
  re-ask.
- **Rolling place-once back is useless.** Measured: no later edge arrives spontaneously. The one
  edge that does arrive — CLOAKED/UNCLOAKED on a workspace switch — already clears `placed` via
  `Vanished`, so the window self-heals on the next switch-away-and-back with no code change.

**Scope is exactly one Effect.** `PositionWindow` writes geometry with `SetWindowPos` and needs no
view, so Distribute, Slots, and `tile` never hit this. View registration is winspace's **only**
asynchronous shell dependency.

**Why this ADR exists at all.** The fix is ~20 lines and trivially reversible — normally not
ADR-worthy. It earns one on the second criterion: a future reader finds a `SetTimer` polling loop
and checks it against `README.md`'s "no window geometry polling, no background CPU" and
`CONTEXT.md`'s **Probe** entry (*Avoid*: "Poll, scan, snapshot-loop — there is no interval"), and
reads it as a violation of the project's central promise. Without this record they rip it out, or
take it as licence for the next timer. The boundary needs writing down.

## Decision

- **A `Pending move` set, owned by the Worker's I/O layer.** On `moveWindowToWorkspace` failing
  with `0x8002802B`, record `{WindowId, target logical, deadline}` and `SetTimer(25ms)` if the set
  was empty. On `WM_TIMER`, re-attempt each entry. `KillTimer` when it drains. **Idle cost is
  exactly zero wakeups** — the timer does not exist when nothing is pending, which is ~always.
- **Three exits, no others.**

  | Exit | Action |
  |---|---|
  | Success | erase |
  | `Vanished` | erase, never retry |
  | Deadline (250ms) | erase, `warn` naming the exe |

  A re-attempt that fails with anything *other* than `0x8002802B` leaves by the
  Success row: the bridge has already logged it (ADR-0004) and re-asking cannot fix
  it, so the entry is finished. Only the one transient keeps an entry alive.

- **`Vanished` cancellation is mandatory, not a nicety.** Windows recycles HWND values. A pending
  entry that outlives its window can resolve a *recycled* handle and move an unrelated window to
  the target Workspace — trading an intermittent no-op for an intermittent wrong action, which is
  strictly worse. The Worker's Effect executor therefore observes `Vanished` alongside the
  Reducer; this is the one place the mechanism is not wholly invisible to the core.
- **Nothing crosses into the Reducer.** No new Event, no new State, no `Effect` variant. `placed`
  was already set before the Effect ran, so State's belief ("this window was placed") becomes true
  behind its back when the retry lands. The transient dishonesty is accepted — see Consequences.
- **Never a blocking loop.** `Sleep(25)` × 10 inside the Effect executor would stall the Worker for
  250ms, and the Worker services hotkeys and every other Event. A timer is not a style choice here.
- **The budget is 250ms at 25ms polls**, ~5–10× the measured gap. It is **inferred from gap width,
  not from an observed failure** — the natural race was never caught in the instrumented build
  (0 failures in 6 runs). The give-up `warn` is the tripwire that would tell us otherwise.
- **`WINSPACE_DIAG_DROP_FIRST_MOVE` becomes a permanent env-gated test seam** — it force-fails the
  first `moveWindowToWorkspace` per HWND. Without it the race is unforceable and a VM seam would
  pass whether or not the retry exists (ADR-0005 makes a live behavior's definition of done a
  seam; a seam that asserts nothing is not one).

**Where the no-polling boundary sits.** ADR-0007 banned waking up to check whether *winspace's own
world* had drifted — window geometry, layout, focus order. That ban is untouched: no timer reads a
rect, recomputes a layout, or re-asserts placement. This timer waits on a **third party's
asynchronous bookkeeping**, for which no notification exists, and it is **armed on demand and
disarmed on completion**. `README.md`'s promise survives literally — nothing polls geometry, and
background CPU when idle stays zero — and needs no edit.

## Considered Options

- **A frame-paced internal event loop on the main thread** — repurpose the thread blocked in
  `workerThread.join()` (`src\win32.cpp:2720`) into a pump ticking at the monitor refresh rate,
  with the retry as its first tenant. **Rejected on every one of its three justifications.**
  *(a) "Utilize the idle thread":* a thread parked in `join()` costs zero CPU and zero wakeups —
  it is the cheapest possible shutdown barrier, and pumping it converts something free into a
  recurring cost (60–144 wakeups/second, forever, for a set that is empty ~always). *(b)
  "Synchronize all events":* every Event already funnels through one `GetMessage` loop on the
  Worker (`src\win32.cpp:2518`), so total order and atomic State transitions already hold. A frame
  clock adds no ordering — it adds batching, plus up to a frame of latency on **every** Event
  including hotkey-driven ones, against the "stays off the input critical path" claim that opens
  `CONTEXT.md` (16.7ms at 60Hz; 33ms on a 30Hz panel). *(c) "Custom per-frame ordering":* a real
  capability the strict fold does not have, but a frame cut is an arbitrary batch boundary — two
  Events 2ms apart are reordered or not depending on where the tick lands, making behavior depend
  on sub-frame timing luck and defeating seam testing. If batching is ever wanted, **drain-to-empty**
  (`PeekMessage` until dry) is the causally meaningful boundary, adds no latency, and costs nothing
  when idle. Additionally: the retry's 25ms has no relationship to vsync, so frame pacing is a
  *coincidentally similar* interval, not a derived one — and it would scale wrongly with the panel
  (6.9ms at 144Hz, 33ms at 30Hz), leaving multi-monitor, VRR, and display-sleep as open questions
  the retry does not otherwise have.
- **Reduce the failure** — post a `MoveFailed{id}` Event, hold `pending` in State, emit a
  `RetryMove` Effect on a tick. Rejected: "the shell has not registered a view yet, ask again" is
  not a *decision* — there is no policy and no alternative outcome for the Reducer to reason
  about. It is ADR-0004's degrade-and-log category. B would add an Event type, a State field, and
  a tick concept to the pure core to express "call the API again in a bit", for a single call
  site. Its one genuine advantage — State would stop briefly asserting an untrue `placed` — is not
  worth that surface.
- **Roll back place-once on failure** and let a later edge re-place. Rejected as **measured
  useless**: no later edge arrives spontaneously (above).
- **Subscribe to `IVirtualDesktopNotification::ViewVirtualDesktopChanged`** — undocumented, same
  COM family winspace already consumes (`src\win32.cpp:1334`, `:1370`), and the one plausible "the
  view now exists" signal. **Never tested.** If it fires on *initial* registration it would make
  the retry unnecessary; if it only fires on later moves it is useless here. Not chosen because it
  is unmeasured, not because it is wrong — it remains the strictly better mechanism if it works.
- **Fire on the undocumented `0x4005` event** instead of `SHOW`. Rejected: absent from the public
  constant list, no semantics we can rely on across builds, and it buys only what a 25ms poll buys.

## Consequences

- **`State.placed` briefly asserts something untrue.** Between the failed Effect and a successful
  retry, State says the window was placed when it has not moved. Harmless — `placed` was going to
  be true either way, and no reader distinguishes "placed" from "placement in flight." A reader
  who needs that distinction should revisit the rejected option B rather than add a flag here.
- **The I/O layer now observes `Vanished`**, not just the Reducer. A small, deliberate leak in
  "invisible to the core", accepted as the price of HWND-reuse safety.
- **winspace has one timer.** The next one must justify itself against this ADR's boundary:
  waiting on an external party's async state with no notification available, armed on demand,
  zero idle cost. Anything that wakes to inspect winspace's own world is still ADR-0007's ban.
- **A drag-throttle for future tiling would reuse this shape, not the rejected loop.** If
  continuous tiling is ever built, the one part wanting display-rate pacing is collapsing the
  `EVENT_OBJECT_LOCATIONCHANGE` flood during a drag — armed on the first edge, disarmed when the
  drag ends. Same armed-on-demand shape as Pending move. Continuous tiling itself reverses ADR-0007
  and is its own ADR; it does not justify a standing pump today.
- **Fault injection ships in the release binary.** `WINSPACE_DIAG_DROP_FIRST_MOVE` is inert unless
  set and can only make winspace do *less*. The alternative — deleting it and shipping an
  untestable retry for a bug invisible 5 times out of 6 — is worse.
- **The budget is unvalidated against a real slow starter.** 250ms has never met Edge, Teams, or
  VMware. The probe on `prototype/event-order` is the instrument if the `warn` ever fires.
