# The launcher is launch-only; workspace placement stays with `windowrule`

**Status:** Accepted (2026-07-11 — reverses the PID-match design in the 08 issue text
and `DESIGN.md §6`).

The 08 launcher slice (`exec` / `exec-once`) was specified to launch an app via
`CreateProcess`, then match the app's **first window by PID** and assign it to a target
Workspace. This ADR records why that PID-match placement path was dropped before it was
built, and how the launcher reuses machinery that already landed instead.

## Context

By the time 08 was picked up, [ADR-0009](0009-window-rules-place-once-state.md) and PRD 07
had already landed a full app→Workspace placement path: a `windowrule` matches a window by
`exe` / `class` / `title` on the `Appeared` edge and emits `MoveWindowToWorkspace{id,
logical}`, bounded by the place-once `placed` set. Placement by exe therefore already exists,
is tested through the pure seam, and works for foreign windows via the internal
`MoveViewToDesktop` ([ADR-0010](0010-move-to-workspace-internal-move.md)).

The issue's PID-match placement would have added, on top of that: a PID threaded onto every
`Appeared`, a pending-launch map in `State` keyed by PID, a `CreateProcess`→PID round-trip
Event to fold the spawned PID into that map, and timeout/failure handling for a launched PID
that never produces a window. That is a second, parallel matching subsystem next to the one
that already places windows.

The stated justification for PID matching was "more reliable than exe/title" and placing
*this specific instance*. Both are weak in practice:

- **PID matching is the *less* reliable path for the apps that matter.** Browsers, Electron
  apps, and UWP apps launch through a broker/launcher process and surface their window under
  a **different** PID than the one `CreateProcess` returned — so PID matching silently fails
  exactly where an `exe` `windowrule` still succeeds.
- **The only thing PID matching buys over an exe rule** is disambiguating two instances of
  the same exe onto different Workspaces from a single launch — a corner case the launcher
  does not need for its actual job ("I just started this app; put its window on N").

## Decision

- `exec` / `exec-once` are **launch-only**. An entry is a verbatim command line and nothing
  else — no target-Workspace argument, no PID, no window matching.
- **All workspace placement stays with `windowrule`** (match by `exe`, landed in PRD 07). A
  user who wants a launched app on Workspace N writes an `exec-once` line to start it and a
  `windowrule = workspace N, exe:…` line to place it. Two orthogonal, already-understood
  directives instead of one fused stateful one.
- Consequently **nothing about PID matching is built**: `Appeared` does not carry a PID,
  `State` gains no pending-launch map, and there is no `CreateProcess`→PID round-trip or
  launch-timeout logic.
- Launching routes through the pure Reducer (not a Worker-ctor shortcut) so it is testable
  through the seam and so the `exec` vs `exec-once` distinction is pure logic. The Reducer
  holds the entries in `State` as a single tagged `std::vector<ExecEntry>` (`{command, once}`,
  source order) behind a `std::shared_ptr<const …>` — the same O(1)-copy handle
  [ADR-0009](0009-window-rules-place-once-state.md) uses for `rules`. A `Started{}` Event
  (posted by the spine once the Worker HWND exists) makes the Reducer emit an ordered batch of
  `LaunchApp{command}` Effects for **every** entry; the Worker executes each with
  `CreateProcessW` and detaches (close both handles, track nothing).
- **exec-once idempotency is stateless** — it falls out of *which* Event fires, not a
  remembered flag. `Started{}` emits every entry; a `Reloaded{}` Event emits only the
  `once == false` entries. No session bool, no process-list scan, no "is it still running"
  check (this matches Hyprland: exec-once runs only at the initial start, and even a
  newly-added exec-once entry does not run until the next start). In 08 the `Reloaded{}`
  handler and its tests exist, but **only `Started{}` is triggered** — the reload event source
  (file watch / `reload` dispatcher) lands with issue 09.

## Considered Options

- **PID-match placement (the issue / `DESIGN.md §6` as written).** *Rejected* — a whole
  parallel matching subsystem (PID on `Appeared`, pending map, spawn round-trip, timeouts)
  that is *less* reliable than the exe `windowrule` already shipped, precisely for the
  multi-process apps it would most need to handle.
- **Launch-only + placement via `windowrule`.** *Chosen* — reuses landed, tested machinery;
  deletes the hardest and least-reliable part of the slice; composes two orthogonal
  directives the user already has.
- **Keep a target-Workspace argument on `exec` but place it with an internally-synthesised
  exe rule.** *Rejected* — sugar that hides a `windowrule` behind a second syntax; more
  surface, no new capability, and it re-introduces the "which instance" ambiguity for no gain.

## Consequences

- The 08 issue text and `DESIGN.md §6` ("assigns the app's first window by PID match") are
  **superseded by this ADR**; a reader who finds that phrasing should read this file. The
  issue is rewritten to the launch-only shape.
- Placing a launched app requires the user to pair an `exec` line with a `windowrule` line.
  This is a deliberate composition, documented in the seeded default config.
- The launcher cannot place two instances of the same exe on different Workspaces from one
  launch. Accepted: it is not a goal, and no reliable mechanism (PID included) delivers it for
  multi-process apps anyway.
- The window side of `State` grows only by the exec entry list (see ADR-0012); it gains **no**
  per-window launch state, so ADR-0007's geometry ban and ADR-0009's bounded-window-state
  posture are both untouched by the launcher.
