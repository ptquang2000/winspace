# Single-instance Primary with a cross-process control channel, driven by Scoop lifecycle hooks

**Status:** Accepted (2026-07-19 — builds on ADR-0013 autostart and
[ADR-0017](0017-distribution-via-self-bucketed-scoop-package.md) Scoop distribution).

Packaging winspace for Scoop ([ADR-0017](0017-distribution-via-self-bucketed-scoop-package.md))
surfaces two OS realities: a running `winspace.exe` **locks its own image**, so Scoop's
file-swap on update/uninstall fails while it runs; and the autostart Logon task (ADR-0013)
bakes an **absolute** exe path that Scoop's versioned-directory layout moves on every update.
We make winspace **single-instance** (the one live process is the **Primary**), give it a
**cross-process control channel** for `sync-autostart` / `remove-autostart` / `quit` requests,
and add two headless subcommands — **`winspace install`** and **`winspace uninstall`** — that
the Scoop manifest's lifecycle hooks invoke around the file swap.

## Context

`winspace` (no args) is the WM: it owns the global hotkeys (`RegisterHotKey`), the COM Virtual
Desktop bridge, and the OS autostart state. Two of those are **exclusive OS resources** — two
WMs both binding `Alt+<n>` and both driving Virtual Desktops would collide destructively — so
running two instances was never sensible; nothing enforced it only because nothing needed to.

Scoop makes that enforcement, plus a control path, load-bearing:

1. **File lock.** `scoop update` deletes the old `~\scoop\apps\winspace\<version>\` dir and
   `scoop uninstall` deletes the whole app; both **fail if an instance is running from it**.
   So the package must be able to **stop** a running instance before the file swap.
2. **Moved binary vs. baked path.** ADR-0013's Logon task stores an absolute path and
   self-heals only by being re-stamped on the next `Started`/`Reloaded`. After `scoop update`
   the versioned dir moves; unless winspace re-stamps before the next logon, autostart fires a
   stale, deleted path and silently dead-ends.
3. **Hooks may run with nothing live.** Scoop's `post_install` fires on a fresh box where the
   WM isn't running (we deliberately do not auto-launch, ADR-0017); `pre_uninstall` may fire
   with no instance running either. So the setup/teardown work cannot *require* a running
   process.

winspace already has the raw material: the Worker owns a **message-only (`HWND_MESSAGE`)
window** that today receives in-process `Event*` posts from the Hotkey/Hook threads, and a
clean shutdown path (the `Shift+Q` quit, with RAII teardown of the Hotkey/Hook threads).

## Decision

**Single-instance Primary.** `winspace` (no args) is guarded single-instance: a named
mutex plus discovery of the existing message-only window. A second no-args launch detects the
live **Primary** and exits rather than contend for hotkeys and desktops.

**Control channel.** The Worker's existing message-only window is extended to accept
**cross-process** requests (`WM_COPYDATA` / a registered message with a **scalar** payload —
never the in-process `Event*` pointer). Three **Control messages**: `sync-autostart`,
`remove-autostart`, `quit`.

**Two headless subcommands**, parsed in `wWinMain` *before* `runApp()` (windowless, no
console — Scoop reads exit codes, so nothing is printed). Each is **hybrid**: message the
Primary if one is running, else do the same work directly in-process, then exit. They
share **one** task-mutation implementation — ADR-0013's `SyncAutostart` logic — reached by
either entry path.

- **`winspace install`** — make OS autostart match the config's `start_at_login`. It
  **respects the flag as the single source of truth and never enables autostart by itself**
  (a fresh Scoop install does not start seizing logon hooks). Running/none → message
  `sync-autostart` / do the headless one-shot sync.
- **`winspace uninstall`** — remove the `\winspace\<user>` Logon task **unconditionally**, and
  stop the running Primary so the exe unlocks. Running/none → message
  `remove-autostart` + `quit` / remove the task directly.

**Graceful quit.** `quit` routes into the **existing** clean-shutdown path (the one `Shift+Q`
uses) — not `TerminateProcess`. A short timeout with a **forced-kill fallback** guards against
a wedged Primary blocking `scoop update` forever.

**Scoop lifecycle hooks** tie it together:

- **`pre_install`** → graceful-quit any running Primary (releases the file lock). No-op on
  fresh install; the stop on update.
- **`post_install`** → `winspace install` (re-stamps autostart to the new location if the flag
  is on — the update-path self-heal — without launching the WM). **No restart:** on update the
  instance is *stopped and left stopped*; the user restarts it, and if `start_at_login` is on
  the next logon relaunches it via the (now-fresh) Logon task.
- **`pre_uninstall`** → `winspace uninstall` (removes the task + quits the instance so the exe
  can be deleted).

## Considered Options

- **Hybrid commands (message-if-running, else do-it-directly).** *Chosen.* Preserves a single
  writer of autostart state whenever it matters (a race only exists while a WM runs; when none
  runs, a direct write is race-free anyway) **and** keeps the Scoop hooks functional on a box
  with nothing running.
- **Message-only commands (always route through the Primary).** *Rejected.* Cleaner in
  the abstract, but `post_install`/`pre_uninstall` frequently run with **no Primary
  live** — the commands would send to nobody, so autostart never syncs and the Logon task is
  never cleaned up, reintroducing the orphaned-task bug and defeating the package.
- **`TerminateProcess` / `taskkill` for the stop.** *Rejected* as the primary path. Bypasses
  the RAII teardown (hotkeys/hooks may not unregister cleanly) and is exactly the OS-level
  force this codebase avoids. Kept only as the timeout fallback for a wedged instance.
- **Restart the WM after update (`post_install` relaunch).** *Rejected.* Would re-seize the
  global `Alt` chords as a side effect of a background package update. Stop-and-leave-stopped
  keeps update non-invasive; autostart (if enabled) brings it back at the next logon.
- **Duplicate the task-COM logic in the manifest's PowerShell hooks.** *Rejected.* Brittle
  re-implementation of ADR-0013's `ITaskService` interaction; the subcommands reuse the real
  one instead.
- **A boot-time / cross-session mechanism to dodge the file lock.** *Rejected* and excluded by
  ADR-0013: winspace is strictly per-user interactive; the lock is solved by asking the
  running process to exit, not by escaping the session.

## Consequences

- winspace gains a small **out-of-band control surface** distinct from the Reducer's Event
  stream: Control messages mutate OS artifacts (the Logon task) or lifecycle (quit), and do
  **not** flow through `reduce`. This is a deliberate seam — packaging/lifecycle concerns, not
  window-management behavior — and must stay that side of the line.
- The autostart path is now self-healing across `scoop update` via `post_install`, closing the
  moved-binary gap without the WM needing to run between update and logon.
- `scoop uninstall` no longer orphans the Logon task, and neither update nor uninstall can be
  blocked by the running-process file lock.
- Single-instance is now a **guarantee**, not an assumption: exactly one Primary owns the
  hotkeys, the Virtual Desktop bridge, and the autostart state per session.
