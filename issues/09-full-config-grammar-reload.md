# 09 — Full config grammar + reload

**Labels:** `ready-for-agent`

## What to build

Complete the hyprland-style DSL and make it hot-reloadable. The parser handles
`key = value`, `section { ... }`, `bind = MODS, KEY, dispatcher, args`, `exec` / `exec-once`,
`windowrule float|tile|ignore`, workspace rules, and general settings (including
`min_tile_width` / `min_tile_height` and `start_at_login`). The `reload` dispatcher
re-parses and re-applies live — re-registering hotkeys and updating rules. Invalid config
produces diagnostics and leaves the last good config active rather than crashing.

## Acceptance criteria

- [ ] Parser handles key=value, sections, `bind`, `exec`/`exec-once`, `windowrule`, workspace rules, and settings
- [ ] `reload` re-parses and re-registers hotkeys and rules without restart
- [ ] `windowrule float|tile|ignore` overrides eligibility per app
- [ ] Parse errors surface diagnostics and keep the previous good config active
- [ ] Config-parser tests cover valid grammar plus representative error cases

## Blocked by

- 01
