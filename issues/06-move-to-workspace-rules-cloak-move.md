# 06 — Move-to-workspace + place-once rules with cloak-move

**Labels:** `ready-for-agent`

## What to build

Assign windows to workspaces, by command and by rule, without the cross-desktop placement
flash. `movetoworkspace[silent] N` moves the focused window to desktop N. `windowrule`
app→workspace rules are evaluated at `EVENT_OBJECT_CREATE`, matched in order exe → class →
title(regex), first match wins. The move is wrapped `cloak → move → uncloak`
(`DWMWA_CLOAK` around `IVirtualDesktopManagerInternal::MoveWindowToDesktop`) so a window
placed on an inactive desktop never flashes on the current one. Placement is **place-once**:
winspace does not re-assert a rule if the user later moves the window.

## Acceptance criteria

- [ ] `movetoworkspace N` moves the focused window to desktop N; the `silent` variant does not follow it
- [ ] A `windowrule` pins a matching app to its target workspace on open
- [ ] Match order is exe → class → title(regex), first match wins
- [ ] Inactive-target placement cloaks before move and uncloaks after — no visible flash
- [ ] Rules place once; a user-moved window is not yanked back
- [ ] Rule-matcher / reducer tests assert the assignment + cloak-move effect sequence

## Blocked by

- 01
- 02
