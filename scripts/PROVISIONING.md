# Provisioning the `winspace-e2e` snapshot

The VM seam-test harness ([ADR-0005](../docs/adr/0005-vm-seam-test-harness.md)) drives the real
Release `winspace.exe` inside a throwaway VM and asserts against independent OS state. The VM is
**`win11-24h2`**; the host orchestrator (`Invoke-SmokeSeams.ps1`) reverts to a snapshot named
**`winspace-e2e`** at the start of every run — a pristine, **stock** Windows 11 baseline (no
registry policy of any kind). That snapshot must be captured **logged in and ready**. This is the
one-time runbook. (`vmctl exec`/`cp` below take the VM name as their first positional; substitute
your VM's name if it differs.)

Everything below is load-bearing: a snapshot captured at the lock screen, or without VMware Tools,
silently breaks every Trigger. The harness fails **loud** when it can (Release gate, interactive-session
gate), but the items here are its assumptions.

## What the snapshot must contain

| # | Requirement | Why |
|---|-------------|-----|
| 1 | **Windows 11 24H2 (build 26100)** | The only Virtual Desktop COM variant winspace implements (ADR-0002). Older builds log "not yet implemented" and disable VD switching. |
| 2 | **VMware Tools** installed and running | `vmctl cp` / `exec` ride the Tools (VIX) backend. No Tools → no deploy, no runner. |
| 3 | **Autologon enabled** | After a snapshot revert the VM restarts; without autologon it sits at the lock screen with **no interactive session**, and `SendInput` has nowhere to inject. |
| 4 | **Pester v5** installed for the guest user | The Guest runner (`Invoke-WinspaceSeams`) runs the seam files through Pester and emits JUnit XML. |
| 5 | **`C:\winspace-e2e\` directory exists** | The deploy root the host copies the exe + guest module into. |
| 6 | **1-desktop baseline** | Each seam stages its own precondition from a known floor; the create-on-demand seam drives 1 → 3 desktops, then asserts a +1 delta. |
| 7 | **`vmctl auth` credentials registered on the host** | Guest ops (`cp`, `exec`) authenticate with the guest user. Registered on the **host**, not baked into the snapshot. |
| 8 | **No hotkey policy — stock shell** | The seeded default is `$mod = ALT`; `Alt+<n>` / `Alt+Shift+<n>` / `Alt+<letter>` all register via `RegisterHotKey` on an **unmodified** Windows 11 (ADR-0014). Nothing needs freeing — do **not** apply `NoWinKeys` or any other policy. The native VD hotkeys (`Win+Ctrl+D` / `Win+Ctrl+F4`) the seams use to stage desktops are OS-level and always available. |

> No **VC++ redistributable** is needed: the Release build links the CRT statically (`/MT`,
> [ADR-0018](../docs/adr/0018-release-links-crt-statically.md)), so the deployed exe is
> self-contained. (Debug `/MDd` is rejected host-side and never deployed.)

## One-time steps

Run these **inside the guest** (via console or `vmctl exec`), unless marked *(host)*.

### 1. VMware Tools
Install from the VMware Workstation menu (**VM → Install VMware Tools**) and reboot. Confirm from the
host:

```powershell
vmctl inspect win11-24h2            # Tools should report running
```

### 2. Autologon
Set the auto-logon registry values (replace the user/password with the guest account):

```powershell
$k = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Set-ItemProperty $k AutoAdminLogon '1'
Set-ItemProperty $k DefaultUserName 'test'
Set-ItemProperty $k DefaultPassword 'test'
```

Reboot and confirm the desktop comes up **without** a manual login. (Alternatively use Sysinternals
`Autologon64.exe`, which stores the password as an LSA secret rather than plaintext.)

### 3. Pester v5

```powershell
Install-Module Pester -MinimumVersion 5.0 -Force -SkipPublisherCheck -Scope AllUsers
Import-Module Pester -MinimumVersion 5.0    # confirm it loads
```

### 4. Deploy root + 1-desktop baseline

```powershell
New-Item -ItemType Directory -Force -Path 'C:\winspace-e2e'
# Collapse to a single Virtual Desktop: close extras with Win+Ctrl+F4 until one remains.
```

### 5. Register credentials *(host)*

```powershell
vmctl auth set win11-24h2 --user test --password test
```

### 6. Capture the baseline snapshot — **logged in**
With the guest booted, auto-logged-in, sitting on a single desktop:

```powershell
vmctl snapshot commit win11-24h2 winspace-e2e -m "e2e baseline: autologon, Tools, Pester, 1 desktop"
```

The snapshot **name** must be `winspace-e2e` (the orchestrator's default `-Snapshot`). This is the
pristine, stock baseline the harness reverts to — no policy layered on top.

Verify: after `vmctl snapshot reset win11-24h2 winspace-e2e`, `RegisterHotKey(MOD_ALT,'1')` must
return `OK` in an interactive (`-it`) session — with **no** registry policy applied. That is the
whole point of `$mod = ALT` (ADR-0014): winspace binds on a stock Windows 11.

> **End users:** because winspace defaults to `$mod = ALT`, a real install needs **no** setup — the
> `Alt+<n>` / `Alt+Shift+<n>` chords register out of the box. The tradeoff (documented in the seeded
> config header, `src/win32.cpp`) is that these global Alt chords shadow the focused app's own
> `Alt+<key>` shortcuts while winspace runs; rebind if that bites.

## Running the harness *(host)*

```powershell
.\build.ps1 -Config release                          # produce build\release\winspace.exe
.\scripts\Invoke-SmokeSeams.ps1                        # revert once, run all seams, summarise
.\scripts\Invoke-SmokeSeams.ps1 -Seam create-on-demand # a single seam by tag
.\scripts\Invoke-SmokeSeams.ps1 -SkipReset            # reuse current (already-clean) state
.\scripts\Invoke-SmokeSeams.ps1 -Fresh                # bulletproof: revert per live seam
```

A green run over the real Release binary is the proof the harness works end-to-end — there is no
unit test for the plumbing (ADR-0005). The suite covers the six workspace-switch Smoke seams, the two
error-handling runtime seams (`formaterror-quality`, `degrade-dont-crash`), and the six window-tracking
Smoke seams (`fill-one`, `adoption-fill`, `reclaim`, `ineligible`, `cloaked-uwp`, `clean-unhook`), with
issues 03–10 reported as skipped scaffolds. `results.xml`, `run.log`, and any `failure-*.png` are copied
back next to the script.

### Default vs `-Fresh` isolation

- **Default** — revert the `winspace-e2e` snapshot **once**, then run every seam in one interactive
  guest session. Each seam stages its own desktop-count precondition in-guest (native VD hotkeys),
  so VD state does not leak between seams. Fast; this is what CI runs.
- **`-Fresh`** — revert the snapshot **per live seam** and re-invoke the guest runner once per seam
  via its `-Seam` filter, so no seam can inherit dirty state from another. Pays a boot +
  interactive-logon wait per seam; use it when dirty state is suspected. The skipped 03–10 scaffolds
  touch no VM and are collected in one final non-reverting pass; the per-seam JUnit XMLs
  (`results.<seam>.xml`) are merged into `results.xml` for the summary. `-Fresh` discovers the live
  seam set from the guest (Pester discovery), so a newly-dropped seam file needs no orchestrator edit.

## Notes

- **`cp`, not `push`.** The host deploys with `vmctl cp` (VMware Tools, single-file), not `vmctl push`
  (SSH/SFTP), so the snapshot needs only Tools — no OpenSSH server.
- **Config is self-seeding.** On first launch winspace writes its default config
  (`%USERPROFILE%\.config\winspace\winspace.conf`) with `$mod = SUPER` and `bind`s for
  workspaces 1–5 + quit — exactly what the seams drive (and why NoWinKeys is required). No config
  needs pre-staging.
- **One snapshot, all modes.** The snapshot provisioned here satisfies the full suite (workspace-switch +
  error-handling) in both default and `-Fresh` modes — no per-mode setup. The degrade-don't-crash seam
  spawns its own conflicting-hotkey helper (a background `powershell.exe` holding `Win+1`, winspace's
  first seeded bind) inside the guest; no extra provisioning is needed for it.
