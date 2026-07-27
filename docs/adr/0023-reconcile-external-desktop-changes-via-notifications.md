# 23. Reconcile external Virtual Desktop changes via the notification service

**Status:** Accepted (2026-07-27)

## Context

[ADR-0003](0003-sparse-virtual-workspace-model.md) closes by naming this gap explicitly:
*"Reconciliation of desktops created outside winspace mid-run is a separate deferred
policy."* This ADR is that policy.

Today `adopt()` runs **once**, at startup. Nothing after it observes the Virtual Desktop
set. Press Win+Ctrl+D and the new desktop is never bound to a Logical workspace number, so
winspace's numbering silently diverges from what Task View shows — press `workspace 4` and
you may get a *fifth* desktop appended rather than the one you are looking at. Press
Win+Ctrl+→ and both `State.currentWorkspace` and the bridge's `m_current` go stale.

The switch half of that is presently **latent**: `currentWorkspace` is written in three
places and read in **none**, and `switchTo` resolves its target by GUID, so a switch lands
correctly however stale the field is. That changes the moment
[ADR-0024](0024-quit-cleanup-provenance-gated.md) lands, which needs to know what exists
and who made it.

## Decision

Subscribe to `IVirtualDesktopNotification` and reconcile.

- **Provenance.** Each `logical→GUID` binding gains one bool, `createdByWinspace`, set true
  only by `createAndBind`. This separates two questions that look like one: whether a
  desktop is **addressable** (bound to a logical number) from whether it is **ours**
  (destroyable by Quit cleanup). Every desktop seen by Adoption or Reconciliation is
  **foreign**.
- **Foreign desktops are bound anyway**, at the **lowest free** logical number — so labels
  stay compact and reuse numbers freed by an external destroy. `max+1` would ratchet
  upward all session and leave you binding `workspace 17` on a four-desktop machine.
- **A notification is a trigger, never the data.** The sink posts to the Worker's
  message-only window and touches nothing else. On a clean pump turn the Worker runs one
  **idempotent reconcile**: re-enumerate, rebuild bindings preserving Provenance, assign
  lowest-free to anything new, recompute current.
- **Split by layer.** Map maintenance (`Created`, `Destroyed`, `Moved`, `NameChanged`)
  stays bridge-internal per ADR-0003. The one fact the Reducer models re-enters as a single
  new Event, **`WorkspaceChanged{logical}`**, which becomes the **sole writer** of
  `State.currentWorkspace` — retiring the three speculative writes that recorded *intent*
  (`winspace.cpp:740`, `:768`, `win32.cpp:1672`).
- **Degrade path.** If a future Windows build breaks the notification vtable, log it
  ([ADR-0004](0004-win32-error-handling.md)) and run the same reconcile at the top of each
  workspace operation instead — noticing on the next keypress rather than dying.

## Considered options

- **Do nothing.** GUID resolution self-heals switches, so this survives longer than it
  looks. Rejected: it does not fix external *creation*, and it leaves ADR-0024 with no
  ownership signal to build on.
- **Reconcile on demand only** (no notifications; re-enumerate before each workspace
  operation). Genuinely attractive — it is the same stateless-read-at-the-moment-of-need
  pattern that Spatial focus, Distribute and `tile` all already commit to, and it adds no
  COM surface. Rejected because it can only ever notice a change on the *next* keypress,
  never react to one. Retained as the degrade path, so the design still has this behaviour
  when the notification path is unavailable.
- **A dedicated notification thread with its own STA**, mirroring the Hook thread's
  isolation. Rejected: it would need cross-apartment marshalling back to the Worker to
  solve a problem a `PostMessage` already solves.
- **Callback does the work inline on the Worker STA.** Rejected as unsafe, not merely
  untidy — see Consequences.
- **Full Event set** (`DesktopCreated` / `DesktopDestroyed` into `reduce`). Rejected: drags
  GUIDs or a parallel identity scheme toward the core against ADR-0003, for no behaviour
  gain.

## Consequences

- **A second undocumented COM vtable.** This is the real cost, taken knowingly.
  `IVirtualDesktopNotification` joins `IVirtualDesktopManagerInternal` as an interface that
  must be re-pinned per Windows build, doubling the surface a Windows update can silently
  break. The degrade path is what makes that survivable: a broken vtable costs liveness,
  not function.
- **Reentrancy is the hazard the sink design exists to kill.** An STA sink's callbacks are
  dispatched by the message pump — *including the pump that runs inside an outbound
  blocking COM call*. A `CurrentVirtualDesktopChanged` can therefore arrive **inside**
  `doSwitch`'s own `SwitchDesktop`, while `resolveLiveDesktop` is mid-iteration over the
  desktop snapshot. A callback that only posts cannot corrupt anything.
- **Ordering and dropped notifications stop mattering.** Because a full re-enumeration is
  idempotent, coalescing many notifications into one reconcile is correct by construction,
  and no `Created`-before-`CurrentChanged` ordering has to be modelled — which is precisely
  where undocumented COM implementations tend to surprise you.
- **`currentWorkspace` becomes eventually consistent.** Between a bind firing and the
  notification landing, State still names the *old* Workspace. Nothing reads the field
  today, so there is no hazard now — but any future logic that switches and then reads
  "where am I" inside the same `reduce` will read stale. ADR-0024's quit sequence is the
  first thing that must respect this.
- The notification fires for winspace's **own** switches too. That is not noise to filter:
  it is what lets one path serve both cases, and it completes the pattern the codebase
  already uses twice — the Reducer emits intent (`ResolveFocus`, `ResolveDistribute`) and
  reality re-enters as an Event (`FocusResolve`, `DistributeResolve`). `SwitchToWorkspace`
  → `WorkspaceChanged` is the third instance.
