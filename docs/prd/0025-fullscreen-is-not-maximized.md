# PRD 0025 — Fullscreen is not maximized: restore focus on a taskbar-less Display

**Status:** Ready for agent
**ADR:** none (small, reversible predicate fix — the gap was the glossary, now filled by
`CONTEXT.md` → **Fullscreen**)
**Lands:** first, independent of PRD 0023/0024/0026

## Problem Statement

Spatial focus does not work between Displays. On a two-monitor setup where the taskbar is
shown only on the primary Display, the user cannot move focus **onto** the second Display,
and once focused there cannot move focus **within** it either. Both directions fail. The
second Display is, for focus purposes, invisible.

The user experiences this as "`focus left`/`focus right` just does nothing" — silently, with
no diagnostic, on the exact setup a window manager exists to serve.

## Solution

Stop conflating **Fullscreen** with **maximized**.

The **Eligibility gate** rejects Fullscreen windows. Fullscreen is currently decided by
geometry alone — a window whose rect covers its Display's full bounds (`rcMonitor`). On a
Display with **no taskbar the work area *is* the monitor bounds**, so a maximized window
there covers `rcMonitor` exactly. Since ADR-0020 **Distribute** maximizes every Eligible
window by default, every window on a taskbar-less Display is therefore classified Fullscreen
→ Ineligible → never a **Candidate**. On the primary Display the taskbar shrinks the work
area enough that a maximized window falls short of `rcMonitor`, so it stays Eligible — which
is why the bug looks monitor-specific rather than like a broken predicate.

Geometry cannot distinguish the two cases. **Window state can**: a maximized window is
`IsZoomed`; a fullscreen one is not. Fullscreen becomes *covers `rcMonitor` **and** is not
OS-maximized*.

## User Stories

1. As a user with two Displays, I want `focus right` to move focus onto a window on my
   second Display, so that I can navigate my whole desk with the keyboard.
2. As a user with two Displays, I want `focus left` to move focus back off the second
   Display, so that navigation is symmetric and I am never stranded.
3. As a user whose second Display carries several windows, I want `focus up`/`focus down`
   to move between them, so that focus works *within* a Display and not only between them.
4. As a user who has turned off "show my taskbar on all displays", I want winspace to behave
   identically to a user who left it on, so that a Windows appearance setting does not
   silently change my window manager's behavior.
5. As a user whose windows were auto-placed by **Distribute**, I want those windows to remain
   focus **Candidates**, so that the feature that placed them does not also hide them.
6. As a user, I want a **maximized** window to be a normal managed window in every respect,
   so that maximizing is never an accidental way to opt a window out of winspace.
7. As a user watching a video fullscreen, I want that window skipped by focus, so that the
   original protection the gate provided is preserved.
8. As a user launching an app that opens **fullscreen**, I want **Distribute** to leave it
   alone rather than maximizing it out of fullscreen, so that the ADR-0020 protection that
   depends on this same fact still holds.
9. As a user on a single Display with the taskbar auto-hidden, I want maximized windows to
   stay Eligible, so that the same bug does not reappear in single-monitor form.
10. As a user who manually dragged a window to exactly cover a taskbar-less Display, I accept
    that it reads as Fullscreen, so that the rule stays simple and explainable.
11. As a maintainer, I want the Fullscreen rule expressed as a **pure** predicate over probed
    facts, so that the monitor-crossing case is testable without a two-monitor machine.
12. As a maintainer, I want `CONTEXT.md` to define Fullscreen precisely, so that the
    ambiguity that produced this bug cannot silently reappear.

## Implementation Decisions

- **Fullscreen stops being a probed *verdict* and becomes a derived *predicate*.** This is
  the seam-level change and the reason the fix is testable at all. Per
  [ADR-0006](../adr/0006-window-tracking-probe-decide-seam.md) the **Probe** gathers facts
  and the **Reducer** decides; today `probeWindow` violates that by deciding Fullscreen in
  the adapter, where no unit test can reach it.
- **`WindowAttrs` gains two probed facts** — the window's Display bounds (`rcMonitor`, as a
  `Rect`) and `zoomed` (`IsZoomed`) — and **loses** the `fullscreen` verdict. The adapter
  reads three raw facts (`rect`, monitor bounds, zoomed) and asserts nothing.
- **`isEligible` derives Fullscreen** from those facts: covers-monitor **AND NOT** zoomed.
  The gate's shape is unchanged — still the AND of probed facts — so the existing
  `[reducer]` eligibility test extends rather than being rewritten.
- **Nothing else changes.** `resolveFocus` is untouched; ADR-0008's rule was always correct
  and monitor-agnostic. **Distribute**'s `Appeared` protection and the `tile` sweep inherit
  the corrected verdict automatically because both already route through `isEligible`.
- **The `!zoomed` half is load-bearing and must be commented as such** at the predicate, or
  a future reader will "simplify" it away and reintroduce the bug.

## Testing Decisions

A good test here asserts **externally observable focus behavior over synthetic rects** — it
never inspects how the Probe was implemented. The bug is a policy error dressed as an
adapter detail, so pulling it into the Reducer is what makes a real test possible.

- **Seam: the existing pure reducer seam** (`src/winspace_test.cpp`, Catch2). **No new seam.**
- **Prior art:** `"isEligible is the AND of the probed facts — each condition alone flips the
  verdict"` (`[reducer]`) and `"traversal crosses monitors via virtual-screen coordinates, no
  boundary special case"` (`[reducer][focus]`) — the second already covers cross-monitor
  traversal and would have caught this had Fullscreen been reducer-side.
- **New cases**, all `[reducer]` / `[reducer][focus]`:
  - a window covering `rcMonitor` **and** zoomed is Eligible (the regression test);
  - a window covering `rcMonitor` and **not** zoomed is Ineligible;
  - a zoomed window *not* covering `rcMonitor` is Eligible (ordinary maximized-with-taskbar);
  - focus resolves onto a maximized window on a second Display whose work area equals its
    monitor bounds — the reported bug, expressed as rects;
  - focus resolves *between* two maximized windows on that same second Display.
- **No Smoke seam.** ADR-0008 already records that the VM is **single-display**, so
  cross-Display behavior is covered at the reducer seam by design, not by omission. Adding a
  guest test here would assert nothing the reducer tests do not.

## Out of Scope

- Loosening `WS_THICKFRAME` for focus candidacy (`CONTEXT.md` flags it as a future
  candidate).
- Any change to `resolveFocus`'s ranking rule (ADR-0008 stands).
- Detecting *exclusive* fullscreen (DirectX) distinctly from borderless — the style gate
  already excludes both.
- Making the VM harness multi-display.

## Further Notes

This lands first and alone. It touches none of PRD 0023/0024/0026 and is the only one of the
four that fixes a defect rather than adding behavior.

The accepted false positive — a window manually dragged and sized to exactly cover a
taskbar-less Display reads as Fullscreen — is recorded in `CONTEXT.md` deliberately, so that
a future bug report is recognized as a known edge rather than re-diagnosed from scratch.
