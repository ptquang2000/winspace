# 07 — windowrule place-once rules (reintroduce the hook)

**Labels:** `ready-for-agent`
**References:** [ADR-0009](../docs/adr/0009-window-rules-place-once-state.md) (place-once state),
[ADR-0006](../docs/adr/0006-window-tracking-probe-decide-seam.md) (Probe/policy split),
[ADR-0007](../docs/adr/0007-drop-tiling-no-window-geometry.md) (no geometry),
`CONTEXT.md` (Windows & focus, Threads), `DESIGN.md` §5.

> **Owns the hook adapter.** Per ADR-0007 the `SetWinEventHook` adapter and the `Appeared` /
> `Vanished` lifecycle stream were removed from master when tiling was dropped. This slice — the
> hook's first genuine consumer — reintroduces them **on their own dedicated thread**. This
> remains **place-once**: winspace assigns a window to a Workspace, it never sizes or positions
> it (ADR-0007).

## What to build

`windowrule` app→workspace rules that pin a matching window to a target workspace when it
appears. Rules are evaluated on the **`Appeared` edge** (`EVENT_OBJECT_SHOW` /
`EVENT_OBJECT_UNCLOAKED` — **not** raw `CREATE`, where the window is half-born and the title is
empty), gated on `isEligible`. Match precedence is the fixed order **exe → class →
title(regex)**, config order breaking within-field ties, first match wins. The assignment is
**place-once**: a matching window is assigned exactly once in its lifetime — a user-moved window
is never yanked back — and rules also apply to windows already open at startup (adoption).

The move itself reuses the `MoveWindowToWorkspace` Effect, the bridge move path, and the
Worker cloak→move→uncloak wrapping built in 06.

## Design decisions

**Trigger edge & place-once (ADR-0009).** Rules fire on `Appeared`, which re-fires on every
uncloak. `State` gains `std::vector<WindowRule> rules` (seeded from config) and
`std::unordered_set<WindowId> placed`. On `Appeared`: skip if `id ∈ placed`; else match (gated
on `isEligible`), emit `MoveWindowToWorkspace{id, N}` if a rule matched, then insert `id` into
`placed` **regardless**. On `Vanished`: erase `id` (bounds the set; guards against HWND reuse).
Startup adoption posts synthetic `Appeared`s through the same path.

**Probe facts (ADR-0006 split).** `Appeared` carries `WindowAttrs` (Eligibility bools + rect,
via the existing `probeWindow`) **and** a new `WindowIdentity { WindowId id; std::string exe,
windowClass, title; }` (new `probeIdentity`: `QueryFullProcessImageNameW` basename,
`GetClassNameW`, `GetWindowTextW`, narrowed to UTF-8). The focus sweep does **not** probe
identity — strings stay off the hot focus path.

**Pure matcher.** In the Reducer: three passes (exe, then class, then title) over `State.rules`;
exe/class exact case-insensitive, title `std::regex`; first match supplies the target
workspace. Rules live in `State` because `reduce(state, event)` takes no config.

**Config grammar.** New directive `windowrule = workspace N, <field>:<pattern>`; RHS split on
the **first** comma (rule vs. match spec) so a title regex may contain commas. `Config` grows
`std::vector<WindowRule> rules`; a malformed rule yields a `Diagnostic` and parsing continues.
The parser is extended, not reshaped (PRD 09 contract).

**Threads (CONTEXT.md).** The hook adapter runs on its **own dedicated thread**
(`WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS`, `thread_local` worker HWND, `postEvent`
transport, noise gate `idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd`,
`PostThreadMessage(WM_QUIT)` + `UnhookWinEvent` teardown), mirroring the Hotkey thread. It owns
no State — it only produces `Appeared` / `Vanished`. `runApp` grows a third `jthread`; the
Worker ctor takes the parsed rules and seeds `m_state.rules` (`loadBinds` → `loadConfig`
returning `{binds, rules}`).

## Acceptance criteria

- [ ] A `windowrule` pins a matching app to its target workspace on `Appeared` (SHOW/UNCLOAKED),
      including apps already open at startup (adoption)
- [ ] Match precedence is exe → class → title(regex), config order breaking within-field ties,
      first match wins
- [ ] Rules place once: a window is assigned at most once per lifetime; a user-moved window is
      not re-pinned on later uncloaks (`placed` set; erased on `Vanished`)
- [ ] Rules match only `isEligible` windows
- [ ] Reducer tests: an `Appeared` matching a rule emits exactly `MoveWindowToWorkspace{thatId,
      N}`; a repeat `Appeared` for the same id emits nothing; field precedence and
      first-match-wins are pinned over synthetic `WindowIdentity` sets; an ineligible/`placed`
      window emits nothing
- [ ] Parser tests: `windowrule` grammar (each field kind, first-comma split, malformed →
      diagnostic + continue)
- [ ] Clean hook teardown on quit — no dangling `SetWinEventHook` (VM Smoke seam / self-test)

## Blocked by

- 06 (reuses the `MoveWindowToWorkspace` Effect, the bridge move path, and the cloak-move
  wrapping it builds)
