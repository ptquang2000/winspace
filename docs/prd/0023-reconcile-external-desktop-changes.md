# PRD 0023 — Reconcile Virtual Desktop changes winspace did not cause

**Status:** Ready for agent
**ADR:** [ADR-0023](../adr/0023-reconcile-external-desktop-changes-via-notifications.md)
**Lands:** second (after PRD 0025); **PRD 0024 depends on this one's Provenance**

## Problem Statement

winspace looks at the Virtual Desktop set exactly once, at startup. After that it assumes it
is the only thing changing desktops. It isn't — Windows gives the user Win+Ctrl+D,
Win+Ctrl+F4, Win+Ctrl+←/→ and Task View, and all of them work whether winspace likes it or
not.

So: the user presses Win+Ctrl+D, gets a new desktop, and winspace never binds it to a
**Logical workspace number**. Press `workspace 4` while standing on it and winspace appends a
*fifth* desktop rather than landing on the one in front of you. Press Win+Ctrl+→ and both
`State.currentWorkspace` and the bridge's current workspace go stale and stay stale for the
rest of the session. The user's mental model of "workspace 3" and winspace's diverge
permanently, with nothing reported.

## Solution

Subscribe to the OS's Virtual Desktop notifications and **reconcile** — treating each
notification as a *trigger* to re-derive the truth, never as the truth itself.

Every desktop becomes **addressable** (bound to a logical number, lowest free), including
ones the user made by hand. Each binding records **Provenance** — whether winspace created
that desktop — which keeps "addressable" separate from "ours" and is what makes
[PRD 0024](0024-quit-cleanup.md)'s cleanup safe to build. The single fact the Reducer models,
which Workspace is current, re-enters as one new Event and becomes honest for the first time.

## User Stories

1. As a user, I want a desktop I create with Win+Ctrl+D to be reachable with `workspace N`,
   so that winspace and Task View never disagree about what exists.
2. As a user, I want `workspace 4` pressed while standing on an unbound desktop to *land*
   somewhere sensible rather than silently appending a new desktop, so that I do not
   accumulate junk desktops by pressing keys that look like they should work.
3. As a user, I want a desktop I destroy with Win+Ctrl+F4 to release its logical number, so
   that my numbering stays compact instead of drifting upward all session.
4. As a user, I want the freed number reused by the next desktop, so that a long session does
   not leave me binding `workspace 17` on a four-desktop machine.
5. As a user, I want switching with Win+Ctrl+→ to leave winspace correctly informed about
   where I am, so that the next winspace key I press behaves as if I had used winspace all
   along.
6. As a user, I want desktops I created by hand to survive winspace's lifecycle, so that
   winspace inherits my session rather than annexing it.
7. As a user, I want reordering desktops by dragging in Task View not to break my workspace
   numbers, so that the GUID-anchored promise of ADR-0003 holds under external edits too.
8. As a user, I want winspace to keep working — degraded, not dead — if a Windows update
   breaks the notification interface, so that a patch Tuesday never leaves me without a
   window manager.
9. As a user, I want that degradation to still notice external changes on my next keypress,
   so that "degraded" means slower to notice, not wrong.
10. As a user, I want a broken notification interface reported in the log, so that I can tell
    the difference between degraded and healthy.
11. As a maintainer, I want notifications coalesced into one idempotent reconcile, so that no
    ordering between `Created` and `CurrentChanged` has to be modelled.
12. As a maintainer, I want a notification arriving *during* an outbound COM call to be
    incapable of corrupting the binding map, so that STA reentrancy is designed out rather
    than debugged later.
13. As a maintainer, I want the Reducer to keep reasoning in bare logical numbers, so that
    ADR-0003's `windows.h`-free core rule holds.
14. As a maintainer, I want `State.currentWorkspace` to have exactly one writer, so that the
    field means "where the OS says we are" rather than "where we hoped to go".
15. As a maintainer, I want the reconcile policy to be a pure function, so that lowest-free
    assignment and Provenance preservation are unit-tested without a live desktop.

## Implementation Decisions

- **Provenance.** Each binding becomes `{logical, key, createdByWinspace}`. Set true **only**
  by `createAndBind`. Adoption and Reconciliation both record `false`. Separates
  *addressable* from *ours*.
- **An opaque `DesktopKey` in core.** A plain 16-byte POD carrying the desktop's identity,
  `windows.h`-free — mirroring how `WindowId` and `MonitorId` are already opaque handle
  newtypes the Reducer reasons over without knowing what they wrap. The bridge narrows
  `GUID → DesktopKey` at the boundary, exactly as it already narrows `Mod→MOD_*` and
  `Key→VK_*`. This is what lets the reconcile policy be pure.
- **A pure reconcile policy.** `reconcile(previous, liveKeys) → bindings`:
  - keys present in both keep their logical number **and** their Provenance;
  - keys in `previous` but not `live` are dropped, freeing their numbers;
  - keys in `live` but not `previous` are bound at the **lowest free** logical number, with
    `createdByWinspace = false`;
  - the result is a function of its inputs only — no I/O, no COM, no ordering assumptions.
- **A notification is a trigger, never the data.** The **Notification sink** implements
  `IVirtualDesktopNotification` on the **Worker's existing STA** (no second apartment, no
  marshalling). Its callbacks touch neither the map nor State — they only post to the
  Worker's message-only window. On a clean pump turn the Worker re-enumerates, calls the pure
  reconcile, installs the result, and posts `WorkspaceChanged` **only if current actually
  differs**.
- **Split by layer.** Map maintenance (`Created`, `Destroyed`, `Moved`, `NameChanged`) stays
  bridge-internal per ADR-0003. Exactly **one** new Event reaches the Reducer:
  `WorkspaceChanged{logical}`.
- **`WorkspaceChanged` is the sole writer of `State.currentWorkspace`.** Delete the three
  writers that recorded intent: the speculative `next.currentWorkspace` in `WorkspaceSwitch`
  and in `MoveToWorkspace`'s follow branch, and the bridge's assignment inside `doSwitch`.
  The notification fires for winspace's **own** switches too, so one path serves both — the
  third instance of the emit-intent / receive-reality pattern that `ResolveFocus`→
  `FocusResolve` and `ResolveDistribute`→`DistributeResolve` already use.
- **Degrade path.** If the notification interface cannot be acquired, log it
  ([ADR-0004](../adr/0004-win32-error-handling.md)) and invoke the **same** reconcile at the
  top of each workspace operation. No second code path — only a different trigger.
- **`reduce(WorkspaceChanged)` emits no Effects.** It updates State and stops. Emitting a
  switch here would loop.

## Testing Decisions

A good test asserts what the user can observe — which desktops are addressable, what number
each carries, and where winspace thinks it is — never how the notification was delivered.
Delivery is exactly the part that is untestable off a live session, which is why the design
pushes all policy out of the callback.

- **Seam 1 — the existing pure reducer seam** (`src/winspace_test.cpp`, Catch2). Covers the
  entire reconcile policy, because `DesktopKey` is opaque and `reconcile` is pure:
  - a stable set reconciles to itself, numbers and Provenance unchanged;
  - a new key takes the lowest free number, not `max+1`;
  - a destroyed key frees its number and the next new key reuses it;
  - a winspace-created binding keeps `createdByWinspace == true` across reconciles (the
    property PRD 0024 depends on);
  - reconcile is idempotent — running it twice on the same live set changes nothing;
  - reordering the live key list does not change the result (no positional assumptions).
  - Plus: `reduce(WorkspaceChanged)` updates `currentWorkspace` and emits **no** Effect; and
    `WorkspaceSwitch` / `MoveToWorkspace` **no longer** write `currentWorkspace`. The
    existing `[reducer]` cases that assert those speculative writes must be **updated, not
    deleted** — they become the pin on the new single-writer rule.
- **Seam 2 — one new Smoke seam** (`scripts/guest/WorkspaceReconcile.Tests.ps1`, Pester),
  because live notification wiring is a **Smoke seam** by definition: only the running binary
  can exhibit it. Desktops are not Displays, so the single-display VM is no obstacle.
  - Create a desktop externally, then assert `workspace N` lands on it rather than appending
    (OS Oracle: desktop count and active desktop from the Virtual Desktop registry state).
  - Switch desktops externally, then assert the next winspace switch behaves correctly.
  - Per [ADR-0005](../adr/0005-vm-seam-test-harness.md), the seam must be verified **red**
    before the feature and green after, or it proves nothing.
- **Prior art:** `WorkspaceSwitch.Tests.ps1` for the OS Oracle pattern, and PRD 0022's
  `retry-after-view-miss` for a seam that forces a race deliberately.
- **Explicitly not tested:** reentrancy during an outbound COM call. It is designed out (the
  callback only posts), not asserted — there is no seam that can provoke it deterministically.
  This is a known coverage gap and is recorded in the ADR rather than papered over.

## Out of Scope

- **Reaping** — destroying empty desktops *during* a session. Still deferred; it needs
  per-Workspace occupancy tracking the hook does not maintain. Quit-time cleanup is
  [PRD 0024](0024-quit-cleanup.md) and is not blocked by that gap.
- Desktop **names** (`NameChanged` is consumed as a reconcile trigger only; winspace does not
  read or set desktop names).
- Per-Display Virtual Desktops (a Windows 11 feature winspace does not model — all Displays
  switch together, per `CONTEXT.md` → **Display**).
- Reacting to an external switch with any *behavior* beyond recording it.

## Further Notes

The cost taken knowingly here is a **second undocumented COM vtable** that must be re-pinned
per Windows build. The degrade path is what makes that survivable: a broken vtable costs
liveness, not function. Reviewers should treat the degrade path as part of the feature, not
as error handling bolted on — it is the reason this option was chosen over reconciling purely
on demand.
