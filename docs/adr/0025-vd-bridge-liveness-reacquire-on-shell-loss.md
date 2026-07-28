# 25. Virtual Desktop bridge liveness: re-acquire on shell loss

**Status:** Accepted (2026-07-28)

## Context

The COM bridge is acquired **once**, in the Worker constructor (`win32.cpp:2402`), and
`m_bridge` is a `unique_ptr` where **null means unsupported, permanently** — guarded by
`if (m_bridge)` at eight call sites. Nothing re-validates it, and nothing rebuilds it.

Two ordinary events break that assumption. Both were reproduced on 26100.8893.

**The logon race.** [ADR-0013](0013-autostart-per-user-logon-task.md) argues a logon task
*"reaches the first manageable moment as early as anything can — and Adoption absorbs any
window that opened first."* That reasoning covers **windows**. It does not cover the
**ImmersiveShell**, which is not yet serving at that moment — and unlike a late window, a
missing shell is absorbed by nothing. Observed: `explorer.exe` and winspace both started at
05:51:36; the instance ran ~5 hours with 4 threads (no live COM STA/RPC) against 7 for a
healthy one, hotkeys all registered, every Workspace operation a silent no-op.

**A shell restart.** The proxies go stale but stay non-null and callable, so the null guard
passes, the Reducer runs, the Effect dispatches, and only the last step fails:

```
[ERROR] src\win32.cpp:1987 (hr=0x800706BA): The RPC server is unavailable.   ×3
```

Three errors for three synthesized keypresses — `m_manager->GetDesktops()` inside
`resolveLiveDesktop`. A stale proxy never heals; only a fresh `CoCreateInstance` does.

Two things turn a recoverable fault into a session-long outage. **Place-once**
([ADR-0009](0009-window-rules-place-once-state.md)) means a Place rule's Workspace move is
attempted exactly once, so a move lost during an outage is lost forever — this is the
user-visible *"new apps don't go to their workspace"*. And
[ADR-0004](0004-win32-error-handling.md)'s degrade-and-log strategy writes only to stderr,
which the Logon task discards for a `/SUBSYSTEM:WINDOWS` binary — so in every shipped
configuration "degrade and log" is really *degrade silently*.

**There is no readiness signal.** Measured twice, milliseconds after killing the shell:

| Milestone | Run 1 | Run 2 |
|---|---|---|
| `explorer.exe` process exists | 48 | 50 |
| `WaitForInputIdle` returns | 168 | 150 |
| `Shell_TrayWnd` created | 371 | 322 |
| `IApplicationViewCollection` serving | 503 | 482 |
| **`IVirtualDesktopManagerInternal` serving** | **598** | **576** |
| `IVirtualDesktopNotificationService` serving | 601 | 580 |

Every candidate fires **200–450 ms early**, and the three services come up at *different*
times. There is no instant at which "the shell is ready" — only "this interface answers
now." That is a direct consequence of [ADR-0002](0002-workspaces-as-os-virtual-desktops.md)'s
premise: Microsoft never published these interfaces, so it never defined a moment at which
they become available.

### Measured before implementation: object creation vs. interface registration

The table above did not answer the one question the **Unsupported** discriminator turns on.
The natural discriminator is *"the ImmersiveShell object was created, but no known IID matched
→ this OS is unsupported"*, and that is safe only if object creation and interface
registration happen close together. Measured with `scripts/spike/VdRegistrationTiming.cpp`
(fresh objects every poll, so no cached proxy can answer for a service that is not really
back), on **build 26100.1742**, four runs, milliseconds from the shell process being killed:

| Run | ImmersiveShell object creatable | `IApplicationViewCollection` | `IVirtualDesktopNotificationService` | **`IVirtualDesktopManagerInternal`** | **gap** |
|---|---|---|---|---|---|
| 1 | 1324 | 1324 | 1590 | 1590 | **266** |
| 2 | 737 | 737 | 1477 | 1477 | **740** |
| 3 | 686 | 686 | 797 | 1034 | **348** |
| 4 | 524 | 524 | 639 | 639 | **115** |

**The gap is real: 115–740 ms, mean 367 ms.** The ImmersiveShell object accepts creation
several hundred milliseconds *before* the virtual desktop manager is registered, so a
discriminator that concluded Unsupported the moment an IID failed to match **would misfire on
every ordinary shell restart** — converting a transient fault into the permanent one this ADR
exists to remove. The time-aware rule below is therefore load-bearing, not defensive.

The implemented budget is **3000 ms**, which clears the worst observed gap by ~4×. It costs a
genuinely unsupported build one extra diagnostic-free second at startup and costs a healthy
machine nothing, since a successful acquisition ends the attempt immediately.

Two secondary findings, both supporting **all-or-nothing acquisition**: the view collection
answered in the same poll as object creation in all four runs, and the notification service
led the desktop manager in run 3 (797 vs 1034) while tying it in the others — so the order in
which the required services come up is **not fixed**, and no one service can be used as a
proxy for the others being ready.

*(These runs are on a different revision — 26100.1742 — from the 26100.8893 measurements
above. The two tables measure different things and are not directly comparable; what matters
is that both were taken on 26100, and that the gap is hundreds of milliseconds rather than
tens.)*

## Decision

Treat the bridge as a **connection that can drop and be rebuilt**, not a capability acquired
at birth.

- **Three states replace `nullptr`.** **Connected**; **Disconnected** (recoverable — retry);
  **Unsupported** (terminal — no known IID matched, or a stubbed variant). ADR-0002's
  fail-closed guarantee is unchanged: winspace still never calls through an unverified
  vtable.
- **The bridge object outlives its connection.** It is constructed once and persists.
  `m_bindings`, their **Provenance**, and the **Home desktop** survive every reconnect; only
  the COM pointers are replaced.
- **Death is detected on the shell process handle.** `GetShellWindow()` →
  `GetWindowThreadProcessId` → `OpenProcess(SYNCHRONIZE)` → `RegisterWaitForSingleObject`,
  re-armed on the new PID after each rebuild. A kernel transition cannot be missed: no queue
  to overflow, no broadcast to be excluded from, no dependence on a pump that may be blocked
  inside an outbound COM call.
- **A reactive backstop.** `RPC_S_SERVER_UNAVAILABLE`, `RPC_E_DISCONNECTED`, or
  `CO_E_OBJNOTCONNECTED` at any call site marks the bridge Disconnected.
- **Readiness is verified, never observed.** An event says *when to start asking*; only a
  successful acquisition answers. The asking runs **off the Worker**, loops until **all**
  required services are obtained **together**, and posts **one** message to the Worker.
  Nothing periodic is added to the Worker and nothing touches the input path.

  *Refined at implementation.* COM interface pointers are apartment-affine, so what crosses to
  the Worker is the **proof** that the services answer, not the proxies themselves: the probe
  thread creates every required service on its own apartment, releases them, and posts
  `k_wmReacquire`; the Worker then performs the real acquisition on the STA that owns the
  apartment. This keeps the ADR's actual requirement — the Worker does **one** bounded
  acquisition, never a loop — while avoiding marshalling raw proxies across apartments. The
  second acquisition is immediate and cheap: the services were answering microseconds earlier.
- **Unsupported is never latched mid-attempt.** Within an acquisition attempt an unmatched
  IID counts as Disconnected; Unsupported is concluded only after the attempt's budget
  elapses. A genuinely unsupported build reports a few hundred milliseconds later, once.
- **Reconnection repairs via Reconciliation, not Adoption.** After each success winspace runs
  the existing pure `reconcile(previous, live)` — preserving Provenance, assigning lowest-free
  to anything new — and latches `m_home` on the **first** acquisition only. With no previous
  bindings `reconcile` yields `1..N`, which *is* Adoption's binding behaviour, so the two
  collapse into one path — making literal what `CONTEXT.md` already asserts.

  *Corrected at implementation.* That equivalence was **not** true as written: `reconcile`
  numbered newcomers by sorting their identity bytes, while `adopt()` used the OS's
  enumeration order. Indistinguishable mid-session, where newcomers arrive one at a time —
  and silently catastrophic on the collapsed path, where it reassigned **every** workspace
  number at startup so `workspace 1` no longer meant the leftmost desktop. `reconcile` now
  numbers newcomers in the order `live` presents them, which is the OS's desktop order.
  Existing bindings were always matched by identity and are unaffected either way, so the
  byte-sort was buying an order-independence that only ever applied to the half that did not
  need it. Caught by the shell-restart Smoke seam, not by the unit tests — which had encoded
  the byte-sort as an assertion.
- **Moves queue; switches drop.** A Disconnected bridge records a **Pending move**
  (`WindowId` + logical) and replays it on reconnect, pruning windows that have since gone.
  A `workspace N` press made while disconnected is a no-op.

Both triggers converge on one **idempotent, coalesced** rebuild, so overlapping triggers are
no-ops by construction — the same property ADR-0023 relies on.

## Considered options

- **Bounded startup retry only.** Rejected: it cannot fix the explorer-restart case, where
  the bridge was built successfully and later died.
- **Reactive re-acquire only, no proactive signal.** Rejected: with no bridge object built at
  all (the logon case) there is nothing to fail on, and recovery would wait for the first
  keypress — by which time the `exec-once` apps have appeared and lost their placement to
  Place-once. That is precisely the reported symptom.
- **`TaskbarCreated` broadcast.** Rejected twice over: it never arrives, because the Worker
  window is `HWND_MESSAGE` (`win32.cpp:2388`) and broadcasts reach only top-level windows;
  and it fires ~200 ms early regardless. Receiving it would mean a hidden top-level window,
  surrendering windowlessness for a signal that is wrong anyway.
- **`WaitForInputIdle` on the shell process.** The strongest documented readiness primitive
  Win32 offers. Rejected on measurement: **430 ms early**.
- **A Shell Service Object** (a COM DLL the shell loads in-process at startup). Genuinely
  "notified when the shell starts". Rejected: it means shipping a second binary that loads
  **inside explorer**, defeating [ADR-0017](0017-distribution-via-self-bucketed-scoop-package.md)'s
  single-exe package and [ADR-0018](0018-release-links-crt-statically.md)'s static CRT.
- **SENS `ISensLogon::StartShell`.** Documented and real. Rejected: it needs a persistent
  COM+ catalog subscription, announces shell *start* rather than service availability, and
  does not fire on an explorer restart.
- **`SetWinEventHook` on the shell window's `EVENT_OBJECT_DESTROY`,** reusing the existing
  Hook thread. Rejected: it infers a process fact from a window fact by pattern-matching
  class names (`Progman`, `WorkerW`, one `Shell_TrayWnd` *per monitor*); destroy delivery for
  another process is best-effort under `WINEVENT_OUTOFCONTEXT` and can be dropped on a
  force-kill; and it needs a private path *upstream* of the noise gate whose whole job is
  suppressing exactly that traffic.
- **`SetTimer` on the Worker window** to drive retries. Rejected: it puts a periodic message
  into the pump that must stay clear, for a burst that lasts 200–450 ms.
- **Destroy and reconstruct the bridge on rebuild, re-running `adopt()`.** The smallest diff.
  Rejected as silently destructive — see Consequences.
- **Queue switches as well as moves.** Rejected: a stale switch replayed after the outage is
  not a delayed success but a surprise, and it would race the reconcile that is recomputing
  the current Workspace at the least trustworthy moment.

## Consequences

- **Provenance loss is the failure this ADR exists to prevent.** Reconstructing the bridge
  would re-run **Adoption**, tagging every desktop `foreign`, after which
  [ADR-0024](0024-quit-cleanup-provenance-gated.md) would leave behind every desktop winspace
  created — and would relocate the Home desktop to wherever the user stood when the shell
  died. The failure is silent and surfaces only at quit. Persisting the bridge object is
  therefore not a tidiness choice; it is the correctness requirement.
- **The retry loop is not laziness — it is the only available observation.** The measurement
  table above is the load-bearing artefact of this ADR. Without it a future reader will
  "simplify" the loop into an edge trigger and reintroduce the outage.
- **All-or-nothing acquisition is required, not defensive.** The notification service came up
  **3 ms** after the desktop manager in run 1. A partial acquisition leaves
  `m_notificationCookie == 0` and permanently degrades Reconciliation to
  `reconcileBeforeOperation`.
- **A short poll survives, relocated.** Moving it to the wait thread removes it from the
  Worker and from the input path, but it is still polling. That is honest and unavoidable.
- **A third undocumented dependency in spirit, though not in vtable.** Which process hosts
  the ImmersiveShell is an implementation detail Microsoft may change. The reactive backstop
  is what makes that survivable: recovery keys off the *failure*, not off our theory of the
  hosting arrangement.
- **Presses during an outage are silently lost.** At logon that is a sub-second window; after
  a shell crash the user presses, sees nothing, and presses again. Accepted as the correct
  trade over a surprise switch.
- **The Reducer, State, Events and Effects are all unchanged.** Connection lifetime is
  entirely an I/O concern. The only core addition in this work is a pure function
  (see ADR-0024's amendment). This is deliberate: the Reducer must not learn a connection
  exists.
- **Recovery is only as observable as the log.** This ADR is accepted together with a
  size-capped file sink behind the existing choke point (`win32.cpp:94-106`), because
  ADR-0004's strategy is load-bearing and currently has no destination in any shipped
  configuration.
- **The forced-outage test hook** (`WINSPACE_FORCE_VD_UNAVAILABLE_MS`) joins
  `WINSPACE_FORCE_VD_VARIANT` (`win32.cpp:1650`) as production code that exists for tests.
  A second instance of an accepted trade: without it the Pending move path is covered only by
  a race that passes on fast machines.
