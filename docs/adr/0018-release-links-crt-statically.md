# The Release build links the CRT statically (`/MT`)

**Status:** Accepted (2026-07-19 — reverses the `/MD` posture recorded in `PROVISIONING.md`
item 5).

To make the distributable self-contained, the **Release** build links the C runtime
**statically** (`/MT` instead of `/MD`). The exe grows by a couple hundred KB and gains zero
runtime dependencies — no VC++ redistributable, no `depends` in the Scoop manifest, and
`PROVISIONING.md`'s redist step goes away. Debug stays `/MDd`.

## Context

The Release build previously linked the CRT dynamically (`/MD`), so a clean machine needed
`VCRUNTIME140.dll` present — which is why the VM harness provisions the VC++ x64 redist
(`PROVISIONING.md` item 5) and why a naive Scoop package would need a redist `depends`
against the `extras` bucket. Distributing via Scoop ([ADR-0017](0017-distribution-via-self-bucketed-scoop-package.md))
makes that dependency a user-facing wart: cross-bucket `depends` is friction, and a
missing-DLL dialog on first run is a bad out-of-box experience.

The relevant facts that make static linking cheap here:

- winspace is a **single exe with no plugin DLLs**, so the one thing `/MD` buys — sharing one
  CRT copy across several modules — is worth nothing.
- The **Universal CRT (`ucrtbase.dll`) is a component of Windows itself** since Windows 10, so
  even under `/MD` the only truly-missing piece on a clean modern box is the small
  `VCRUNTIME140.dll`. The redist exists to supply essentially that one DLL.
- Static linking the MSVC CRT pulls in **only the referenced objects**, not "the whole
  runtime." Measured on this codebase, the Release exe goes from **448 KB (`/MD`) to
  ~650 KB (`/MT`)** — +~200 KB, still under 1 MB.

## Decision

Release compiles with `/MT` (and drops `/DNDEBUG`-adjacent nothing else); Debug keeps `/MDd`
for the faster incremental/edit-continue story and because Debug is never distributed
(`build.ps1` already refuses to deploy a `/MDd` binary). The Scoop manifest declares **no**
`depends`, and `PROVISIONING.md` item 5 is removed.

## Considered Options

- **`/MT` static CRT for Release.** *Chosen.* ~200 KB buys a fully self-contained exe that
  runs on any Win10/11 box with nothing installed — no bucket juggling, no redist step, no
  first-run DLL dialog.
- **Keep `/MD` + declare `extras/vcredist2022` as a Scoop `depends`.** *Rejected.* Saves
  ~200 KB but requires the user to have the `extras` bucket added and inherits cross-bucket
  `depends` friction — trading a trivial size win for real install-time fragility.
- **A no-CRT build (`/NODEFAULTLIB` + custom entry point).** *Rejected (out of scope.)* Would
  yield a binary well under the current 448 KB, but it is a large, separate undertaking with
  its own hazards — unrelated to "make winspace installable."

## Consequences

- The Scoop artifact is one self-contained ~650 KB exe; the package needs no `depends` and the
  VM snapshot no longer needs the redist pre-installed.
- A CRT security fix now requires **rebuilding and re-releasing** winspace rather than the user
  picking it up via a redist update. Acceptable for a per-user hobby-scale WM, and the CI
  release path (ADR-0017) makes a rebuild-and-tag cheap.
- Debug and Release now differ in CRT linkage as well as optimization; the purity-check
  rebuild in `build.ps1` is unaffected (it turns on OS-call link errors via the WM-library
  split, independent of `/MT` vs `/MD`).
