# Live config reload: Worker-parsed, atomic, two-thread fan-out

**Status:** Accepted (2026-07-11)

The `reload` dispatcher re-reads and re-applies the config file live — re-registering
hotkeys, replacing window rules and launch entries, and re-launching `exec` apps —
without restarting winspace. This ADR records how a reload is orchestrated across the
pure Reducer and the two I/O threads that hold the config-derived state, and why an
invalid reload keeps the running config rather than degrading it.

## Context

The config-derived state is split across three owners, none of which can re-derive
itself in place:

- **Binds** are live OS hotkeys owned by the **Hotkey thread** — `RegisterHotKey` /
  `UnregisterHotKey` are thread-affine, so the `HotkeyTable` can only be rebuilt on
  that thread.
- **`rules`** and **`execs`** are immutable `std::shared_ptr<const …>` in the Worker's
  `State`, seeded once at Worker construction ([ADR-0009](0009-window-rules-place-once-state.md),
  [ADR-0011](0011-launcher-launch-only-placement-via-windowrule.md)).
- The **Reducer is pure** — it may not read a file. The `Reloaded{}` Event that
  already exists ([ADR-0011](0011-launcher-launch-only-placement-via-windowrule.md))
  only *consumes* an already-parsed result (it re-emits `LaunchApp` for the `exec`
  entries); it never parses.

So a reload is inherently an I/O orchestration that fans out to two threads plus a file
read — it cannot be a single pure `reduce`. The `parse()` seam itself is unchanged
(issue 09 only adds directives); this ADR is about *driving* it live.

The parser never fails wholesale: `parse(text) → {Config, diagnostics}` always yields a
`Config`, skipping malformed lines and diagnosing them. There is therefore no built-in
"this file is rejected" signal — "keep the last good config" has to be a policy layered
on top of the diagnostics vector.

## Decision

- **The trigger is a Bind like any other.** `reload` is a new zero-argument Dispatcher.
  The Hotkey thread posts a **`Reload{}` Event** to the Worker through the ordinary
  transport — nothing config-specific runs on the Hotkey thread at trigger time.
- **The Reducer asks, it does not act.** `reduce(state, Reload{})` is pure and emits a
  single **`ReloadConfig{}` Effect**. It reads no file and touches no `State`.
- **The Worker parses.** Executing `ReloadConfig`, the Worker re-resolves the config
  path, re-reads the file, and calls `parse()` **on the Worker thread**. The path
  resolution, file read, and the keep-last-good decision therefore live on the Worker,
  not in the process spine (`app.cpp`) where the *initial* load lives.
- **Reload is atomic — keep-last-good on any diagnostic.** If the new file produces
  **any** Diagnostic (or is unreadable), the Worker logs the errors and changes
  **nothing**: the running Binds, `rules`, and `execs` stay live. Only a clean parse is
  applied. The gate is exactly `parsed.diagnostics.empty()`.
  Reload also passes `SeedPolicy::NoSeed` to the shared `readAndParseConfig`, so a
  reload of a *deleted* file is treated as unreadable and keeps the last good config
  — it never re-seeds the built-in default. Startup passes `SeedPolicy::Seed`. The
  seed step lives inside `readAndParseConfig` but is gated by this policy, preserving
  the startup-seeds / reload-doesn't asymmetry.
- **A clean parse fans out to three places:**
  1. the Worker swaps `m_state.rules`, `m_state.execs`, and `m_state.startAtLogin` to
     the freshly-parsed values (new `shared_ptr`s);
  2. the Worker hands the new `std::vector<Bind>` to the Hotkey thread by
     `PostThreadMessage` of a heap-allocated pointer — mirroring how `WM_QUIT` already
     unwinds that thread — and the Hotkey thread's loop destroys its old `HotkeyTable`
     (unregistering every hotkey on the correct thread) and constructs a new one;
  3. the Worker posts **`Reloaded{}`** to itself, so the existing exec-relaunch path
     ([ADR-0011](0011-launcher-launch-only-placement-via-windowrule.md)) runs unchanged
     — `exec` entries re-launch, `exec-once` entries do not.
- **Startup keeps its per-line-degrade behavior.** The initial load still keeps the
  good lines and drops the bad, because startup's only fallback is the *built-in
  default* — destructive (one typo would wipe every user bind). Reload's fallback is
  the user's own running config — non-destructive — so reload can afford to be atomic.
  The asymmetry is deliberate: **fall back to the least-destructive thing available.**

## Considered Options

- **Parse on the Hotkey thread** (it already owns the Binds), then post `rules`/`execs`
  to the Worker. *Rejected* — it splits config parsing across two threads (the Worker
  still needs the parsed rules) and puts file I/O on the thread that must stay
  responsive to keystrokes.
- **Keep parse in `app.cpp` and call back into it from the Worker.** *Rejected* — the
  spine's threads have already been launched and the config-derived state lives on the
  Worker; routing a live reload back through the spine adds a cross-thread hop and a
  second owner of the "current good config" for no benefit. The Worker is where the
  state to be replaced lives, so it is where the parse belongs.
- **Per-line degrade on reload** (apply the good lines, drop the bad, like startup).
  *Rejected* — winspace is windowless and logon-launched; diagnostics go to stderr,
  which the user rarely sees live. A silently-dropped bind would vanish on reload with
  no visible signal, leaving a running config that is a surprise hybrid. Atomic reload
  keeps the user's working setup live until the *whole* new file is clean, so nothing
  silently disappears — the worst case is "my edit didn't take effect."
- **A file watcher instead of an explicit `reload` dispatcher.** *Rejected for this
  slice* — a watcher fires mid-save (partial file) and on every editor write, forcing
  debounce and half-file-parse handling. An explicit `reload` bind is one deterministic
  edge the user controls. A watcher can be added later on top of the same
  `ReloadConfig` machinery.

## Consequences

- The Worker gains file-I/O responsibilities (`configPath` / `readFile` / `parse`) it
  did not have before; these move (or are shared) out of `app.cpp`. The Worker remains
  the single owner of everything config-derived — now including the "current good
  config" that a failed reload falls back to.
- There are now **two** reload-related names that a reader must not conflate:
  **`Reload{}`** is the trigger Event (hotkey → reducer → `ReloadConfig` Effect);
  **`Reloaded{}`** is the post-parse Event that only re-launches `exec` entries. They
  are recorded under *Reload* / *Reloaded* in `CONTEXT.md`.
- A plain `exec` entry re-launches on **every** reload (that is its defined meaning vs
  `exec-once`), so repeated reloads spawn repeated copies of `exec` apps — intended,
  and documented in the seeded default config.
- Reload does **not** re-assert rules against already-open windows: a newly-added
  `windowrule` (place or ignore) only affects a window when it next `Appears`,
  consistent with place-once ([ADR-0009](0009-window-rules-place-once-state.md)).
- Behavior asymmetry between startup and reload is intentional and documented, so a
  future reader who expects them to share a code path finds the rationale here.
