# Issues — Full config grammar + reload (issue 09)

Tracer-bullet slices derived from `PRD.md`. Each cuts end-to-end through the seam(s) it
touches — the pure config parser (Seam 2), the pure Reducer (Seam 1), and, for reload,
the I/O layer verified by a VM Smoke seam. Slices 1–3 are independent and can be grabbed
in any order; slice 4 (reload) has no hard blocker but is best landed after 1–2 so its VM
Smoke seam also exercises ignore rules and `start_at_login` for free.

Design of record: [ADR-0012](docs/adr/0012-config-reload-worker-parsed-atomic.md)
(reload), the [ADR-0009](docs/adr/0009-window-rules-place-once-state.md) extension
(ignore action + `ignored` set), on [ADR-0007](docs/adr/0007-drop-tiling-no-window-geometry.md)
(no geometry).

## `windowrule … ignore` (exclude from focus)

### What to build

Give `windowrule` a second action beside `workspace N`: `ignore`, which excludes a
matching window from Spatial focus. A `WindowRule` carries an action tag — `Place`
(pin to a Workspace, existing) or `Ignore` — and the matcher returns the first matching
rule's action under the unchanged exe→class→title precedence. The Reducer keeps a bounded
`ignored` set of `WindowId` (mirroring `placed`): an Eligible `Appeared` whose first match
is an Ignore rule inserts the id into `ignored` (and `placed`); `Vanished` clears both.
Directional focus resolution drops any Candidate in `ignored`. An Ignored window is still
`isEligible` and is otherwise untouched — winspace never moves or sizes it, and it is
still Alt-Tab reachable.

### Acceptance criteria

- [ ] `windowrule = ignore, exe:…` (and `class:` / `title:`) parses to an Ignore-action
      rule; the old "unsupported windowrule action" diagnostic for `ignore` is gone
- [ ] A `windowrule ignore` with a missing/empty pattern is diagnosed, not silently accepted
- [ ] `matchRule` returns the first matching rule's action under exe→class→title precedence;
      a window matching both an Ignore and a Place rule resolves deterministically by that order
- [ ] An Eligible `Appeared` matching an Ignore rule inserts the id into `ignored`; `Vanished`
      erases it
- [ ] `resolveFocus` never returns an ignored window, and still selects the next-best
      Candidate past it
- [ ] An Ignored window remains `isEligible` and is never moved, sized, or otherwise acted on
- [ ] Parser and reducer tests cover the above

### Blocked by

None — can start immediately.

## `start_at_login` parsed into State

### What to build

Parse a flat `start_at_login = <bool>` setting and carry it into `State`, ready for the
slice-10 logon task to consume. No `section { }` grammar and no workspace rules are added
(both were geometry-only — ADR-0007). The flag becomes a `bool startAtLogin` in `State`,
seeded at Worker construction beside `rules`/`execs`. This slice emits **no Effect** — it
only parses and holds the value.

### Acceptance criteria

- [ ] `start_at_login = true` / `false` parses (case-insensitive) into `Config`
- [ ] A non-bool value (e.g. `start_at_login = maybe`) is diagnosed
- [ ] Absent `start_at_login` behaves identically to `false` (no task later)
- [ ] `startAtLogin` is carried into `State`, seeded at Worker construction
- [ ] Reducing any Event leaves `startAtLogin` unchanged and emits no Effect for it
- [ ] Parser and reducer tests cover parse, bad value, and State round-trip

### Blocked by

None — can start immediately.

## "removed with tiling" diagnostics

### What to build

Make a ported Hyprland config read as *deliberately scoped* rather than broken. A small
static list of known-removed names — dispatchers (`movewindow`, `maximize`,
`resizeactive`, `togglefloat`, `movetomonitor`) and settings (`min_tile_width`,
`min_tile_height`) — produces a targeted "removed with tiling" Diagnostic. Every other
unknown dispatcher/directive keeps the generic message, so genuine typos are still caught.

### Acceptance criteria

- [ ] A `bind` to each of the five removed tiling dispatchers yields a targeted "removed
      with tiling" diagnostic naming the dispatcher
- [ ] `min_tile_width` / `min_tile_height` settings yield the same targeted diagnostic
- [ ] An unknown name that is NOT a known dropped one still yields the generic
      unknown-dispatcher / unknown-directive diagnostic
- [ ] Parser tests cover both the targeted and the generic paths

### Blocked by

None — can start immediately.

## Live `reload`

### What to build

Add the `reload` Dispatcher and make config hot-reloadable with no restart, orchestrated
per ADR-0012. Prefactor first: extract one shared config-load helper (path resolve + file
read + `parse`) usable by both the process spine and the Worker. The `reload` hotkey posts
a `Reload{}` Event; the pure Reducer emits a single `ReloadConfig{}` Effect (it reads no
file). The Worker executes `ReloadConfig` on its own thread: re-read + re-parse, and apply
**atomically** — any Diagnostic (or an unreadable file) keeps the currently-running config
live and logs the errors, changing nothing. On a clean parse the Worker fans out: reseed
config-derived `State` (rules, execs, and `startAtLogin` where present), hand the new Binds
to the Hotkey thread via a new `PostThreadMessage` control message (heap `std::vector<Bind>*`,
delete-on-failure ownership) that rebuilds its `HotkeyTable`, and post `Reloaded{}` to
itself so `exec` entries re-launch and `exec-once` entries do not. Startup keeps its
per-line-degrade behavior (the asymmetry is deliberate).

### Acceptance criteria

- [ ] The shared config-load helper is used by both the spine's initial load and the
      Worker's reload path (no duplicated path/read/parse logic)
- [ ] `reload` parses to a zero-argument dispatcher; `reduce(state, Reload{})` emits exactly
      one `ReloadConfig{}` and no State change
- [ ] Triggering `reload` on a valid edited file re-registers hotkeys (a newly-added bind
      fires; a removed bind no longer does), reseeds rules/execs/`startAtLogin`, and
      re-launches `exec` (not `exec-once`) entries
- [ ] Triggering `reload` on a file with any parse error keeps the previous good config
      fully live (previously-working binds still fire) and logs diagnostics — nothing is
      partially applied
- [ ] The `reload` bind itself survives a bad reload, so the user can fix and reload again
- [ ] Startup still degrades per-line (a malformed line is skipped, the rest applied)
- [ ] Parser and reducer tests cover `reload` → `ReloadConfig`; a VM Smoke seam covers
      apply-new-bind and keep-last-good on a live desktop

### Blocked by

None hard. Recommended after `start_at_login parsed into State` (so the `startAtLogin`
reseed field exists) and `windowrule … ignore` (so the reload Smoke seam also exercises
ignore rules).
