# 01.02 — Config parser + semantic types (Seam 2)

**Labels:** `ready-for-agent`
**Blocked by:** 01.01

## What to build

The pure config parser and the semantic input types it produces — all in core, compiling
without `windows.h`. This is **Seam 2**: `parse(text) → (config, diagnostics)`.

Semantic types owned by core (never Win32 constants):
- `enum class Mod : uint8_t { Super=1, Alt=2, Ctrl=4, Shift=8 }` (bitflags).
- `enum class Key` seeded with what this slice needs: the workspace digits and a key
  usable for a `quit` Bind. Grown additively later.

Grammar — a strict subset of the eventual Hyprland DSL (so the full-grammar slice only
adds directives, never reshapes the parser):
- `#` line comments.
- `$name = tokens` variable definitions with `$name` substitution.
- `bind = MODS, KEY, dispatcher, args` where `dispatcher ∈ { workspace, quit }`.

A malformed line yields a diagnostic and parsing continues (collect-and-continue). The
`diagnostics` half of the return tuple exists from day one. "Keep last good config" and
live reload are out of scope (issue 09).

## Acceptance criteria

- [ ] `parse` returns `(config, diagnostics)`; config holds a list of `Bind{Mod flags, Key, Dispatcher, arg}`
- [ ] `$mod = SUPER ALT` then `bind = $mod, 1, workspace, 1` → `Bind{Super|Alt, N1, workspace, 1}`
- [ ] Comments and blank lines ignored; `workspace`/`quit` dispatchers recognized
- [ ] A malformed line produces a diagnostic while all valid Binds still parse
- [ ] Unknown key or dispatcher name is diagnosed in the parser (not deferred to registration)
- [ ] Compiles into the test TU with no WM libraries linked
- [ ] Catch2 tests cover the above; no live-desktop / Windows dependency
