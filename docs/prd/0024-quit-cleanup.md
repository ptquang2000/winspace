# PRD 0024 — Quit cleanup: leave the session as winspace found it

**Status:** Ready for agent
**ADR:** [ADR-0024](../adr/0024-quit-cleanup-provenance-gated.md)
**Lands:** third — **depends on [PRD 0023](0023-reconcile-external-desktop-changes.md)** for
**Provenance**

## Problem Statement

winspace creates Virtual Desktops on demand and destroys none. Quitting is
`PostQuitMessage(0)` and nothing else. So every session leaves its desktops behind: the user
quits winspace and is left standing in a session littered with six empty desktops they now
have to clean up by hand, one Win+Ctrl+F4 at a time.

`winspace uninstall` is the sharpest form of this — the user uninstalls the program and the
program's debris stays.

## Solution

On exit, switch to the **Home desktop** — the one that was active when winspace started — and
destroy every desktop winspace itself created. **Foreign desktops** are untouched. The
session ends with exactly the desktops that existed before winspace started.

Windows are never lost: `RemoveDesktop` takes a **fallback** desktop and migrates them to it.
Cleanup can only consolidate windows onto home, never destroy them.

## User Stories

1. As a user, I want quitting winspace to remove the desktops it created, so that my session
   is left the way I found it.
2. As a user, I want desktops I created by hand to survive the quit, so that "clean up all
   workspaces" never means "delete my stuff".
3. As a user, I want to end up on the desktop I started on, so that quitting does not also
   teleport me somewhere unfamiliar.
4. As a user with windows open on a winspace-created desktop, I want those windows to survive
   and follow me to home, so that quitting a window manager never closes my work.
5. As a user, I want those windows consolidated somewhere I can actually reach, so that I am
   not left hunting for a browser stranded on a desktop I can no longer address by keybind.
6. As a user, I want cleanup to happen whether or not the created desktops are empty, so that
   whether quit tidies up does not depend on where my windows happened to be sitting.
7. As a user running `winspace uninstall`, I want the desktops removed as part of
   uninstalling, so that uninstalling actually uninstalls.
8. As a user stopping a running winspace by launching a second instance, I want the same
   cleanup, so that every way of stopping winspace behaves identically.
9. As a user, I accept that killing winspace from Task Manager cleans up nothing, so that the
   normal paths can stay simple.
10. As a user whose cleanup partly fails, I want winspace to still exit, so that a COM error
    can never leave me with a window manager I cannot quit.
11. As a user, I want any cleanup failure logged, so that leftover desktops are explainable.
12. As a maintainer, I want cleanup expressed as an Effect emitted by `reduce(Quit)`, so that
    a user-visible behavior is asserted at the pure seam instead of hidden in a destructor.
13. As a maintainer, I want the switch to home to complete before any removal, so that
    cleanup never removes the desktop it is standing on.
14. As a maintainer, I want the set of desktops to remove chosen by a pure function over
    Provenance, so that the "never delete a foreign desktop" guarantee is unit-tested.

## Implementation Decisions

- **A new Effect.** `reduce(Quit)` emits the ordered pair `{CleanupWorkspaces{}, Exit{}}` —
  cleanup first. Every `Quit`-Event path inherits it: the `quit` bind, the `Control::Quit`
  message from `winspace uninstall` or a second instance, and the `WINSPACE_SELFTEST_QUIT`
  hook. Ordered emission where order matters is existing precedent — `MoveToWorkspace`
  already emits its move before its switch.
- **Home desktop.** The desktop active at **Adoption**, captured once at startup and held in
  the bridge. Not "logical 1" — home is wherever the user happened to be.
- **A pure selection.** `desktopsToCleanup(bindings) → keys` filters on
  `createdByWinspace == true`. Pure, over PRD 0023's opaque `DesktopKey`, so the safety
  guarantee is unit-testable.
- **Executing the Effect, in order:** switch to home and **let that complete**; then remove
  each selected desktop via `RemoveDesktop(desktop, home)`; then let `Exit` run. The ordering
  constraint lives inside the Effect, **not** in `reduce`, and **must not** consult
  `currentWorkspace` — PRD 0023 makes that field eventually consistent, so it will still name
  the old Workspace at this moment.
- **Unconditional, not empty-only.** No occupancy check, no `GetWindowDesktopId` sweep. The
  emptiness question is not asked, so the answer cannot make shutdown unpredictable.
- **Degrade and log** ([ADR-0004](../adr/0004-win32-error-handling.md)). A failed switch or a
  failed removal is logged and skipped; `Exit` always runs. Quitting must not be blockable.
- **No setting.** No `cleanup_on_exit`. Provenance already provides the safety a knob would
  protect, and the Settings grammar is deliberately down to one surviving option.

## Testing Decisions

A good test asserts the **desktops that exist after the process is gone** — an OS-level fact,
verified by an independent Oracle, not by anything winspace reports about itself. That is the
whole point: a cleanup feature that only its own logs can confirm is not confirmed.

- **Seam 1 — the existing pure reducer seam** (`src/winspace_test.cpp`, Catch2):
  - `reduce(Quit)` emits exactly `{CleanupWorkspaces, Exit}`, **in that order**, and clears
    `running`. The existing `"a Quit Event emits Exit and clears running"` case is **updated**
    to pin the pair — it becomes the ordering test.
  - `desktopsToCleanup` returns only `createdByWinspace` bindings;
  - a bindings set with no winspace-created entries yields an empty selection (quit after a
    session that created nothing removes nothing);
  - a mixed set returns exactly the created subset and never a foreign key — the data-loss
    guarantee, stated as an assertion.
- **Seam 2 — one new Smoke seam** (`scripts/guest/QuitCleanup.Tests.ps1`, Pester). Cleanup is
  a **Smoke seam** by definition: only the running binary can exhibit it, and the OS desktop
  set is the Oracle.
  - Start winspace with a known desktop count; create desktops via winspace; quit; assert the
    OS desktop count and identities are back to the starting set.
  - Create a desktop **externally**, let winspace bind it, quit, assert **it survives** — the
    Provenance guarantee, live. This is the case worth the most; it is the one whose failure
    is data loss.
  - Park a window on a winspace-created desktop, quit, assert the window still **exists** and
    is on home.
  - Assert cleanup runs on the `winspace uninstall` path, not only the `quit` bind.
  - Verified **red** before the feature and green after, per
    [ADR-0005](../adr/0005-vm-seam-test-harness.md).
- **Prior art:** `WorkspaceSwitch.Tests.ps1` and `Installation.Tests.ps1` — the latter already
  drives the `uninstall` path and is the natural neighbour for the uninstall case.
- The selftest quit hook already posts a `Quit` Event, so it picks cleanup up with no extra
  plumbing — the harness gets the seam for free.

## Out of Scope

- **Reaping** — live, empty-only destruction during a session. Still deferred (needs
  occupancy tracking); explicitly a different feature from this one.
- Cleanup on crash, kill, or logoff. No `Quit` Event is posted, so nothing runs. Accepted.
- Restoring which windows were on which desktop across a restart — winspace persists no
  State by design.
- Restoring window **geometry** to its pre-winspace values. Placement is place-once and never
  recorded; unwinding it is not possible and not wanted.

## Further Notes

The consolidation of windows onto home is the intended behavior, not a regrettable
side-effect, and reviewers should not "improve" it into an empty-only reap. Once winspace
exits the user no longer has `workspace N` binds, so a window left on desktop 7 becomes
*harder* to reach than it was a second earlier. Stranding is the destructive option;
consolidating is the kind one. The reasoning is recorded in ADR-0024 so this does not get
relitigated as a bug report.
