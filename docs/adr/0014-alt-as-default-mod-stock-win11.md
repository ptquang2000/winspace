# The default `$mod` is `ALT`, so winspace binds on a stock Windows 11

**Status:** Accepted (2026-07-12 — supersedes the mod-key choice previously recorded only in
session memory, never in an ADR, across three reversals: `SUPER ALT` → `CTRL ALT` → `SUPER` → `ALT`).

The seeded default config (`src/io/config_io.cpp`, `k_defaultConfig`) binds workspace switching,
move-to-workspace, focus, reload, and quit to a modifier `$mod`. This ADR records *why* `$mod`
defaults to `ALT` — a choice that runs against every tiling-WM convention (Super/Win is the idiom)
and so will read as arbitrary to a future reader unless the constraint behind it is written down.
It has flip-flopped repeatedly precisely because that constraint lived only in memory; this record
exists to stop a fourth reversal.

## Context

winspace registers its binds with `RegisterHotKey`. On Windows the available chord space is not
free: the shell and the OS reserve large families of chords, and a reserved chord fails
registration with `ERROR_HOTKEY_ALREADY_REGISTERED` (1409). winspace correctly skip-and-logs a
failed bind (ADR-0004), but a skipped bind is a *dead* bind — the feature silently does nothing.

The reserved landscape, established by direct `RegisterHotKey` probes on a clean `win11-24h2`
guest (see the VM harness, ADR-0005):

- **`Win+<key>` shell shortcuts** (`Win+1..5`, `Win+Q`, `Win+E/D/R/Tab`, …) are reserved by the
  shell. `RegisterHotKey(MOD_WIN, …)` returns 1409 on stock Windows. They can be freed *only* by
  the machine-global `NoWinKeys=1` policy (`HKCU\…\Policies\Explorer`), which also disables the
  user's own `Win+E/D/R/Tab`.
- **`Win+Alt+<digit>`** is the taskbar jump-list accelerator — also reserved (1409). This is what
  killed the original `SUPER ALT` default.
- **The `Win+<arrow>` and `Win+h/j/k/l` clusters** are held by the shell / Explorer / OS even
  *under* `NoWinKeys` (`Win+J` is Explorer's self-healing bind; `Win+K`/`Win+L` are
  system/SYSTEM-owned). `NoWinKeys` frees shell *input-pipeline* shortcuts, never another
  process's or the OS's `RegisterHotKey` claim.
- **`Alt+<digit>`, `Alt+<letter>` (incl. `h/j/k/l`), `Alt+Shift+<key>`** all register cleanly on
  a stock guest with **no policy applied**.
- **Native Virtual-Desktop hotkeys `Win+Ctrl+D` / `Win+Ctrl+F4`** live below the shell hotkey
  table and are always available regardless of `$mod` — orthogonal to this decision.

The requirement that forces the choice: **winspace should work on an unmodified Windows 11**, with
no registry policy the end user must opt into. Any default whose binds need `NoWinKeys` fails that
bar — it either ships broken out of the box or demands a global, destructive policy first.

## Decision

The seeded default is **`$mod = ALT`**. Every default bind references `$mod`, so the whole default
keymap sits in the `Alt` / `Alt+Shift` space that registers on stock Windows 11:

| Dispatcher | Bind |
|---|---|
| `workspace 1..5` | `Alt+1..5` |
| `movetoworkspacesilent 1..5` | `Alt+Shift+1..5` |
| `focus left/down/up/right` | `Alt+H/J/K/L` |
| `reload` | `Alt+Shift+R` |
| `quit` | `Alt+Shift+Q` |

`quit` sits on `Alt+Shift` (not bare `Alt+Q`) so a stray keystroke cannot kill the WM. The focus
binds, previously hardcoded to a literal `ALT` specifically to *escape* the reserved `Win+hjkl`
under the old `$mod = SUPER`, now collapse onto `$mod` — the reason for the split evaporated once
`$mod` itself became `ALT`.

The `NoWinKeys` requirement is **dropped entirely**: winspace never set it, and the default no
longer needs it. The VM harness reverts to the pristine, stock **`winspace-e2e`** snapshot; the
policy-baked `winspace-e2e-nowinkeys` child is retired.

## Considered Options

- **`$mod = ALT` (chosen).** The only modifier whose digit and letter chords register on a stock
  Windows 11 with no policy. Makes winspace work out of the box and lets the whole `NoWinKeys`
  story — an end-user opt-in, a baked VM snapshot, an elevated provisioning step — be deleted. The
  price (see Consequences) is real but bounded and user-rebindable.

- **`$mod = SUPER` (Win) + require `NoWinKeys=1`.** *Rejected (this reverses the prior default.)*
  It is the tiling-WM idiom and reads naturally, but it **cannot bind on a stock machine**:
  `Win+<n>`/`Win+Q` return 1409 until the user applies a machine-global policy that *also* kills
  their `Win+E/D/R/Tab`. That is exactly the "works only after a destructive opt-in" failure the
  requirement rules out. It also forced a two-snapshot VM setup (`winspace-e2e` +
  `winspace-e2e-nowinkeys`) and an elevated provisioning step.

- **`$mod = SUPER ALT` (the original default).** *Rejected.* `Win+Alt+<digit>` is the taskbar
  jump-list accelerator and returns 1409 on stock Windows — even *with* `NoWinKeys`. It bound none
  of workspaces 1–5. This is the concrete failure that started the whole saga.

- **`$mod = CTRL ALT` (an interim default).** *Rejected.* `Ctrl+Alt+<n>` does register on stock
  Windows with no policy — the one property that made it a viable interim — but `Ctrl+Alt` is a
  two-modifier chord for the *primary, highest-frequency* action (workspace switch), and
  `Ctrl+Alt+<key>` is widely colonised (e.g. `Ctrl+Alt+Del`, vendor and IDE bindings). `Alt`
  alone achieves the same "registers on stock" property with a lighter, single-modifier chord for
  the common case, so `Alt` dominates it.

- **Bare `Alt` as a whole hotkey (Alt alone, no second key).** *Rejected* (and not the same thing
  as `$mod = ALT`). A single-`Alt` global hotkey fires unreliably from synthesized input and, held
  alone, activates the focused app's menu bar. `$mod = ALT` never binds `Alt` alone — every bind is
  a real `Alt+<key>` chord, which `RegisterHotKey` consumes wholesale, so this failure mode does
  not apply. Recorded here only because "we tried Alt and rejected it" is a half-truth that has
  caused confusion.

## Consequences

- **winspace works on an unmodified Windows 11** — no registry policy, no elevation, no setup. The
  `NoWinKeys` end-user requirement, the `winspace-e2e-nowinkeys` snapshot, and the elevated
  provisioning step are all gone.
- **Accepted tradeoff — global Alt shadowing.** As global hotkeys, winspace's `Alt+<key>` chords
  win over the focused app's own `Alt+<key>` accelerators/mnemonics while winspace runs (e.g. an
  app's `Alt+<digit>` panel switch). This is intrinsic to a bare-Alt modifier and is the price of
  not needing `NoWinKeys`. It is documented in the seeded config header and is user-rebindable.
- **Accepted caveat — multi-layout `Alt+Shift`.** `Alt+Shift` is the OS input-language/layout
  toggle. A full `Alt+Shift+<key>` chord normally suppresses the bare toggle, but on machines with
  two or more keyboard layouts installed the `reload` / `movetoworkspace` / `quit` binds may clash.
  Single-layout machines (the common case, and the VM) are unaffected. Documented, not mitigated.
- **Latent config coupling.** Because the default focus binds now reference `$mod`, a user who
  rebinds `$mod` back to `SUPER` silently re-exposes the reserved `Win+hjkl` and breaks focus. This
  is the user's config to own; the shipped default is correct.
- **The native VD staging chords `Win+Ctrl+D` / `Win+Ctrl+F4` are untouched** — they are OS-level
  and independent of `$mod`; the VM harness still uses them to stage desktops.
- **If a future default must return to `Win`,** it inherits the `NoWinKeys` requirement and the
  two-snapshot VM setup wholesale — this decision must be revisited as a unit, not silently flipped.
