# 01.06 — COM Virtual Desktop bridge (24H2) + sparse model

**Labels:** `ready-for-agent`
**Blocked by:** 01.04
**Respects:** `docs/adr/0002`, `docs/adr/0003`

## What to build

The hardest integration, quarantined behind one abstraction. `IVirtualDesktopBridge` is a
pure-abstract interface speaking winspace vocabulary — Logical workspace numbers in,
switches out. `IVirtualDesktop*` / `IObjectArray*` / `HRESULT` never escape it. The Worker
executes the `SwitchToWorkspace{logical}` Effect by calling the bridge.

- **Acquisition** — `CoCreateInstance(CLSID_ImmersiveShell, IID_IServiceProvider)` →
  `QueryService(CLSID_VirtualDesktopManagerInternal, <probed IID>)`.
- **Variant selection — IID-probe, self-validating.** `QueryInterface` each known IID
  newest→oldest; first `S_OK` wins (QI-success ⟺ correct vtable). Read build/UBR only for
  diagnostic logging. If none match → return null + a loud diagnostic (fail closed, never
  call through a wrong vtable).
- **Interfaces** — hand-declared `MIDL_INTERFACE` structs per variant; IIDs/vtable
  orderings from the community RE lineage, annotated by build/KB. **Implement and verify
  only the 24H2+ variant** (dev build 26100.8655): `GetDesktops`, `IVirtualDesktop::GetID`,
  `CreateDesktop`, `SwitchDesktop`. Declare 21H2 and 23H2-KB5034204+ with a stub that
  emits a loud "variant not yet implemented" diagnostic.
- **Sparse/virtual model** — a Logical workspace number maps to a Virtual Desktop **by
  GUID, not array position**. **Adoption** at startup: bind existing desktops to `1..N` by
  GUID; seed `current_workspace` from the active desktop. **Switch:** hit → resolve stored
  GUID to its current live array index → `SwitchDesktop`; miss → `CreateDesktop` (one,
  appended) → bind logical→GUID → switch. No intermediate filling, no clamp.

## Acceptance criteria

- [ ] Bridge speaks only Logical workspace numbers; no COM type escapes it
- [ ] IID-probe selects the 24H2+ variant on the dev machine; log names the resolved IID
- [ ] No matching IID → null + loud diagnostic; no call through a mismatched vtable
- [ ] Adoption binds pre-existing desktops to `1..N` by GUID; current workspace seeded from active desktop
- [ ] Switch hit resolves GUID→live index (survives Task View reorder); miss creates exactly one desktop
- [ ] Forcing a stubbed variant emits the "not yet implemented" diagnostic
- [ ] Not unit-tested — verified via task 07's smoke script
