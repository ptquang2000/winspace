# 5. Automating the I/O-layer smoke seams in a VM

**Status:** Accepted (2026-07-04)

## Context

winspace's core (Reducer, config parser) is unit-tested in a `windows.h`-free TU.
Everything below the seam — `RegisterHotKey`, the COM Virtual Desktop bridge, adoption,
create-on-demand, clean quit, the variant diagnostic, degrade-don't-crash — is
**not unit-testable** (ADR-0004; slice-11 records "there is no new test seam" for io).
Its definition of done is a **manual smoke script**: a human presses `$mod+N` on a live
24H2 desktop and eyeballs the result. That gate is real but unrepeatable, unattended-hostile,
and mutates the tester's own session (it creates and switches OS Virtual Desktops).

We want to run those smoke steps unattended, in a throwaway VM, without a human at the
keyboard. The available lever is [`vmctl`](../../../vmctl) (VMware Workstation wrapper).
The obvious approach — have the host inject the hotkey via `vmctl MKS.sendKeyEvent` — is
the one that does not work reliably: VMware's MKS input path drops the **Super/Win key** in
a chord (the host steals it) and gives no press/release atomicity, and `$mod` is `SUPER ALT`.
That is the "hard ceiling" this ADR routes around.

Several sub-decisions are entangled: how to fire a Dispatcher, how to observe the result,
how to isolate one seam from the next, and where the logic runs. Each had genuine
alternatives; this records the set we chose together.

## Decision

Build a `scripts/` harness that drives the **real running Release binary** in a VM and
asserts against **independent OS state**. The locked decisions:

- **Scope.** Automate the two *implemented* slices now — slice-01 (VD switch: windowless,
  adoption, create-on-demand, GUID-anchored stability, quit, variant diagnostic) and the
  two *runtime* seams of slice-11 (`formatError` diagnostic quality, degrade-don't-crash).
  Issues 02–10 (tiling, multi-display, rules, launcher, autostart) are designed but unbuilt,
  so they ship as `It -Skip` scaffolds — a new seam is a one-file drop-in.

- **Trigger — guest-side `SendInput`, not host-side MKS.** A PowerShell helper *inside the
  guest* P/Invokes `user32!SendInput` to synthesize a genuine `Win+Alt+N`, launched on the
  interactive desktop via `vmctl exec -it`. This exercises the **real** `RegisterHotKey →
  WM_HOTKEY → Event → Reducer` path exactly as a physical press would — the highest-fidelity
  E2E — with **zero winspace source changes**. `SendInput` can hold the Win key;
  `SendKeys`/`WScript.Shell` cannot, which is why they were never candidates. The same
  `SendInput` path also drives native VD hotkeys (`Win+Ctrl+D` / `Win+Ctrl+F4`) for setup.

- **Oracle — OS-state primary, log secondary.** Assertions consult *independent* OS state
  wherever an external observable exists:
  - **Virtual-desktop state** from the registry:
    `HKCU\...\Explorer\VirtualDesktops\VirtualDesktopIDs` (packed 16-byte GUID array → count)
    and `CurrentVirtualDesktop` (active GUID). No undocumented per-build COM IID needed —
    this is how komorebi/others read VD state. Adoption, create-on-demand, and GUID-stability
    all assert on registry deltas.
  - **Windowless** via `EnumWindows` filtered by the winspace PID + `IsWindowVisible`/
    `WS_EX_APPWINDOW`.
  - **Liveness** via `Get-Process` (clean-exit, degrade-don't-crash).

  winspace's own **stderr log** is a *secondary* Oracle, used only for seams with no external
  observable: the resolved variant IID, `formatError` text quality, and the skip-and-log
  degrade line. Because winspace is windowless, it is launched `winspace.exe 2> run.log` so
  its `lg::` output (ADR-0004) is captured. Screenshots (`vmctl MKS captureScreenshot`) are
  failure artifacts, never an assertion source. Rationale: an OS-state Oracle keeps the
  system-under-test from vouching for its own behavior.

- **Isolation — revert once, reset per-seam in-guest.** VD state is sticky (adoption is the
  whole point), so seams must not leak desktops into one another. Snapshot-revert once at
  suite start to a clean `winspace-e2e` snapshot; between seams, drive the desktop count to
  each seam's precondition with native VD hotkeys (`Win+Ctrl+F4` down to one, `Win+Ctrl+D` up
  to N) and kill winspace + clear the log. A `-Fresh` flag forces a full `vmctl snapshot
  reset` per seam for when dirty state is suspected — bulletproof but pays a boot +
  interactive-logon wait each seam.

- **Architecture — thin host orchestrator + fat interactive guest runner.** Every host→guest
  hop through `vmcli`/`vmrun` costs seconds, and `SendInput`/registry/window reads only work
  inside the interactive session. So the **host** does suite-level lifecycle only (revert,
  `cp` the exe, `push` the guest module, launch the runner, collect XML, summarize); the
  **guest runner** is one interactive PowerShell process (`exec -it`) that runs *all* seams in
  a single logged-in session and emits JUnit XML. `-Fresh` mode re-invokes the runner
  per-seam (it takes a `-Seam` filter) with reverts between.

- **Framework — Pester v5.** `Describe` per slice, `It` per seam, `It -Skip` for the 02–10
  scaffolds, custom `Should` assertions over the OS-state probes, JUnit XML copied back for
  CI. Pester v5 is baked into the snapshot.

- **GUID-anchored stability — restart-adoption proxy, not a reorder.** The literal smoke step
  ("reorder desktops in Task View, `$mod+5` → same desktop") needs
  `IVirtualDesktopManagerInternal::MoveDesktop`, an undocumented COM method whose IID changes
  per Windows build — the exact versioning pain winspace exists to encapsulate. Instead assert
  the *same guarantee* automatably: record the GUID after `$mod+5`, quit + relaunch winspace,
  press `$mod+5` again, assert the **same GUID** (adoption re-binds by GUID, never by
  position). The literal reorder variant is `It -Skip` with a pointer to the manual step.

- **Deploy — host Release build + single-file `cp`.** `build.ps1` Release on the host, then
  `vmctl cp build\winspace.exe :C:\winspace-e2e\`. **Release only:** winspace links the CRT
  dynamically (`/MD` release, `/MDd` debug); the debug CRT is non-redistributable, so a Debug
  build won't run in a clean guest. The snapshot carries the VC++ x64 redistributable.
  Build-clean and purity checks stay on the host `build.ps1` — they are compile/unit-time, not
  VM-runtime, so no compiler is dragged into the VM.

- **VM prerequisite — autologon.** After a revert the VM restarts; without autologon it sits
  at the lock screen with no interactive session and `exec -it` `SendInput` has nowhere to
  inject. The `winspace-e2e` snapshot is captured logged-in with autologon on. Documented in
  `scripts/PROVISIONING.md`.

- **Timing — poll, never sleep.** After a Trigger, poll the Oracle (registry GUID / log line)
  until the expected condition or a short timeout — fast when green, loud when not.

## Consequences

- The unattended suite exercises the genuine OS-hotkey path and asserts on OS truth, so a
  green run means the shipping artifact actually works on a real desktop — a strictly stronger
  signal than unit tests, which stop at the seam.
- winspace source is untouched: no test-only injection hook, no IPC surface. The deferred
  `hyprctl`-equivalent (DESIGN §10) is *not* pulled forward to serve testing.
- One VM prerequisite is load-bearing and non-obvious (autologon); a snapshot captured at the
  lock screen silently breaks every Trigger. Hence the runbook and the loud-failure gate.
- The GUID-stability seam tests the guarantee, not the literal gesture; a regression that only
  manifests under a Task-View reorder (and not under restart) would slip. Accepted: re-solving
  winspace's per-build IID problem in test code would rot faster than it protects.
- Adding a real seam later is a `Describe`/`It` plus an Oracle probe; unbuilt features already
  have their skipped `It` waiting.

## Alternatives rejected

- **Host-side `vmctl MKS.sendKeyEvent` chord** — simplest (no guest helper), but the flagged
  ceiling: VMware MKS is unreliable holding the Super key and lacks press/release atomicity.
- **White-box injection hook in winspace** (env-var or named-pipe IPC posting synthetic Events
  to the Worker, à la `WINSPACE_SELFTEST_QUIT`) — tests a fake path, not `RegisterHotKey`, and
  grows winspace's surface for test convenience.
- **Log-primary Oracle** — trivial to wire since winspace already emits the lines, but the
  system-under-test grades its own homework.
- **Screenshot/visual-primary Oracle** — most black-box, but brittle to theme/resolution/timing
  and heavy; kept only as a failure artifact.
- **Full snapshot revert per seam as the default** — bulletproof but a boot + logon wait per
  seam; demoted to the opt-in `-Fresh` flag.
- **Fat host loop issuing per-seam `exec` calls** — more live host control, but each seam pays
  multiple slow interactive round-trips; the fat guest runner keeps input, reads, and asserts
  in the one session where they work.
- **Hand-rolled single-file test runner** — zero guest dependency, but reinvents skip/pending
  and standard XML; Pester's module cost was accepted for the fuller framework.
- **COM `MoveDesktop` / registry / mouse-drag reorder** for GUID stability — each either
  re-solves the per-build IID problem, fights explorer's in-memory VD cache, or is
  pixel-fragile; the restart-adoption proxy tests the same guarantee cleanly.
- **Guest build from source** (VS Build Tools in the snapshot) — heavier/slower snapshot to
  fold in build-clean; those checks belong on the host, so only the Release exe crosses.
- **Static-linking (`/MT`) a test build** to drop the redist — tests a differently-linked
  artifact than ships; the VC++ redist in the snapshot is the honest cost.
