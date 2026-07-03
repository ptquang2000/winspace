# 3. Sparse/virtual workspace model over the dense-positional OS primitive

**Status:** Accepted (2026-07-03)

## Context

Windows Virtual Desktops are a **dense, ordered array** identified by **GUID**: there are
no gaps, and deleting one shifts the positions of every desktop after it. winspace wants
an **i3/hyprland-style dynamic model** (per `DESIGN.md`): workspace numbers are labels
materialized on demand, and the user wants empty, unfocused workspaces destroyed
("reaped") so junk desktops don't accumulate.

The naive mapping — *workspace N ≡ the Nth desktop* (dense/positional) — has two bad
properties: pressing `workspace 5` from empty leaves four junk desktops forever, and it
needs a clamp hack so a fat-fingered `workspace 500` doesn't spawn 500 desktops. It is
also fundamentally incompatible with reaping: destroying a middle desktop renumbers every
position after it, so a position-anchored model cannot preserve stable workspace numbers.

## Decision

Adopt a **sparse/virtual** workspace model.

- A **logical workspace number** (1, 2, 3, …) is mapped to a Virtual Desktop **by GUID**,
  never by array position.
- **Switch:** hit → resolve the stored GUID to its *current* array index (live), then
  `SwitchDesktop`. Miss → `CreateDesktop` (one desktop, appended at the tail), bind the
  logical number to its GUID, switch. **No intermediate filling** (`workspace 5` creates
  one desktop, not 1..4) and **no clamp** (`workspace 500` = one desktop labeled 500).
- **Startup adoption:** existing desktops are bound to logical `1..N` by GUID; the active
  desktop seeds `current_workspace`.
- **The `logical→GUID` map, adoption, create-on-demand, and GUID→position resolution all
  live in the bridge (I/O side), never in the pure reducer.** The reducer reasons only in
  logical numbers and stays free of `windows.h` (a GUID is a Windows type). This is the
  same translation-adapter boundary used for `Mod→MOD_*` and `Key→VK_*`.
- **Reaping is deferred** (it needs window tracking to know "empty"), but this model is
  precisely what makes it safe: because identity is GUID-anchored, reaping a middle
  desktop renumbers OS positions without disturbing winspace's logical numbering.

## Consequences

- Eliminates junk intermediate desktops and the clamp hack; matches the stated
  i3/hyprland design intent.
- The walking skeleton carries a real GUID-anchored map and live position resolution
  instead of a trivial `index = N-1` — deliberately more than the minimum, chosen so the
  skeleton is a truthful template rather than a lie ripped out when reaping lands.
- Live position resolution means winspace tolerates user-driven reordering (Task View
  drag) without drift. Reconciliation of desktops *created* outside winspace mid-run is a
  separate deferred policy.

## Alternatives rejected

- **Dense/positional (`workspace N` ≡ Nth desktop, create intermediates, clamp to ~20)** —
  accumulates junk desktops, needs a clamp hack, and is incompatible with reaping because
  it cannot preserve logical numbers across mid-array deletion.
- **Map stored as array position instead of GUID** — silently points at the wrong desktop
  after any reap or reorder.
- **Map owned by the reducer** — would drag Windows `GUID` types into core (violating the
  `windows.h`-free rule) and force an I/O feedback round-trip to bind newly created
  desktops.
