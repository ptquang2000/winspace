# PRD — Full config grammar + reload (issue 09)

## Problem Statement

A winspace user edits `~/.config/winspace/winspace.conf` to change their keybinds,
window rules, or startup apps — and today the only way to see those edits take effect
is to quit winspace and start it again. There is also no way to say "don't let this
window (a dock, a always-on-top widget) steal directional focus", and no way to declare
"start winspace with my session" from the config itself. The grammar the parser accepts
is a partial Hyprland subset, so a user porting a Hyprland config hits `bind` lines for
dispatchers that no longer exist (the tiling ones) and gets a bare "unknown dispatcher"
that reads like a typo rather than a deliberate scoping decision.

## Solution

Complete the Hyprland-subset DSL and make it hot-reloadable, without winspace ever
owning window geometry (ADR-0007):

- A **`reload`** Dispatcher re-reads and re-applies the config file live — re-registering
  hotkeys, replacing WindowRules and Launch entries, and re-launching `exec` apps — with
  no restart.
- **`windowrule … ignore`** excludes a matching window from Spatial focus (it is never
  a focus target), alongside the existing `windowrule = workspace N` placement.
- **`start_at_login`** is parsed as a flat bool setting and carried into `State`, ready
  for the slice-10 logon task to act on.
- A `bind` (or setting) naming a **removed tiling** dispatcher/option gets a **targeted**
  "removed with tiling" Diagnostic, so a ported Hyprland config reads as *scoped*, not
  broken.
- An invalid reload **keeps the last good config** live rather than degrading or
  crashing.

## User Stories

1. As a winspace user, I want a `reload` keybind, so that my config edits take effect
   without quitting and restarting winspace.
2. As a winspace user, when I hit `reload`, I want my **hotkeys** re-registered from the
   new file, so that added/removed/changed binds are live immediately.
3. As a winspace user, when I hit `reload`, I want my **WindowRules** replaced, so that a
   newly-added `windowrule` places (or ignores) apps that appear afterward.
4. As a winspace user, when I hit `reload`, I want my `exec` apps re-launched and my
   `exec-once` apps left alone, so that reload matches the documented exec semantics.
5. As a winspace user, when my edited file has an error, I want winspace to keep my
   currently-running config live and log the error, so that a typo never leaves me with a
   half-applied or broken setup.
6. As a winspace user, I want the `reload` keybind itself to survive a bad reload, so
   that I can fix the file and reload again without restarting.
7. As a winspace user, I want to mark a window `ignore` in a `windowrule`, so that a dock
   or always-on-top widget never becomes a directional-focus target.
8. As a winspace user, I want an `ignore` rule to match by `exe`, `class`, or `title`
   exactly like a placement rule, so that the match syntax is uniform.
9. As a winspace user, I want an ignored window to still behave normally in every other
   respect (I can still Alt-Tab to it; winspace never moves or resizes it), so that
   `ignore` only affects directional focus.
10. As a winspace user, I want `start_at_login = true` in my config, so that I can
    declare autostart from the same file rather than a separate tool (the task itself
    lands in slice 10).
11. As a winspace user porting a Hyprland config, when I have a `bind` to `movewindow`,
    `maximize`, `resizeactive`, `togglefloat`, or `movetomonitor` — or a
    `min_tile_width` / `min_tile_height` setting — I want a message telling me it was
    removed with tiling, so that I understand it is scoped out, not mistyped.
12. As a winspace user, I want an unknown directive or dispatcher that is *not* a known
    dropped one to still produce a clear generic Diagnostic, so that real typos are still
    caught.
13. As a winspace user, I want a malformed line at **startup** to be skipped with the
    rest of my config still applied, so that one typo does not wipe every bind back to
    the built-in default.
14. As a winspace maintainer, I want reload orchestrated through the pure Reducer and the
    existing I/O threads (ADR-0012), so that the parse stays testable through Seam 2 and
    the reload logic stays consistent with the rest of the architecture.
15. As a winspace maintainer, I want the new grammar and reload behavior covered by
    parser and reducer tests plus a VM Smoke seam, so that regressions are caught without
    a live desktop for the pure parts.

## Implementation Decisions

**Grammar (Seam 2 — `parse`).**
- New Dispatcher **`reload`** (zero-argument, like `quit`).
- `windowrule` gains an **action**: `workspace N` (Place, existing) or **`ignore`**
  (new). The existing "unsupported windowrule action" diagnostic for `ignore` is
  replaced by real parsing. One rule still names one match field (`exe:` / `class:` /
  `title:`) with the same case rules.
- New flat setting **`start_at_login = <bool>`** — `true`/`false` case-insensitive;
  anything else is a Diagnostic. No `section { }` grammar and no workspace rules are
  added (both were geometry-only — ADR-0007). Parsed into `Config`.
- A small static list of **known-removed** names — dispatchers (`movewindow`,
  `maximize`, `resizeactive`, `togglefloat`, `movetomonitor`) and settings
  (`min_tile_width`, `min_tile_height`) — produces a **targeted** "removed with tiling"
  Diagnostic; every other unknown name keeps the generic message.

**Core (Seam 1 — `reduce` / `State`).**
- `WindowRule` carries an action tag `{Place → workspace N | Ignore}`. `matchRule`
  returns the **first matching rule's action** under the unchanged exe→class→title
  precedence.
- `State` gains **`std::unordered_set<WindowId> ignored`** (mirrors `placed`) and
  **`bool startAtLogin`**. On an Eligible `Appeared` whose first match is Ignore, insert
  the id into `ignored` (and `placed`); on `Vanished`, erase from both. `resolveFocus`
  drops any Candidate whose id ∈ `ignored`. An Ignored window is still `isEligible`.
- New Event **`Reload{}`** and new Effect **`ReloadConfig{}`**: `reduce(state, Reload{})`
  emits `ReloadConfig{}` and touches nothing (the Reducer is pure — it cannot read a
  file). The existing `Reloaded{}` Event and its exec-relaunch handler are unchanged.
- `startAtLogin` is carried into `State` (seeded at Worker construction, reseeded on
  reload) but emits **no Effect** this slice.

**I/O layer (no new seam; VM Smoke seam only).**
- The Worker executes `ReloadConfig` by re-resolving the config path, re-reading the
  file, and calling `parse()` **on the Worker thread**. Config path-resolution / file
  read / keep-last-good decision live on the Worker; the initial load in the spine and
  the reload path share one helper rather than duplicating path logic (prefactor).
- **Atomic keep-last-good:** if the new parse yields any Diagnostic (or the file is
  unreadable), log and change nothing. Only a clean parse (`diagnostics.empty()`) is
  applied.
- On a clean parse the Worker fans out (ADR-0012): swap `rules` / `execs` /
  `startAtLogin` into `State`; hand the new `Bind` vector to the Hotkey thread via a new
  `PostThreadMessage` control message carrying a heap `std::vector<Bind>*` (delete-on-
  failure ownership, mirroring `postEvent`), whose loop rebuilds its `HotkeyTable`
  (unregister-all then register-all, on that thread); and post `Reloaded{}` to itself so
  the exec-relaunch path runs unchanged.
- Startup retains **per-line degrade** (keep good lines, drop bad) — the asymmetry with
  atomic reload is deliberate (ADR-0012).

## Testing Decisions

Test external behavior at the two pure seams; verify the cross-thread I/O through the VM.

- **Seam 2 (parser, `config_test.cpp`) — prior art: the existing bind/windowrule/exec
  cases.** Add: `reload` dispatcher parses; `windowrule … ignore` parses to an Ignore
  action for each of exe/class/title; `start_at_login` parses `true`/`false` and
  diagnoses a non-bool; each known-removed dispatcher/setting yields the targeted
  message while a genuine typo yields the generic one; a `windowrule ignore` with a
  missing/empty pattern is diagnosed. Assert on parsed `Config` values and on Diagnostic
  presence/wording, not internals.
- **Seam 1 (reducer, `reducer_test.cpp`) — prior art: the existing Appeared/place-once,
  FocusResolve, and Started/Reloaded cases.** Add: `Reload{}` emits exactly one
  `ReloadConfig{}` and no State change; an Ignore-matched `Appeared` inserts into
  `ignored`; `resolveFocus` skips an ignored Candidate and still picks the next-best; a
  Place-vs-Ignore first-match tie resolves by precedence/order; `Vanished` clears
  `ignored`; `startAtLogin` round-trips through State with no Effect.
- **VM Smoke seam (`ADR-0005` harness) — prior art: the slice-06/07/08 smoke seams.**
  Drive a live winspace: edit the on-disk config (add a bind), Trigger `reload`, assert
  the new bind's Oracle effect fires and an old removed bind no longer does; then write a
  broken file, Trigger `reload`, and assert the previously-working binds still fire
  (keep-last-good).

## Out of Scope

- The Task Scheduler logon task itself — `start_at_login` is only *parsed and carried to
  State* here; registering/removing the task is slice 10.
- A file watcher / auto-reload — reload is an explicit `reload` dispatcher only
  (ADR-0012 rejects a watcher for this slice).
- Any `section { }` block grammar and any workspace rules (geometry-only; ADR-0007).
- Re-asserting rules against already-open windows on reload — a new place/ignore rule
  only affects a window when it next `Appears` (consistent with place-once, ADR-0009).
- Louder-than-stderr diagnostics surfacing for reload errors (stderr is accepted for
  now).
- Unicode case-folding for exe/class matching (ASCII fold only, unchanged).

## Further Notes

- Two names must not be conflated: **`Reload{}`** is the trigger Event; **`Reloaded{}`**
  is the post-parse Event that only re-launches `exec` entries (ADR-0012, CONTEXT.md).
- A plain `exec` re-launches on every reload by design (vs `exec-once`); repeated reloads
  spawn repeated copies — intended and documented in the seeded default config.
- Design of record: ADR-0012 (reload), ADR-0009 extension (ignore action + `ignored`
  set), with scope resting on ADR-0007 (no geometry).
