# 2. Workspaces are OS Virtual Desktops via the undocumented internal COM interface

**Status:** Accepted (2026-07-03)

## Context

winspace needs a notion of "workspace" the user switches between as a unit. Two broad
strategies exist on Windows:

- **Userspace show/hide** — winspace owns the illusion, hiding/showing windows itself
  (the FancyZones-adjacent approach). Full control, but taskbar, Alt-Tab, and focus do
  *not* naturally follow, so we would be reimplementing shell behavior and fighting it.
- **OS Virtual Desktops** — a winspace workspace *is* a Windows Virtual Desktop, so
  taskbar grouping, Alt-Tab scoping, and focus "just work."

The catch: the only interface that can *switch* and *enumerate* desktops,
`IVirtualDesktopManagerInternal`, is **undocumented**. The public `IVirtualDesktopManager`
lacks `SwitchDesktop`/`GetDesktops`. Worse, Microsoft **bumps the interface IID whenever
the vtable changes**, and has done so *within* a single build (KB5034204 shipped a new IID
inside build 22631), so build-number detection is insufficient to pick the right vtable.

## Decision

Back every workspace with an OS Virtual Desktop through `IVirtualDesktopManagerInternal`.

- **Acquisition:** `CoCreateInstance(CLSID_ImmersiveShell, IID_IServiceProvider)` →
  `QueryService(CLSID_VirtualDesktopManagerInternal, <probed IID>)`.
- **Variant selection is IID-probe, self-validating — not a version table.**
  `QueryInterface` each known IID newest→oldest; the first `S_OK` wins, because
  QI-success ⟺ correct vtable (the IID bump *is* the version signal). Build/UBR are read
  only for diagnostic logging.
- **Interfaces are hand-declared `MIDL_INTERFACE` structs per variant**, IIDs and vtable
  orderings sourced from the community RE lineage (komorebi / GlazeWM /
  VirtualDesktopAccessor), each annotated with the build/KB it was captured on. This
  community source *is* the source of truth — Microsoft documents none of it.
- **All COM lives behind `IVirtualDesktopBridge`**, which speaks winspace vocabulary;
  `IVirtualDesktop*` / `IObjectArray*` / `HRESULT` never escape the bridge. Only the
  24H2+ variant is implemented/verified first (the dev machine); 21H2 and 23H2-KB
  variants are declared + a loud "not yet implemented" stub.

## Consequences

- Taskbar, Alt-Tab, and focus scoping come for free; winspace does not fight the shell.
- We take a hard dependency on undocumented, RE-sourced ABI. A future Windows build with
  a new IID/vtable requires capturing a new variant — but the IID-probe *fails loudly*
  (returns null + diagnostic) rather than calling through a wrong vtable, so breakage is
  detectable, not silent-corruption.
- The bridge seam keeps the blast radius contained: the rest of winspace is oblivious to
  COM.

## Alternatives rejected

- **Userspace show/hide illusion** — loses native taskbar/Alt-Tab/focus integration;
  contradicts the north star of leaning on OS-native facilities.
- **Build-number → vtable table** — provably insufficient (KB5034204 changed the IID
  mid-build). IID-probe subsumes it.
- **Raw vtable-ordinal indexing** — brittle and unreadable versus hand-declared
  `MIDL_INTERFACE` structs.
