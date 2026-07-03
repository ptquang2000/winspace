# PRD — Walking skeleton: config-driven workspace switch

**Labels:** `ready-for-agent`
**Slice:** issue 01 (see `issues/01-walking-skeleton-workspace-switch.md`)
**Respects:** `docs/adr/0002-workspaces-as-os-virtual-desktops.md`, `docs/adr/0003-sparse-virtual-workspace-model.md`
**Vocabulary:** `CONTEXT.md` (Workspace, Virtual Desktop, Logical workspace number, Adoption, Reaping, Reducer, Event, Effect, State, Dispatcher, Bind, Hotkey thread, Worker thread)

## Problem Statement

I want a keyboard-driven way to switch between Windows 11 Virtual Desktops that feels
native and stays instant, but before any tiling or window-management value can be built,
there needs to be a proven end-to-end path from a keystroke to an OS desktop switch. The
riskiest part of that path — the undocumented COM interface whose binary layout changes
per Windows build — has to be conquered first, because if it can't be driven reliably,
nothing else in winspace matters. I need the smallest possible slice that exercises the
whole spine (config → hotkey → pure logic → OS effect) so every later feature has a
truthful template to plug into instead of a shape that gets torn up later.

## Solution

A windowless user-session process that reads a minimal Hyprland-style config, registers
the bound hotkeys with the OS, and routes each press through a pure Reducer into a Virtual
Desktop switch executed by a COM bridge. Pressing `$mod+N` switches to Logical workspace N
— adopting the desktops that already exist, materializing a new one on demand if N doesn't
exist yet, and always landing on the *same* desktop for a given logical number even after
the user reorders desktops in Task View. A `quit` Bind exits the process cleanly. The slice
establishes the core seam (`reduce(state, event) → (newState, effects)`) and the
translation-adapter boundary (semantic types in, Win32/COM out) that the whole product is
built on, and it proves the hardest integration — runtime selection of the correct Virtual
Desktop vtable variant — works on the target machine.

## User Stories

1. As a keyboard-driven user, I want to press `$mod+N` and switch to Logical workspace N, so that I can move between contexts without the mouse.
2. As a user, I want the switch to move the actual OS Virtual Desktop, so that the taskbar, Alt-Tab, and focus behave natively and I'm not looking at a winspace-owned illusion.
3. As a user, I want winspace to adopt the Virtual Desktops I already have open when it starts, so that launching it doesn't reset or duplicate my existing session.
4. As a user, I want the desktop I'm currently on to become the current Logical workspace at startup, so that winspace inherits where I already am.
5. As a user, I want pressing `$mod+N` for a workspace that doesn't exist yet to create exactly one new desktop, so that I get the workspace I asked for without junk desktops filling the gap.
6. As a user, I want `$mod+5` from three desktops to create one desktop (not four), so that unused intermediate workspaces never accumulate.
7. As a user, I want a fat-fingered `$mod` bind to a huge number to create just one desktop labeled with that number, so that a typo can't spawn hundreds of desktops.
8. As a user, I want a given Logical workspace number to always take me to the same physical desktop even after I drag-reorder desktops in Task View, so that my workspace numbers are stable.
9. As a user, I want to quit winspace with a bound `quit` Dispatcher, so that I can stop it cleanly without Task Manager.
10. As a user, I want winspace to run with no console window, no taskbar button, and no Alt-Tab entry, so that it stays out of my way and is visible only in Task Manager.
11. As a user, I want the hotkey to work even while my machine is under heavy load, so that switching never stutters when I most need it.
12. As a user, I want winspace to never install a low-level keyboard hook, so that it can't time out or drop keystrokes and can't be blamed for input lag.
13. As a user, I want a matched hotkey combo to be consumed by winspace and not leak to the foreground app, so that my Bind never triggers something in the app underneath.
14. As a user, I want unbound key combinations to pass through untouched, so that winspace only intercepts exactly what I configured.
15. As a user, I want a config Bind expressed as `bind = MODS, KEY, dispatcher, args`, so that configuration is familiar to Hyprland users.
16. As a user, I want to declare a reusable modifier once as `$mod = ...` and reference it in every Bind, so that I can change my global modifier in one place.
17. As a user, I want comments in my config, so that I can annotate my keybindings.
18. As a user, I want a malformed config line to produce a diagnostic while the rest of my valid Binds still load, so that one typo doesn't disable my whole config.
19. As a user, I want winspace to tell me when a hotkey combo is already taken by another app (registration failed), so that I know why a Bind isn't working.
20. As a user, I want winspace to log which Virtual Desktop vtable variant it resolved on my build, so that I can confirm it detected my system correctly.
21. As a user, I want winspace to fail loudly with a clear diagnostic if it can't find a working Virtual Desktop interface, rather than crash or silently misbehave, so that breakage on a future Windows build is obvious.
22. As a developer, I want all behavioral logic to live in a pure Reducer that emits Effects as plain data, so that I can unit-test workspace-switch behavior with zero Windows dependencies.
23. As a developer, I want the Reducer and config parser to compile without `windows.h`, so that the "core touches no OS API" rule is enforced by the linker, not by discipline.
24. As a developer, I want key and modifier names translated to Win32 constants only in the I/O adapter, so that the parser stays pure and testable.
25. As a developer, I want all COM ugliness quarantined behind a bridge that speaks workspace vocabulary, so that the rest of winspace is oblivious to `IVirtualDesktop`, `IObjectArray`, and `HRESULT`.
26. As a developer, I want the correct vtable variant selected by probing interface IIDs (not by matching build numbers), so that a Windows update that shifts the vtable within a build doesn't silently break switching.
27. As a developer, I want the Hotkey thread and Worker thread wired exactly as the real architecture, so that later slices plug into a truthful skeleton instead of one that has to be re-threaded.
28. As a developer, I want the Worker thread to own the COM bridge as the sole STA thread, so that COM apartment rules are never violated.
29. As a developer, I want the `logical→GUID` map and adoption/create-on-demand logic to live in the bridge, so that the Reducer reasons only in Logical workspace numbers and never sees a GUID.

## Implementation Decisions

**Build & process shape** (see the settled build-system handoff):
- One handmade `build.ps1` invoking `cl.exe` directly — no CMake, no vcpkg, no package manager. Assumes an x64 Native Tools dev prompt (`cl` on `PATH`); fails fast with one line if absent.
- **Two unity translation units.** The app TU includes core + I/O adapters + `WinMain`, links the WM libraries (`user32 ole32 oleaut32 …`), built `/SUBSYSTEM:WINDOWS` (windowless). The test TU includes **core only** + Catch2 and links **none** of the WM libraries — so any accidental OS call from core is a link error. This reproduces a three-target linker-level purity guarantee with two `cl` invocations.
- Catch2 v3 vendored (amalgamated). Always-clean rebuild; `/std:c++latest`, `/W4 /WX`, `/permissive-`, `/utf-8`, `UNICODE`.

**Core (pure, no `windows.h`):**
- The central seam is a pure Reducer. Event and Effect are each a `std::variant` of small aggregate structs; `reduce` dispatches with an overloaded visitor and returns fresh State by value. Shape (from a prototype, trimmed to the decision):
  ```
  struct ReduceResult { State state; std::vector<Effect> effects; };
  ReduceResult reduce(const State& s, const Event& e);

  Event  = WorkspaceSwitch{ int n } | Quit{}
  Effect = SwitchToWorkspace{ int logical } | Exit{}
  State  = { int current_workspace; bool running; }
  ```
- Semantic input types owned by core: `enum class Mod : uint8_t { Super=1, Alt=2, Ctrl=4, Shift=8 }` (bitflags) and an `enum class Key` seeded with what this slice needs (workspace digits + a `quit` key), grown additively per slice.
- The config parser is pure: `parse(text) → (config, diagnostics)`. Grammar is a strict subset of the eventual Hyprland DSL: `#` comments, `$name = tokens` variable definitions with substitution, and `bind = MODS, KEY, dispatcher, args` where `dispatcher ∈ { workspace, quit }`. A malformed line yields a diagnostic and parsing continues (collect-and-continue); the `diagnostics` half of the tuple exists from day one. The "keep last good config" retention is deferred to the full-grammar slice.

**I/O adapters (own all `windows.h`/COM):**
- **Hotkey thread:** calls `RegisterHotKey` (never `WH_KEYBOARD_LL`), runs a `GetMessage` loop, translates `WM_HOTKEY` → Event, and posts it to the Worker via `PostMessage` to the Worker's message-only window (`WM_APP`, heap-allocated `Event*` passed in `LPARAM`; the Worker takes ownership and deletes after reducing). Checks each `RegisterHotKey` return value and emits a diagnostic on `ERROR_HOTKEY_ALREADY_REGISTERED`.
- **Worker thread:** `CoInitializeEx(COINIT_APARTMENTTHREADED)`; creates a message-only (`HWND_MESSAGE`) window in its constructor so its HWND is valid before anything posts; runs a `GetMessage` loop whose `WndProc` reduces each Event and executes the emitted Effects. Sole owner of State, the Reducer, and the COM bridge.
- **Mod/Key → Win32 translation** happens only here (`Mod` → `MOD_*`, `Key` → `VK_*`) via two small tables.

**COM Virtual Desktop bridge** (ADR 0002, ADR 0003):
- `IVirtualDesktopBridge` is a pure-abstract interface speaking winspace vocabulary; `IVirtualDesktop*`/`IObjectArray*`/`HRESULT` never escape it. Minimal surface this slice: switch to a Logical workspace number, plus whatever adoption/create-on-demand needs internally.
- Acquisition: `CoCreateInstance(CLSID_ImmersiveShell, IID_IServiceProvider)` → `QueryService(CLSID_VirtualDesktopManagerInternal, <probed IID>)`.
- **Variant selection is IID-probe, self-validating:** `QueryInterface` each known IID newest→oldest; first `S_OK` wins (QI-success ⟺ correct vtable). Build/UBR read only for diagnostic logging. Returns null + a loud diagnostic if none match.
- Interfaces hand-declared as `MIDL_INTERFACE` structs per variant, IIDs/vtable orderings sourced from the community RE lineage and annotated by build/KB. **Only the 24H2+ variant is implemented and verified** (dev machine build 26100.8655), wiring `GetDesktops` / `IVirtualDesktop::GetID` / `CreateDesktop` / `SwitchDesktop`. The 21H2 and 23H2-KB5034204+ variants are declared + a stub that emits a loud "variant not yet implemented" diagnostic.
- **Sparse/virtual workspace model:** a Logical workspace number maps to a Virtual Desktop **by GUID, not array position**. Startup **adoption** binds existing desktops to `1..N` by GUID and seeds `current_workspace` from the active desktop. **Switch:** hit → resolve the stored GUID to its current live array index → `SwitchDesktop`; miss → `CreateDesktop` (one desktop, appended), bind the logical number to its GUID, switch. No intermediate filling, no clamp. The `logical→GUID` map, adoption, create-on-demand, and GUID→position resolution all live in the bridge.

## Testing Decisions

- **Good tests exercise external behavior, not implementation details.** For the Reducer that means feeding Event sequences and asserting the emitted **Effects** (e.g. a `WorkspaceSwitch{5}` Event yields a `SwitchToWorkspace{5}` Effect; a `Quit` yields `Exit`) — never inspecting State internals directly beyond what an Effect reveals.
- **Seam 1 — the pure Reducer.** Unit-tested with zero Windows dependencies (the test TU links no WM libraries). Coverage: workspace-switch Event → `SwitchToWorkspace` Effect with the right logical number; `Quit` Event → `Exit` Effect and `running` cleared; State transition of `current_workspace`.
- **Seam 2 — the config parser.** Unit-tested as `parse(text) → (config, diagnostics)`: `$mod` variable definition and substitution; well-formed `bind` lines producing the expected `Bind{Mod flags, Key, Dispatcher, arg}`; comments ignored; `workspace`/`quit` dispatchers recognized; representative malformed lines producing diagnostics while valid Binds still parse (collect-and-continue).
- These are the **two highest seams available**; no mockable bridge seam is introduced (the skill prefers fewer seams, and the bridge's value is entirely in real COM behavior that a fake wouldn't prove).
- **Prior art:** Catch2 v3 (an earlier iteration used per-module suites for BSP/rules/config); this reducer-centric seam supersedes separate logic suites by testing behavior through observable Effects.
- **Not unit-tested — manual smoke only** (thin I/O adapters needing a live interactive desktop; the I/O-layer definition of done):
  1. **Windowless:** launch → no console, no taskbar button, no Alt-Tab entry; visible only in Task Manager.
  2. **Adoption:** with 3 desktops open, start winspace, press `$mod+2` → lands on the *existing* 2nd desktop (no new desktop spawned).
  3. **Create-on-demand:** press `$mod+5` (only 3 exist) → exactly one new desktop appears at the tail and activates; `$mod+4` after creates one more (no intermediate filling, no clamp).
  4. **GUID-anchored stability:** reorder desktops in Task View, press `$mod+5` again → lands on the *same* desktop created earlier.
  5. **Quit:** `quit` Bind → process exits cleanly; no orphan desktops beyond those the test created.
  6. **Variant diagnostic:** on the 24H2 machine the log names the resolved 24H2+ IID; forcing a stubbed variant emits the loud "not yet implemented" line.

## Out of Scope

- Window tracking, the eligibility gate, and fill-one behavior (issue 02) — no `SetWinEventHook`, no event-hook thread in this slice.
- All tiling: BSP layout, multi-display fill/float, spatial focus/move (issues 03–05).
- Move-to-workspace, rules, cloak-move placement, place-once behaviors, launcher (issues 06–08).
- The full config grammar, `windowrule`, `section {}`, live `reload` and last-good-config retention (issue 09).
- Autostart / Task Scheduler registration (issue 10).
- **Reaping** of empty, unfocused workspaces — deferred (needs window tracking to detect "empty"); the GUID-anchored model here is what makes it safe later.
- **Mid-run reconciliation** of desktops created/destroyed outside winspace (Task View) — a separate deferred policy; this slice only does live position resolution on switch.
- Implementing and verifying the **21H2** and **23H2-KB5034204+** vtable variants — declared and stubbed only; implemented when reachable on those builds.
- IPC / CLI surface.

## Further Notes

- **Deliberately more than the minimum.** The walking skeleton carries a real GUID-anchored map, adoption, and live position resolution instead of a trivial `index = N-1`. That extra weight is chosen so the skeleton is a *truthful template* for reaping and later slices, not a lie that gets ripped out. This is the ADR 0003 stance.
- **Hardest integration first.** The COM Virtual Desktop bridge (undocumented, per-build vtable) is the single highest-risk dependency; the skeleton exists partly to prove it can be driven on the target machine before any product value is layered on.
- **Performance thesis.** Responsiveness under load comes from architecture, not priority: `RegisterHotKey` (kernel-delivered, no input-hook timeout), the Worker off the input critical path, and never escalating scheduler priority. The GlazeWM stutter this avoids was specifically `WH_KEYBOARD_LL` timeouts.
- **Consumption is automatic.** A matched `RegisterHotKey` combo is swallowed by the system (the foreground app never sees it) with no per-key propagation logic — unlike the `WH_KEYBOARD_LL` swallow/`CallNextHookEx` decision that is the stutter mechanism. Only the exact bound combo is consumed; everything else passes through.
