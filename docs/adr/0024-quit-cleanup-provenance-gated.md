# 24. Quit cleanup: destroy every winspace-created desktop, unconditionally

**Status:** Accepted (2026-07-27)

## Context

winspace creates Virtual Desktops on demand ([ADR-0003](0003-sparse-virtual-workspace-model.md))
and destroys none. Quit is `PostQuitMessage(0)` and nothing else (`win32.cpp:2119`), so a
session leaves behind every desktop it materialised.

`CONTEXT.md` defers **Reaping** because it "needs per-Workspace occupancy tracking the hook
does not yet maintain." That blocker does **not** apply at quit: a one-shot question needs
no continuous tracking, and `IVirtualDesktopManager::GetWindowDesktopId` — the *public*,
documented, stable API — answers it in one `EnumWindows` pass. Quit-time cleanup is
unblocked in a way live reaping is not.

Two facts shape the decision. `RemoveDesktop` already exists in the pinned vtable
(`win32.cpp:1347`) and takes a **fallback desktop**: windows on a removed desktop *migrate*
to it and are never destroyed. And [ADR-0023](0023-reconcile-external-desktop-changes-via-notifications.md)
supplies **Provenance**, without which "clean up all workspaces" would also delete desktops
the user made by hand in Task View.

## Decision

On exit, switch to the **Home desktop** (the one active at Adoption) and remove every
desktop whose Provenance is `createdByWinspace`, with home as the `RemoveDesktop` fallback.
Foreign desktops survive untouched. The session ends with exactly the desktops that existed
before winspace started.

Cleanup is **unconditional, not empty-only**, and it hangs off a new Effect:
`reduce(Quit)` emits the ordered pair `{CleanupWorkspaces{}, Exit{}}`.

## Considered options

- **Empty-only** (reap just the created desktops carrying no windows). Rejected. Its
  caution is aimed at a risk that is not there — nothing is lost either way, since windows
  migrate to the fallback. Its actual cost is that it makes shutdown **unpredictable**:
  whether quit tidies up would depend on where your windows happened to be sitting, which
  is the worst property a shutdown path can have. It also still accumulates the junk
  desktops ADR-0003 opens by complaining about, needing only one parked window to do it.
- **Do nothing** (status quo). Rejected — the accumulation is the complaint.
- **Cleanup in Worker teardown** rather than as an Effect. Rejected: it covers no more
  cases (a crash posts no `Quit` either way) and hides a significant user-visible behaviour
  in a destructor where no reducer test can see it.
- **A `cleanup_on_exit` setting.** Rejected. `CONTEXT.md`'s Settings grammar has been
  deliberately whittled to a single surviving option and this does not clear that bar;
  Provenance already provides the safety the knob would be protecting.

## Consequences

- **Windows consolidate onto home**, and that is the point rather than a regret. Once
  winspace exits the user no longer has `workspace N` binds, so a window parked on desktop
  7 does not become neutral — it becomes *harder to reach* than it was a second earlier,
  recoverable only by walking Win+Ctrl+→ six times. Stranding windows on desktops the user
  can no longer address is the destructive option; consolidating is the kind one.
- **Every `Quit`-Event path inherits cleanup for free**, because all three converge on
  `reduce`: the `quit` bind, the `Control::Quit` message from `winspace uninstall` or a
  second instance (`win32.cpp:2094`), and the `WINSPACE_SELFTEST_QUIT` hook (`:2501`). The
  `uninstall` case matters most — uninstalling and leaving eight desktops behind would be
  the worst version of this feature. The selftest case means the VM harness gets a **Smoke
  seam** for cleanup with no extra plumbing.
- **A kill or a crash cleans up nothing.** Unavoidable and accepted.
- **The switch to home must complete before any removal**, or cleanup removes the desktop
  it is standing on. This ordering constraint lives inside the Effect, not in `reduce` —
  and it cannot lean on `currentWorkspace`, which ADR-0023 makes eventually consistent.
- The ordered-pair emission follows existing precedent: `MoveToWorkspace` already emits
  `MoveForegroundWindowToWorkspace` before `SwitchToWorkspace` precisely because order
  matters (`winspace.cpp:764-770`).
