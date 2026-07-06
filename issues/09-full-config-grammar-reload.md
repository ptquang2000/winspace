# 09 — Full config grammar + reload

**Labels:** `ready-for-agent`

## What to build

Complete the hyprland-style DSL and make it hot-reloadable. The parser handles
`key = value`, `section { ... }`, `bind = MODS, KEY, dispatcher, args`, `exec` / `exec-once`,
`windowrule` (app→workspace assignment and `ignore`), workspace rules, and general settings
(including `start_at_login`). The `reload` dispatcher re-parses and re-applies live —
re-registering hotkeys and updating rules. Invalid config produces diagnostics and leaves the
last good config active rather than crashing.

Only dispatchers that survive the no-geometry model (see
[ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md)) are valid `bind` targets:
`workspace`, `movetoworkspace[silent]`, `focus left|right|up|down`, `exec`/`exec-once`,
`reload`, `quit`. The tiling dispatchers (`movewindow`, `maximize`, `resizeactive`,
`togglefloat`, `movetomonitor`) and the `min_tile_width` / `min_tile_height` settings are gone.

## Acceptance criteria

- [ ] Parser handles key=value, sections, `bind`, `exec`/`exec-once`, `windowrule`, workspace rules, and settings
- [ ] `reload` re-parses and re-registers hotkeys and rules without restart
- [ ] `windowrule` assigns a matching app to a Workspace or marks it `ignore` (excluded from focus); it never affects geometry
- [ ] A `bind` to a removed tiling dispatcher is a diagnostic, not a silent accept
- [ ] Parse errors surface diagnostics and keep the previous good config active
- [ ] Config-parser tests cover valid grammar plus representative error cases

## Blocked by

- 01
