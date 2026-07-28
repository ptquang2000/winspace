# PRD 0027 — Virtual Desktop bridge liveness

Status: ready-for-agent

## Problem Statement

winspace silently stops managing Workspaces, and the user has no way to find out.

Two user-visible symptoms, both reported together:

1. **A newly launched app does not move to its configured Workspace.** A `windowrule = workspace N, exe:…` never takes effect for apps started at login.
2. **Workspace switching does nothing.** Every `workspace N` bind is a no-op for the whole session.

From the user's seat the two look unrelated, and neither looks like a crash. winspace is still running, every hotkey is still registered, `focus`, `tile`, and `movetodisplay` all still work. Only the two dispatchers that touch Virtual Desktops are dead — and they are dead *quietly*, for hours, until the user restarts winspace by hand.

Both are the same defect. The COM bridge to the OS Virtual Desktop interfaces is acquired **exactly once**, when the Worker thread starts, and is never re-validated or rebuilt. Two ordinary events break it:

- **The logon race.** The Logon task starts winspace at the same moment the shell starts. When winspace wins, the ImmersiveShell is not yet serving, bridge acquisition fails, and winspace runs the entire session with Virtual Desktop support disabled.
- **A shell restart.** If the shell restarts or crashes mid-session, the bridge's COM proxies go stale. They remain non-null and callable, so no null check fires and nothing crashes — every call simply fails with `RPC_S_SERVER_UNAVAILABLE` and returns to a caller that logs and moves on.

Two aggravating factors turn a recoverable fault into a session-long outage:

- **Placement is Place-once.** A Place rule's Workspace move is attempted exactly once, on the window's first Eligible `Appeared`. If the bridge is down at that instant the move is lost permanently — there is no later edge and no retry. This is symptom 1.
- **Diagnostics have no sink.** Every failure degrades and logs, per ADR-0004, but logging goes only to stderr and the Logon task launches a windowless binary with nowhere for stderr to go. In every shipped configuration, "degrade and log" is really "degrade silently."

A third, independent defect surfaced during the same investigation: `exec-once = msedge` fails with "file not found" even though `msedge` launches fine from the Run dialog. Launch entries are handed to Win32 as a verbatim command line, and that API searches `PATH` but not the shell's registered application paths — where the shell records most normally-installed applications (browsers, Office, developer tools). The launcher therefore cannot start a large class of ordinary apps, and says so only in the log nobody can read.

## Solution

winspace treats the Virtual Desktop bridge as a **connection that can drop and be rebuilt**, rather than a capability acquired once at birth.

From the user's perspective:

- **Workspace switching survives a shell restart.** If the shell crashes or is restarted, winspace notices, reconnects on its own, and the next `workspace N` press works. No restart, no relogin, no lost configuration.
- **Apps launched at login land on their configured Workspace.** If a window appears while the bridge is down, its Workspace move is remembered and applied as soon as the connection is restored, rather than being lost to Place-once.
- **Logical Workspace numbers, Provenance, and the Home desktop are unchanged by a reconnect.** After recovery, the same numbers address the same desktops, and quitting still destroys exactly the desktops winspace created and no others.
- **Quitting cleans up even if the Home desktop was destroyed by hand.** Cleanup picks a surviving desktop to land on and migrate windows to, preferring one winspace did not create.
- **A genuinely unsupported Windows build still fails loudly and immediately**, exactly as today. Recovery applies to a *lost* connection, never to an unsupported OS.
- **The launcher starts normally-installed applications by bare name.** `exec-once = msedge` works without the user discovering an install path.
- **There is a log to read.** Diagnostics are written to a size-capped file in the user's local application data, so a fault that happened at login can be diagnosed afterwards.

## User Stories

1. As a winspace user, I want `workspace N` to keep working after the shell restarts, so that a shell crash does not silently disable my window manager for the rest of the day.
2. As a winspace user, I want winspace to reconnect to the shell on its own, so that I do not have to notice, diagnose, and restart it myself.
3. As a winspace user, I want an app launched by a Launch entry at login to land on the Workspace its windowrule names, so that my session starts laid out the way I configured it.
4. As a winspace user, I want a Place rule that could not be applied immediately to be applied as soon as it can be, so that Place-once does not turn a transient fault into permanent misplacement.
5. As a winspace user, I want a Workspace move that was queued for a window I closed in the meantime to be discarded, so that closing a window never resurrects it somewhere else.
6. As a winspace user, I want a `workspace N` press made while winspace is disconnected to be dropped rather than replayed later, so that my desktop never switches out from under me seconds after I gave up and moved on.
7. As a winspace user, I want my logical Workspace numbers to address the same desktops after a reconnect, so that muscle memory keeps working.
8. As a winspace user, I want desktops winspace created to still be recognised as its own after a reconnect, so that quitting cleans up after itself as it always did.
9. As a winspace user, I want desktops I created myself to still be recognised as mine after a reconnect, so that quitting never deletes my work.
10. As a winspace user, I want the Home desktop to remain the desktop where winspace found me, so that quitting returns me somewhere meaningful rather than wherever I happened to be when the shell crashed.
11. As a winspace user, I want Quit cleanup to still work if I destroyed the Home desktop by hand in Task View, so that a stray Task View action does not leave winspace's desktops behind forever.
12. As a winspace user, I want Quit cleanup to prefer landing on a desktop winspace did not create, so that cleanup never spares one of its own desktops as a side effect of standing on it.
13. As a winspace user, I want windows on a destroyed desktop to migrate rather than vanish, so that cleanup can only ever consolidate my windows.
14. As a winspace user on an unsupported Windows build, I want winspace to say so once and clearly, so that I understand it will not work rather than watching it retry forever.
15. As a winspace user, I want winspace never to call through a COM interface whose layout it has not verified, so that an OS update degrades my window manager rather than corrupting my session.
16. As a winspace user, I want directional focus, tile, and movetodisplay to keep working while the bridge is down, so that a Virtual Desktop fault costs me only Virtual Desktop features.
17. As a winspace user, I want Ignore rules, Distribute, and Slot placement to keep working while the bridge is down, so that windows are still laid out sensibly during a shell outage.
18. As a winspace user, I want reconnection to cost nothing while everything is healthy, so that recovery machinery never slows down my keypresses.
19. As a winspace user, I want winspace to stay responsive to hotkeys throughout an outage and a reconnect, so that it never appears hung.
20. As a winspace user, I want winspace to keep trying to reconnect indefinitely rather than giving up, so that a long shell outage still ends in recovery.
21. As a winspace user, I want `exec-once = msedge` to start Edge, so that I can name applications the same way I do in the Run dialog.
22. As a winspace user, I want the same to hold for other normally-installed applications, so that the launcher is not quietly limited to things on `PATH`.
23. As a winspace user, I want a launch failure to tell me *why* it failed, so that I can fix my config without reading source.
24. As a winspace user, I want the launcher not to depend on the shell being ready, so that starting apps at login is not subject to the same race this work exists to fix.
25. As a winspace user, I want diagnostics written to a file, so that I can diagnose a fault that happened at login hours later.
26. As a winspace user, I want that log to be size-capped, so that a persistent fault cannot fill my disk.
27. As a winspace user, I want the log in a predictable per-user location, so that I can find it and attach it to a bug report.
28. As a winspace user, I want winspace to remain windowless, so that recovery and logging never put a console or a taskbar button on my screen.
29. As a winspace maintainer, I want the two reasons the bridge can be absent — unsupported OS and lost connection — to be distinct named states, so that nobody re-collapses them and reintroduces this bug.
30. As a winspace maintainer, I want the choice of shell-death signal recorded with the measurements that rejected the alternatives, so that a future reader does not "simplify" the retry loop back into the outage.
31. As a winspace maintainer, I want the desktop-set repair on reconnect to reuse the existing Reconciliation policy rather than re-running Adoption, so that Provenance cannot be silently lost.
32. As a winspace maintainer, I want the cleanup landing target to be a pure function over bindings, so that its preference order is unit-tested rather than exercised only at quit.
33. As a winspace maintainer, I want the Reducer to remain unaware that a connection exists at all, so that the core seam stays pure and testable.
34. As a winspace maintainer, I want reconnection to be idempotent and coalesced, so that overlapping triggers cannot race.
35. As a winspace maintainer, I want a deterministic way to force the disconnected state in a test, so that the recovery path is covered by a fixture rather than by a race that passes on fast machines.
36. As a winspace maintainer, I want the queued-move path covered by its own Smoke seam, so that the half of the fix that addresses the reported symptom is not shipped untested.

## Implementation Decisions

### Bridge availability is a three-state model, not a null pointer

The bridge is currently held as an optional value where absence means "this OS is unsupported, permanently". That conflates two different facts. Availability becomes three explicit states:

- **Connected** — all required shell services acquired and verified.
- **Disconnected** — recoverable; the shell is not serving *yet* or not serving *any more*. Retried.
- **Unsupported** — terminal; no known interface identity matched, or one matched a variant whose vtable is not implemented. Diagnosed loudly once, never retried. ADR-0002's fail-closed guarantee is preserved unchanged.

### The bridge object outlives its connection

The bridge is constructed once and persists for the process lifetime. The logical-to-DesktopKey bindings, their Provenance, and the Home desktop are owned by that object and survive every reconnect. Only the COM pointers are replaced.

This is the load-bearing decision. Destroying and reconstructing the bridge would destroy the bindings and re-run **Adoption**, which tags every desktop `foreign` — after which **Quit cleanup** would leave behind every desktop winspace created, and would relocate the Home desktop to wherever the user happened to be when the shell died. The failure would be invisible until quit.

### Detection is proactive, with a reactive backstop

**Proactive.** winspace waits on the shell process as a kernel object, resolving it from the shell window and holding a synchronize-only handle registered with a thread-pool wait. This fires the instant the shell process exits. It cannot be missed: there is no message queue to overflow, no broadcast to be excluded from, and no dependency on a pump that may be blocked inside an outbound COM call. The wait is re-armed on the new shell process after each successful reconnect.

**Backstop.** Any bridge call returning `RPC_S_SERVER_UNAVAILABLE`, `RPC_E_DISCONNECTED`, or `CO_E_OBJNOTCONNECTED` marks the bridge Disconnected. The shell process exiting and the shell services becoming unavailable are correlated, not identical; the backstop makes recovery depend on the failure itself rather than on an assumption about which process hosts the interfaces.

Both paths converge on one idempotent, coalesced rebuild, so overlapping triggers are no-ops by construction — the same property ADR-0023 relies on for Reconciliation.

### There is no readiness signal; readiness is verified, not observed

Measured twice on build 26100.8893, milliseconds after the shell process was killed:

| Milestone | Run 1 | Run 2 |
|---|---|---|
| shell process exists | 48 | 50 |
| `WaitForInputIdle` returns | 168 | 150 |
| taskbar window created | 371 | 322 |
| application view collection serving | 503 | 482 |
| **virtual desktop manager serving** | **598** | **576** |
| virtual desktop notification service serving | 601 | 580 |

Every candidate signal fires 200–450 ms early, and the shell's services begin serving at *different* times. There is no instant at which "the shell is ready"; there is only "this interface answers now." This is a consequence of ADR-0002's premise — these interfaces are undocumented, so no readiness contract exists for them.

Rejected as readiness signals, each for a recorded reason: the taskbar-created broadcast (needs a top-level window, which winspace does not have, and fires ~200 ms early anyway); `WaitForInputIdle` (~430 ms early); a shell service object loaded in-process by the shell (requires a second binary loaded inside the shell, defeating the single-binary package and static runtime linkage); a system event notification subscription (announces shell *start*, not service availability, and does not fire on shell restart); a shell hook window (needs a top-level window, carries no readiness fact).

Therefore: an event tells winspace *when to start asking*; only a successful acquisition tells it the answer.

### Re-acquisition runs off the Worker thread and is all-or-nothing

The rebuild runs on the thread-pool wait thread, not the Worker. It attempts acquisition in a short loop and posts a **single** message to the Worker carrying an already-proven-live bridge. The Worker's message pump is unchanged, nothing periodic is added to it, and nothing is added to the input path.

Acquisition succeeds only when **all** required shell services are obtained together. The measurements show the notification service becoming available as little as 3 ms after the desktop manager; accepting a partial acquisition would leave the notification sink unregistered and permanently degrade Reconciliation to its per-operation fallback.

### Reconnection repairs the desktop set via Reconciliation, and Adoption collapses into it

After each successful acquisition, winspace re-derives bindings using the existing pure Reconciliation policy, which preserves Provenance and assigns lowest-free logical numbers to anything new. The Home desktop is latched on the **first** successful acquisition only and never re-seeded.

With no previous bindings, Reconciliation assigns `1..N` — which is exactly Adoption's binding behaviour. The two therefore become one code path, making literal what the glossary already asserts: Adoption is the one-shot startup form of the same reconciliation.

### Unsupported is never latched during an acquisition attempt

A shell that is starting up may accept the initial object creation before it has registered the virtual desktop interfaces. Classifying "object created, no known interface identity matched" as Unsupported at that moment would wrongly latch the terminal state during normal startup. Unsupported is therefore only concluded after the acquisition attempt's full budget has elapsed; within the attempt, an unmatched identity is treated as Disconnected. A genuinely unsupported build reports a few hundred milliseconds later, once.

*(The relative ordering of object creation and interface registration was not measured during design and must be measured before implementation. The rule above is safe regardless of the answer.)*

### Workspace moves are queued while disconnected; switches are not

A Disconnected bridge records a pending move — a window identity and a target logical Workspace — instead of failing. Pending moves are replayed on successful reconnection, and entries whose window no longer exists are discarded at replay time. The collection is capped, with a loud diagnostic on drop. Neither the Reducer nor State is aware of it.

Only the Workspace move requires the bridge. Ignore-set insertion, Distribute, and Slot placement are plain geometry writes and continue working normally throughout an outage.

A `workspace N` press made while disconnected is **dropped**, not queued. The asymmetry is deliberate and rests on who is waiting: a move is a promise made to a window on an edge nobody is watching, with no second chance under Place-once, so replaying it late produces exactly the configured outcome. A switch is a live response to a keypress; replaying it after the outage is not a delayed success but a surprise. Dropping also keeps reconnection doing exactly one thing, rather than racing a replayed switch against the Reconciliation that is recomputing the current Workspace.

### Quit cleanup gains a cleanup anchor, distinct from the Home desktop

The Home desktop currently serves two jobs that only look like one, because until now it always survived:

- **Home desktop** — where winspace found the user. A fact about the past.
- **Cleanup anchor** — the desktop cleanup switches to and migrates windows to. A *requirement*: it must survive the cleanup.

These separate the moment the Home desktop can be destroyed by hand. The anchor is chosen at quit time by a pure function over the bindings, preferring, in order: the Home desktop if still live; otherwise the lowest-logical **foreign** desktop; otherwise the lowest-logical desktop.

Foreign-first is load-bearing rather than a preference. Cleanup never removes the desktop it is standing on, so anchoring on a `createdByWinspace` desktop would exempt one of winspace's own desktops from removal. Preferring foreign makes that exemption reachable only in the genuine last resort where every desktop is winspace's.

The Home desktop is **not** re-seeded. It remains a fact about the past; the anchor is derived.

This amends ADR-0024.

### Launch entries fall back to the shell's registered application paths

When starting a Launch entry fails with "file not found", winspace consults the shell's registered application path for the named executable and retries once. The diagnostic names the cause.

This preserves the Launch entry contract — the command is still stored unparsed and handed to Win32 as-is, with no variable expansion. The fallback widens *where Win32 looks*; it does not parse or transform the command.

Deliberately **not** implemented by switching to shell execution. Shell execution would resolve application paths for free, but it requires the shell — and Launch entries fire at winspace startup, which at login is precisely the window this work exists to survive. It would reintroduce the exact dependency being engineered around, in the one code path guaranteed to run inside the danger window, and would additionally swap direct process creation for shell verb resolution, a much larger semantic change than it appears.

### Diagnostics gain a file sink

A second sink is added behind the existing single logging choke point, writing to a size-capped file under the user's local application data, in addition to stderr. This is a second sink, not a logging refactor.

ADR-0004's degrade-and-log strategy is load-bearing throughout winspace, and in every shipped configuration the log currently has no destination. A post-hoc file is the only option that helps with a fault that occurred at login; on-demand streaming and debugger-output mirroring both require the observer to already suspect a problem and be attached when it recurs.

### Documentation deliverables

- **A new ADR** recording bridge liveness end to end, and carrying the measurement table above. Without the measurements the retry loop reads as laziness and will eventually be "cleaned up" back into this defect.
- **An amendment to ADR-0024** recording the cleanup anchor.
- **Glossary additions**: Disconnected, Unsupported, Re-acquisition, Pending move, Cleanup anchor.
- **Glossary edits**: Home desktop (stop implying it is the migration target), Adoption and Reconciliation (one mechanism now), Launch entry (the application-path fallback).

## Testing Decisions

### What makes a good test here

Tests assert **external behaviour** only: what the Reducer emits for a given Event, what pure policies return for given plain data, and what the OS reports after the running binary has been driven. No test observes connection state, retry counts, timing internals, or the shape of the pending-move collection. A test that would fail on a refactor which preserved behaviour is the wrong test.

### No new seams

Both seams already exist and both are reused unchanged:

- **The core seam** — pure functions over plain data, exercised directly. Prior art: the existing Reconciliation and cleanup-selection policy tests, and the Reducer tests generally.
- **The Smoke seam** — the running binary driven in a live guest session with real synthesized input, asserted against OS state as the Oracle. Prior art: the workspace-reconcile, quit-cleanup, window-rules, and launcher suites.

The forced-outage environment variable is a **fixture on the existing Smoke seam**, not a new seam. It mirrors the existing forced-variant hook, which exists for exactly the same reason: to make an otherwise unreachable I/O-layer state observable on a normal development machine. New test files use the same seam.

### Core seam

Cleanup anchor selection, as a total function over bindings:
- Home desktop still live → Home is chosen.
- Home gone, foreign desktops present → lowest-logical foreign is chosen.
- Home gone, every desktop created by winspace → lowest-logical is chosen.
- No bindings → no anchor, and cleanup is skipped.
- Home gone and the lowest-logical desktop is winspace's while a higher-logical foreign exists → the foreign one wins, proving preference beats ordering.

Reconciliation, extended:
- Provenance survives a rebuild — a desktop created by winspace is still marked as such after re-deriving bindings from the same live set. This is the property whose violation causes data loss, and it must fail loudly if anyone reintroduces Adoption on the reconnect path.
- Logical numbers are stable across a rebuild with an unchanged live set.
- The Home desktop is unchanged by a rebuild.

### Smoke seam

- **Shell-restart recovery.** With winspace running and healthy, restart the shell, then Trigger `workspace N`. Oracle: the OS reports the current Virtual Desktop changed. Also asserts winspace is still alive, still windowless, and still holds its hotkeys.
- **Create-on-demand after recovery.** Trigger a switch to an unbound Workspace after a shell restart; Oracle: the desktop count increases and the current desktop is the new one.
- **Provenance survives recovery.** Create a Workspace on demand, restart the shell, then quit. Oracle: the created desktop is gone and pre-existing desktops remain.
- **Queued placement across a forced outage.** With the forced-outage fixture active, launch an app whose Place rule names a Workspace, into the outage window. Oracle: after recovery the window is on the configured Virtual Desktop. This is the only test that covers the pending-move path — a shell restart cannot, because no windows are appearing during one.
- **Queued move for a closed window is discarded.** Same fixture; close the window before recovery. Oracle: no desktop membership change, and winspace does not fault.
- **Switches during an outage are not replayed.** Same fixture; Trigger a switch during the outage. Oracle: the current desktop is unchanged both during the outage and after recovery completes.
- **Unsupported still fails closed.** The existing forced-variant seam must continue to produce its diagnostic and must **not** enter a retry loop.
- **Launcher application-path fallback.** A Launch entry naming a bare executable that is registered with the shell but absent from `PATH`. Oracle: the process is running.
- **Log file exists.** After a run containing at least one degraded operation, the log file exists in the expected location and contains the diagnostic. This makes the observability change itself observable.

## Out of Scope

- **The view-registration race on brand-new windows.** A Place rule's Workspace move intermittently fails because the shell has not yet registered a view for a freshly created window, and Place-once means it is never retried. This is a *separate* defect in the `Appeared` handler's timing, independent of connection lifetime, and it reproduces on a perfectly healthy bridge. It is deferred to its own piece of work. The pending-move mechanism introduced here is its natural fix vehicle.
- **Reaping** — destroying empty, unfocused Workspaces during a session. Still deferred; still blocked on per-Workspace occupancy tracking.
- **Implementing the stubbed OS variants.** Unsupported builds continue to fail closed.
- **Queuing anything other than Workspace moves.** Switches are dropped by decision; focus, tile, and movetodisplay never needed the bridge.
- **Log rotation beyond a single size cap**, log levels, structured output, or any log-viewing command.
- **Changing the Launch entry grammar**, adding variable expansion, or adding a way to launch applications from a keybind.
- **Re-seeding the Home desktop.** The anchor is derived instead.
- **Changing the Logon task's trigger or adding a startup delay.** The race is fixed in winspace, not worked around in the scheduler.

## Further Notes

- **Nothing in the Reducer, State, Event set, or Effect set changes.** Connection lifetime is entirely an I/O-layer concern. The only core additions are one pure function and its tests. This is deliberate: the Reducer must not learn that a connection exists.
- **The user's existing configuration needs no edit** once the application-path fallback lands. The `exec-once = msedge` line begins working as written.
- **Everything in this document was reproduced on Windows 11 build 26100.8893.** The failure was observed live: the shell and winspace starting in the same second, three `RPC_S_SERVER_UNAVAILABLE` failures for three synthesized keypresses after a shell restart, and full recovery on a manual restart of winspace. Timings should be re-measured if the OS build changes materially.
- **The `Provenance` regression is the highest-consequence way to get this wrong.** It is silent, it only manifests at quit, and its two failure directions are "leaves the user's desktops littered with winspace's" and "deletes the user's desktops". The test asserting Provenance survives a rebuild is the single most important test in this work.
- **The PRD number is a guess.** PRDs are referenced from source comments but not stored in this repository; `0027` follows the highest referenced number. Renumber if the tracker disagrees.
