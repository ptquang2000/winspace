# Distribution is a self-bucketed Scoop package fed by a CI release-on-tag

**Status:** Accepted (2026-07-19).

winspace needs a way for users to install it. We distribute it as a **Scoop** package whose
**manifest lives in this repo** (the repo doubles as its own Scoop bucket), pointing at a
**zipped release exe attached to a GitHub Release** that a **tag-triggered GitHub Actions**
workflow builds, hashes, and publishes. This ADR records why that shape, over the obvious
alternatives, for a hand-built MSVC binary with no releases yet.

## Context

winspace is a single windowless `winspace.exe` built by `build.ps1` invoking `cl.exe`
directly — no CMake, no vcpkg, no package manager (the North Star: own only what Windows
won't give us). At the time of this decision there are **zero tags and zero releases**, so
the distribution story is greenfield: version scheme, artifact, delivery channel, and update
mechanism are all being chosen at once.

The constraints that steer the choice:

1. **The artifact is a compiled MSVC binary.** Building it needs the x64 Native Tools
   environment (`cl.exe` on PATH). An end user's machine cannot be assumed to have it, so a
   *build-from-source-at-install* channel is off the table.
2. **Scoop's update model moves the binary.** Scoop installs to a **versioned** directory
   (`~\scoop\apps\winspace\<version>\`) with a `current` junction, and `scoop update`
   **deletes the old versioned dir**. This interacts with autostart (ADR-0013 bakes an
   absolute exe path into the Logon task) and with the running process's file lock — handled
   in [ADR-0019](0019-single-instance-orchestrator-and-control-channel.md), not here.
3. **Solo project, early days.** No community-bucket review process is worth taking on before
   the first release exists.

## Decision

- **Channel: Scoop, self-bucketed.** The manifest is `bucket/winspace.json` **in this repo**
  — a Scoop bucket is just a git repo of `*.json`, so the repo is its own bucket. Users add
  it (`scoop bucket add winspace https://github.com/ptquang2000/winspace`) and
  `scoop install winspace`. No second repo, one source of truth.
- **Artifact: a zipped release exe on a GitHub Release.** The manifest URL fetches a zip
  containing the release-built `winspace.exe`; its SHA256 is recorded in the manifest.
- **Version: SemVer, pre-1.0, first tag `v0.0.1`** — honest about maturity; minor for
  features, patch for fixes.
- **Build & publish: GitHub Actions on tag push.** Pushing `vX.Y.Z` runs `windows-latest`,
  `build.ps1 -Config release`, zips the exe, computes the hash, and creates the Release with
  the asset. Free — the repo is **public**, where Actions minutes are unlimited.
- **Manifest automation: `checkver` + `autoupdate`** off the GitHub releases, so a new release
  regenerates the manifest `version`/`url`/`hash` instead of a hand-edited SHA256.
- **Exposure: `bin` shim only, no Start Menu shortcut, no auto-launch on install.** The shim
  puts `winspace` (and its subcommands, ADR-0019) on PATH; installing does not start the WM
  or seize hotkeys.
- **No `persist`.** Config lives at `%USERPROFILE%\.config\winspace\winspace.conf`, outside
  `~\scoop\`, self-seeded on first run — it already survives install/update/uninstall.

## Considered Options

- **Self-bucketed manifest in this repo.** *Chosen.* One repo, full control of cadence, no
  external review gate. A dedicated `scoop-winspace` bucket repo can be split out later
  without changing the artifact story if adoption warrants it.
- **Submit to a community bucket (`scoop-extras`).** *Rejected (for now.)* Widest reach, but
  inherits their review process and quality bar and cedes cadence control — premature with no
  releases yet.
- **Build-from-source at install time.** *Rejected.* Scoop can run a build, but winspace's
  build needs the MSVC toolchain, which an end user's box cannot be assumed to have. A
  non-starter for a compiled MSVC app.
- **Bundle the CRT DLLs / other runtimes in the zip.** *Rejected* and made moot by
  [ADR-0018](0018-release-links-crt-statically.md): the release links the CRT statically, so
  the zip is a single self-contained exe with no runtime to bundle.
- **Manual releases (build locally, `gh release create`, hand-paste the hash).** *Rejected.*
  The release build needs a real toolchain and a correct SHA256 — exactly the error-prone
  steps CI removes for free on a public repo. `checkver`/`autoupdate` close the same loop on
  the manifest side.

## Consequences

- The whole release path is `git tag vX.Y.Z && git push --tags` → CI publishes → `checkver -u`
  regenerates the manifest. No hand-computed hashes.
- Because the manifest is in-repo, the bucket and the source share a history — a release tag,
  its CI artifact, and the manifest that points at it are all one repo.
- Scoop's versioned-dir/`current`-junction layout and its file-swap-on-update behavior are
  load-bearing for autostart correctness and the running-process file lock; those are the
  subject of [ADR-0019](0019-single-instance-orchestrator-and-control-channel.md).
- `PROVISIONING.md`'s redist step is dropped by [ADR-0018](0018-release-links-crt-statically.md),
  not here — but the self-contained artifact is what lets the Scoop package declare **no**
  `depends`.
