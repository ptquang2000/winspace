# PRD 0022 — Pending move: a bounded, armed-on-demand retry for the View registration race

**Status:** ready to implement. Numbered to match
[ADR-0022](../adr/0022-pending-move-retry-armed-on-demand.md), which records the decision and the
rejected alternatives; this PRD records what to build. Vocabulary — **View registration**,
**Pending move**, **Place-once**, **Vanished**, **Smoke seam**, **Oracle** — is defined in
[`CONTEXT.md`](../../CONTEXT.md) and used as-is throughout.

## Problem Statement

A user writes `windowrule = workspace 2, exe:pwsh.exe`, launches pwsh, and **sometimes** the window
stays on the workspace it opened on. Launch it again and it works. There is no pattern the user can
see, nothing they did differently, and no message — the failure is silent unless they go looking in
`run.log`, where they'd find:

```
[ERROR] move window to workspace 2: GetViewForHwnd: (hr=0x8002802B): Element not found.
```

Observed at **1 in 4** manual launches, and expected to be worse for slow starters (Edge, Teams,
VMware) that do more work between window creation and the shell catching up.

The window is not broken and neither is the rule. The shell simply has not finished **View
registration** for the brand-new HWND at the moment winspace handles `Appeared`, so the internal
move path has nothing to move. winspace then never tries again: **Place-once** inserted the id into
`placed` before the Effect ran, so every later edge is ignored. The window self-heals only if the
user happens to switch away from that Workspace and back — which clears `placed` via `Vanished` and
re-runs the rule. Most users will never discover that, and will conclude the rules are flaky.

## Solution

winspace re-asks the shell for a few tens of milliseconds instead of giving up on the first miss.

When the move fails because the view does not exist yet, the Worker records the attempt in a
**Pending move** set and re-attempts every 25ms until the shell catches up — normally on the first
or second attempt, since **View registration** is measured to land tens of ms after window
creation. After 250ms it gives up and says so in the log, naming the app.

From the user's side the feature simply becomes reliable: the rule fires every time, with no visible
delay (25ms is well under the threshold at which a window move reads as anything but instant), and
no new configuration to learn. From the machine's side nothing changes when there is nothing to
retry: the timer is **armed on demand and disarmed the moment the set drains**, so an idle winspace
still does exactly zero background work — the promise `README.md` makes and ADR-0007 established.

## User Stories

1. As a winspace user with a `workspace N` rule, I want my app to land on its target Workspace on
   **every** launch, so that I can trust my config instead of relaunching and hoping.
2. As a user of a slow-starting app (Edge, Teams, VMware), I want the rule to survive the app's
   longer startup, so that the heaviest apps are not the least reliable ones.
3. As a user, I want the retry to be invisible when it succeeds, so that a working feature does not
   spam my log with recovered failures.
4. As a user whose app genuinely never registers, I want **one** clear warning naming the app, so
   that a real failure is findable rather than buried in per-attempt noise.
5. As a user, I want the window to appear in its target Workspace fast enough that I never perceive
   a two-step move, so that the fix does not trade a silent bug for a visible stutter.
6. As a user, I want a window I close during the retry window to stay closed and move nothing, so
   that a late retry never yanks some other window onto a Workspace I did not ask for.
7. As a user, I want my hotkeys to stay responsive while a retry is outstanding, so that a pending
   move never freezes workspace switching or focus.
8. As a user on battery, I want winspace to keep waking zero times per second when idle, so that a
   reliability fix does not cost me battery life.
9. As a user, I want `Distribute`, `Slot` placement, and `tile` to be unaffected, so that the fix is
   contained to the one path that was broken.
10. As a user who drags a window elsewhere after it was placed, I want **Place-once** to still hold,
    so that the retry does not turn into continuous enforcement.
11. As a user, I want a rule that legitimately does not match to stay a no-op, so that the retry
    never invents a placement that no rule asked for.
12. As a user launching several apps at once via `exec`, I want each pending move resolved
    independently, so that one slow app does not delay or cancel another's placement.
13. As a user who quits winspace while a move is pending, I want a clean exit, so that shutdown is
    never blocked or delayed by an outstanding retry.
14. As a user who reloads config while a move is pending, I want the in-flight move to complete
    against the target it was issued for, so that reload does not silently redirect a window.
15. As a maintainer, I want the retry policy testable without a VM, so that its edge cases are
    pinned by fast tests rather than by a slow, flaky live run.
16. As a maintainer, I want the race forceable on demand, so that the live seam actually proves the
    retry exists instead of passing because the race did not happen.
17. As a maintainer, I want the give-up path to emit a distinguishable log line, so that a too-tight
    budget shows up as evidence rather than as a returning mystery.
18. As a maintainer, I want the pure Reducer left untouched, so that the core stays free of I/O
    concerns that are not domain decisions.
19. As a maintainer reading `win32.cpp` in a year, I want the one timer in the codebase to point at
    the ADR that authorizes it, so that it is not mistaken for a violation of the no-polling promise
    or taken as licence for the next timer.
20. As a maintainer, I want the mechanism scoped to the single Effect that needs it, so that it does
    not grow into a general retry framework nobody asked for.
21. As a maintainer, I want `Vanished` cancellation to be non-optional, so that HWND reuse cannot
    turn an intermittent no-op into an intermittent wrong action.
22. As a maintainer, I want the retry budget expressed as named constants, so that widening it after
    a real Edge/Teams measurement is a one-line change.

## Implementation Decisions

### Where it lives

**The I/O layer, on the Worker thread.** No new Event, no new Effect variant, no new field in
`State`. The Reducer is not involved: "the shell has not registered a view yet, ask again" is not a
domain decision — there is no policy and no alternative outcome for the Reducer to reason about. It
is [ADR-0004](../adr/0004-win32-error-handling.md)'s degrade-and-log category. The rejected
alternative (a `MoveFailed` Event, `pending` in `State`, a `RetryMove` Effect) is recorded in
ADR-0022's Considered Options.

### The split: pure policy, adapter mechanism

Mirrors the existing shape in this codebase — `isEligible` and `pickDistributeTarget` are pure and
the adapter gathers the facts.

- **Pure (core, `winspace.cpp` side).** A small value type owning the set of outstanding attempts
  and all of its arithmetic. It has no notion of Win32, COM, or timers; time enters as an injected
  tick value. It answers, for a given moment: which entries are due, which have expired, and whether
  a timer should currently be armed.
- **Adapter (`win32.cpp` side).** Owns the `SetTimer`/`KillTimer` calls, the `GetViewForHwnd`
  re-attempt, and reading the clock. It asks the pure type what to do and does it.

Arming is expressed as a **derived property of the set** — "non-empty ⇒ armed" — not as a pair of
imperative arm/disarm commands. That way the adapter cannot drift out of sync with the policy, and
the "zero wakeups when idle" guarantee is a consequence of the data rather than of remembering to
call `KillTimer`.

### The entry and its three exits

An entry records the `WindowId`, the **target logical workspace** captured at issue time, and a
deadline. Capturing the target at issue time (rather than re-deriving it) is what makes user story
14 hold across a config reload.

| Exit | Trigger | Behavior |
|---|---|---|
| Success | the re-attempt lands | erase; no log |
| Cancelled | `Vanished` for that `WindowId` | erase; never re-attempt |
| Expired | deadline passed | erase; **one** `warn` naming the exe |

**Only `0x8002802B` (`HRESULT_FROM_WIN32(ERROR_NOT_FOUND)`) from `GetViewForHwnd` enters the set.**
Any other failure — a null view collection, a `resolveMoveTarget` miss, a `MoveViewToDesktop`
failure — is a real error and keeps today's degrade-and-log behavior. The retry is for one known
transient, not a blanket wrapper.

### `Vanished` cancellation

Windows recycles HWND values. An entry that outlives its window can resolve a **recycled** handle
and move an unrelated window to the target Workspace — trading an intermittent no-op for an
intermittent wrong action, which is strictly worse. Cancellation is therefore mandatory.

This requires the Worker's Effect-executing side to **observe `Vanished`**, not just hand it to the
Reducer. It is the one place the mechanism is not wholly invisible to the core, and it is a
deliberate, documented leak rather than an oversight.

### Timing

- **Poll interval 25ms, budget 250ms** (≈10 attempts), both named constants. Derived from the
  measured 31–63ms `CREATE`→`SHOW` gap in which registration lands, giving ~5–10× headroom.
  Expected to succeed on the **first or second** attempt.
- **Never a blocking loop.** `Sleep(25)` inside the Effect executor would stall the Worker for up to
  250ms, and the Worker services hotkeys and every other Event. The timer is a correctness
  requirement (user story 7), not a style preference.
- The Worker's existing message loop already pumps, so `WM_TIMER` needs no new thread and no change
  to the threading model. The main thread stays blocked in `workerThread.join()` — see ADR-0022 for
  why the frame-loop alternative was rejected.
- `WM_TIMER` is low-priority and coalescing: the OS may deliver later than 25ms and will not queue
  up a backlog. Both are fine here — the budget is a wall-clock deadline checked against the tick
  value, not an attempt count, so a slow tick spends fewer attempts rather than overrunning.

### The test seam in the product

`WINSPACE_DIAG_DROP_FIRST_MOVE` — an environment-gated diagnostic that force-fails the **first**
`moveWindowToWorkspace` per `WindowId` with the same `0x8002802B`, then lets subsequent attempts
proceed normally. Inert unless the variable is set; it can only make winspace do *less*. It ships in
Release because without it the race is unforceable and the live seam would pass whether or not the
retry exists (see Testing Decisions).

### Non-changes worth stating

- `README.md` needs **no edit**. "No window geometry polling, no background CPU" stays literally
  true: nothing polls geometry, and idle wakeups remain zero.
- `State.placed` keeps asserting a placement that has not landed yet, for the duration of the retry.
  Accepted — it was going to be true either way, and no reader distinguishes "placed" from
  "placement in flight."
- Scope is exactly one Effect. `PositionWindow` uses `SetWindowPos` and needs no view, so
  Distribute, Slots, and `tile` are untouched.

## Testing Decisions

A good test here asserts **external behavior**: for the pure policy, what the set contains and
whether a timer should be armed after a sequence of inputs; for the live seam, where the window
actually ends up according to the OS. Neither should know how the set is stored or when
`SetTimer` is called. Per [ADR-0005](../adr/0005-vm-seam-test-harness.md)'s **Oracle** policy, the
live assertion reads OS state, never winspace's own log.

### Seam A — pure policy tests (`src/winspace_test.cpp`, Catch2)

Prior art: the `isEligible` and `pickDistributeTarget` cases in the same file — pure functions
exercised over their input space with no I/O.

Cases:
1. A `0x8002802B` failure inserts an entry; the set reports it should be armed.
2. An empty set reports it should **not** be armed — the zero-idle-wakeups guarantee, asserted
   directly rather than inferred.
3. A successful re-attempt erases the entry, and the last erase disarms.
4. `Vanished` for a pending id erases it and never yields a due attempt afterwards.
5. `Vanished` for an id that is **not** pending is a no-op.
6. An entry past its deadline reports as expired exactly once, then is gone.
7. An entry is not yet due before the poll interval elapses, and is due after.
8. Two entries resolve independently: one succeeding does not disturb the other's deadline, and the
   set stays armed while either remains (user story 12).
9. The target logical workspace recorded at issue time is the one re-attempted (user story 14).
10. A non-`0x8002802B` failure inserts nothing.

### Seam B — one live Smoke seam (`scripts/guest/WindowRules.Tests.ps1`)

Added to the existing `window-rules` `Describe`, beside `live pin` — the seam it is the reliability
counterpart of. No new test file.

> **`retry-after-view-miss`** — with `WINSPACE_DIAG_DROP_FIRST_MOVE` set for the winspace process,
> start winspace with a `workspace 2` rule, launch the matching app, and assert via
> `Get-WindowDesktopId` that the window ends on workspace 2's GUID. Without the retry this is red
> (the forced first failure is final); with it, green.

Prior art for every mechanic it needs: `WindowRules.Tests.ps1`'s `live pin` (rule-driven placement,
`Start-Winspace` with a written config), and `MoveToWorkspace.Tests.ps1` (the
`Get-WindowDesktopId` + `Get-VdState` Oracle). The only new capability is passing an environment
variable into `Start-Winspace`.

### Deliberately not a live seam

**HWND-recycle cancellation.** Forcing Windows to recycle a specific handle inside a 250ms budget is
not practically achievable in the harness; a live test would prove only "killing the window did not
crash", which is theatre. Covered exactly by Seam A cases 4 and 5.

## Out of Scope

- **Any general retry facility.** One Effect, one `HRESULT`. Nothing else gets wrapped.
- **The frame-paced internal event loop.** Rejected in full — see ADR-0022's Considered Options.
  The main thread stays a blocking shutdown barrier.
- **Per-frame or drain-to-empty event batching, and custom Event ordering.** Not built, not
  scaffolded for.
- **Continuous tiling** and the drag-throttle it would want. That reverses ADR-0007 and is its own
  ADR if it is ever wanted.
- **`IVirtualDesktopNotification::ViewVirtualDesktopChanged`.** Untested, and the strictly better
  mechanism *if* it fires on initial registration. Deliberately deferred, not dismissed — if it is
  ever measured to work, it replaces the polling entirely and this PRD's mechanism can be deleted.
- **Validating the 250ms budget against Edge, Teams, or VMware.** The give-up `warn` is the
  tripwire; the probe on the `prototype/event-order` branch is the instrument. Not blocking.
- **Making `State` honest about placement-in-flight.** Requires the rejected Event/State design.
- **The `title:`-rule-against-a-console-app finding** (titles live on sibling `conhost.exe` HWNDs).
  Unrelated; separately recorded.

## Further Notes

**Why this is worth a PRD and an ADR for ~20 lines of adapter code.** The code is small and
reversible; the *boundary* it establishes is not. winspace will now contain exactly one timer, in a
project whose headline claim is that it contains none. ADR-0022 states the test the next timer must
pass: it waits on an **external** party's asynchronous state, for which no notification exists, and
it is armed on demand with zero idle cost. Anything that wakes up to inspect winspace's own world is
still ADR-0007's ban.

**What we know and what we are guessing.** The lifecycle measurement is real — 4 instrumented runs
on `win11-24h2` establishing that registration lands 31–63ms after `CREATE`, that
`SYSTEM_FOREGROUND` is measurably too early to use as a ready signal, and that no documented event
marks registration. The **250ms budget is an inference from that gap**, not an observed failure
distribution: the natural race was never caught in the instrumented build (0 failures in 6 runs).
That is the weakest link in the design, it is why the give-up path logs loudly, and it is the first
thing to revisit if the warning ever appears.
