# 24. Quit cleanup: destroy every winspace-created desktop, unconditionally

**Status:** Accepted (2026-07-27); amended (2026-07-28) — see *Amendment: the Cleanup anchor*

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

## Amendment (2026-07-28): the Cleanup anchor

### Context

The decision above says "switch to the **Home desktop** … with home as the `RemoveDesktop`
fallback", which quietly gives `m_home` **two** jobs that only look like one because, until
now, home always survived:

- **Home desktop** — *the desktop active at Adoption; where winspace found the user.* A fact
  about the past, and the polite place to leave someone.
- **The cleanup anchor** — the desktop cleanup switches to and migrates windows onto. Not a
  preference but a *requirement*: it must **survive the cleanup**, i.e. not be in `doomed`.

They separate the moment home can vanish — the user destroying it by hand in Task View
mid-session. `cleanupCreatedDesktops` already degrades safely there (`win32.cpp:1813`: log and
return), but the consequence is that quit then leaves **every** winspace-created desktop
standing, which is the accumulation this ADR exists to end.

This surfaced while designing
[ADR-0025](0025-vd-bridge-liveness-reacquire-on-shell-loss.md), which makes `m_home` persist
across reconnects. Persistence is not what breaks it — manual destruction is, and that was
already true.

### Decision

Name the second job and derive it. `m_home` keeps its meaning and is **never re-seeded**. The
anchor is chosen at quit time by a **pure core function over the bindings**, preferring, in
order:

1. the **Home desktop**, if it still names a live desktop;
2. otherwise the lowest-logical **foreign** desktop;
3. otherwise the lowest-logical desktop.

### Considered options

- **Re-seed `m_home` when it vanishes.** Same runtime behaviour, but it fuses the two jobs
  permanently and makes Home mean *"where winspace found the user, or wherever we substituted
  later"* — no longer a fact about anything. It also mutates history to satisfy a need that
  exists for a few milliseconds at quit, and it can only be exercised through the VM harness.
- **Re-seed to the currently active desktop.** Rejected: arbitrary, and likely to be one of
  winspace's own — see below.
- **Re-seed to logical 1.** Rejected: logical 1 is not guaranteed foreign, so it walks into
  the same trap.

### Consequences

- **Foreign-first is load-bearing, not a preference.** Cleanup never removes the ground it
  stands on (`win32.cpp:1836`), so anchoring on a `createdByWinspace` desktop would exempt one
  of winspace's own from removal — the exact accumulation this ADR forbids. Preferring foreign
  makes that exemption reachable only in the genuine last resort where *every* desktop is
  winspace's, where keeping one is unavoidable.
- **The preference order becomes unit-testable.** As a total function over `DesktopKey`s it
  sits beside `reconcile` and `desktopsToCleanup` in core, with no COM in it — the third pure
  policy over the bindings.
- **The glossary grows by one term** (**Cleanup anchor**), and **Home desktop** must stop
  implying it is the migration target. The concept being unnamed is why it was wrong.
- **Still no cleanup when nothing survives.** With no bindings at all there is no anchor and
  cleanup is skipped, exactly as today.
