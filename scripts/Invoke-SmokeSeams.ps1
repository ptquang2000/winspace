<#
.SYNOPSIS
    Host orchestrator for the winspace VM seam-test harness (ADR-0005).

.DESCRIPTION
    Stages the winspace-e2e VM around one Guest runner invocation and reports
    pass/fail. It talks to the VM ONLY through the `vmctl` CLI and does suite-level
    lifecycle only — all Triggers, Oracle reads, and assertions happen in-guest
    (see scripts/guest/WinspaceTest.psm1), because SendInput / registry / window
    reads work only inside the interactive session.

    The spine (host -> guest):

        assert exe is Release (reject the /MDd debug CRT — non-redistributable)
        vmctl snapshot reset winspace-e2e     (clean baseline, once)
        vmctl cp   winspace.exe  + guest module  ->  C:\winspace-e2e\
        vmctl exec -it  <run the Guest runner in the interactive session>
        vmctl cp   results.xml (+ run.log, screenshots) back  ->  summarise

    As of 12.04 it drives all six workspace-switch Smoke seams (create-on-demand, windowless,
    adoption, guid-stability, quit, variant-diagnostic) plus the two error-handling runtime
    seams (formaterror-quality, degrade-dont-crash). As of 02.06 it also drives the six
    window-tracking Smoke seams (fill-one, adoption-fill, reclaim, ineligible, cloaked-uwp,
    clean-unhook), with issues 03–10 present as skipped scaffolds.

    Isolation has two modes (ADR-0005):
      * default  — revert the snapshot ONCE, then run every seam in a single guest
                   session (each seam stages its own precondition in-guest).
      * -Fresh   — bulletproof: revert the snapshot per LIVE seam and re-invoke the
                   guest runner once per seam via its -Seam filter, so no seam can
                   leak VD state into the next. The skipped 03–10 scaffolds need no VM
                   and are collected in one final non-reverting pass. The per-seam
                   JUnit XMLs are merged into one results.xml for the summary.

.PARAMETER Seam
    Run a single seam by its Pester tag (e.g. create-on-demand). Omit to run all.

.PARAMETER Fresh
    Force a full snapshot revert per live seam (see above). Default is revert-once.

.PARAMETER SkipReset
    Skip the snapshot revert (faster local re-runs against already-clean state).
    Ignored under -Fresh (which reverts per seam by definition).

.EXAMPLE
    .\scripts\Invoke-SmokeSeams.ps1
    .\scripts\Invoke-SmokeSeams.ps1 -Seam create-on-demand
    .\scripts\Invoke-SmokeSeams.ps1 -Fresh
#>
[CmdletBinding()]
param(
    [string]$Vm = 'win11-24h2',
    [string]$Snapshot = 'winspace-e2e-nowinkeys',
    [string]$Seam,
    [string]$ExePath = (Join-Path $PSScriptRoot '..\build\release\winspace.exe'),
    [string]$GuestRoot = 'C:\winspace-e2e',
    [switch]$Fresh,
    [switch]$SkipReset
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

# ── vmctl shim: run the CLI and throw loud on a non-zero exit ─────────────────
# NB: a plain (non-advanced) function using the automatic $args on purpose — a
# param([Parameter(ValueFromRemainingArguments)]) block makes this an advanced
# function, which grafts on the common parameters, and then a vmctl flag like
# `-o` (overwrite) binds as an ambiguous prefix of -OutVariable/-OutBuffer
# instead of passing through. $args forwards every token to vmctl verbatim.
function Invoke-Vmctl {
    Write-Host "  vmctl $($args -join ' ')" -ForegroundColor DarkGray
    $out = & vmctl @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host $_ }
        throw "vmctl $($args -join ' ') failed (exit $LASTEXITCODE)"
    }
    return $out
}

# ── preflight: vmctl tooling + VM reachability ───────────────────────────────
# Fail early and legibly on the environment assumptions the whole run rests on:
# vmctl on PATH, valid auth, the VM present + running, and the guest responding.
# A single `exec -t` liveness ping validates all four at once (it launches no
# winspace, so it is safe to run before the snapshot reset). Without this, a
# missing vmctl or an expired auth surfaces as a raw error midway through the
# first real vmctl call (`snapshot reset`) instead of an actionable message here.
function Assert-Environment {
    if (-not (Get-Command vmctl -ErrorAction SilentlyContinue)) {
        throw "vmctl not found on PATH — the harness talks to the VM only through it. " +
              "Install the vmctl CLI and ensure it is on PATH."
    }
    $probe = & vmctl exec -t $Vm "'winspace-preflight'" 2>&1
    if ($LASTEXITCODE -ne 0 -or ($probe -notmatch 'winspace-preflight')) {
        $detail = ($probe | Out-String).Trim()
        throw "vmctl cannot reach guest VM '$Vm' (exit $LASTEXITCODE). Confirm the VM exists and is " +
              "running, and that vmctl auth is valid (run ``vmctl auth``).`n$detail"
    }
    Write-Host "  Environment: vmctl OK, guest '$Vm' reachable" -ForegroundColor Green
}

# ── loud gate: reject a non-Release exe before it ever crosses to the guest ───
# A Debug build links the debug CRT (/MDd -> VCRUNTIME140D.dll / ucrtbased.dll /
# MSVCP140D.dll), which is non-redistributable and won't run in a clean guest.
# Import DLL names sit as ASCII in the PE import table, so a byte scan is enough
# — no dumpbin dependency.
function Test-IsReleaseExe {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($m in 'VCRUNTIME140D.dll', 'ucrtbased.dll', 'MSVCP140D.dll') {
        if ($ascii -match [regex]::Escape($m)) { return $false }  # -match is case-insensitive
    }
    return $true
}

# ── summarise the JUnit XML the guest emitted ────────────────────────────────
function Write-Summary {
    param([string]$XmlPath)
    [xml]$doc = Get-Content -Path $XmlPath -Raw
    # SelectSingleNode/GetAttribute throughout: dotted access to a missing XML
    # child or attribute throws under Set-StrictMode -Version Latest (a passing
    # <testcase> has no <failure> child), so never reach for one that may be absent.
    $tests = 0; $failures = 0; $errors = 0; $skipped = 0
    foreach ($s in $doc.SelectNodes('//testsuite')) {
        $asInt = { param($n) $v = $s.GetAttribute($n); if ($v) { [int]$v } else { 0 } }
        $tests += (& $asInt 'tests'); $failures += (& $asInt 'failures')
        $errors += (& $asInt 'errors'); $skipped += (& $asInt 'skipped')
    }
    Write-Host ''
    Write-Host '── seam results ─────────────────────────────' -ForegroundColor Cyan
    foreach ($tc in $doc.SelectNodes('//testcase')) {
        $failNode = $tc.SelectSingleNode('failure')
        $errNode = $tc.SelectSingleNode('error')
        $failed = $null -ne $failNode -or $null -ne $errNode
        $isSkip = $null -ne $tc.SelectSingleNode('skipped')
        $tag = if ($isSkip) { 'SKIP' } elseif ($failed) { 'FAIL' } else { 'PASS' }
        $color = if ($isSkip) { 'Yellow' } elseif ($failed) { 'Red' } else { 'Green' }
        Write-Host '  ' -NoNewline
        Write-Host $tag -ForegroundColor $color -NoNewline
        Write-Host "  $($tc.GetAttribute('name'))"
        if ($failed) {
            $msg = ($failNode ?? $errNode).GetAttribute('message')
            if ($msg) { Write-Host "        $(($msg -split "`n" | Select-Object -First 1))" -ForegroundColor Red }
        }
    }
    $passed = $tests - $failures - $errors - $skipped
    Write-Host '─────────────────────────────────────────────' -ForegroundColor Cyan
    $ok = ($failures -eq 0 -and $errors -eq 0)
    $line = "  total $tests  |  passed $passed  |  failed $failures  |  errors $errors  |  skipped $skipped"
    Write-Host $line -ForegroundColor ($ok ? 'Green' : 'Red')
    return $ok
}

# ── deploy: exe + the whole guest tree (module + *.Tests.ps1 + scaffold/) ─────
# cp = VMware Tools (no SSH dependency). Recurse so guest\scaffold\*.Tests.ps1
# come across too; each file's subdir is created before its copy so the tree is
# mirrored exactly under $GuestRoot\guest.
function Invoke-Deploy {
    param([string]$Exe)
    $guestDir = Join-Path $PSScriptRoot 'guest'
    Invoke-Vmctl exec -t $Vm "New-Item -ItemType Directory -Force -Path '$GuestRoot\guest' | Out-Null" | Out-Null
    Invoke-Vmctl cp -o $Exe "${Vm}:$GuestRoot\winspace.exe" | Out-Null
    Get-ChildItem -Path $guestDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($guestDir.Length).TrimStart('\', '/')
        $dest = "$GuestRoot\guest\$rel"
        $destDir = Split-Path -Parent $dest
        if ($destDir -ne "$GuestRoot\guest") {
            Invoke-Vmctl exec -t $Vm "New-Item -ItemType Directory -Force -Path '$destDir' | Out-Null" | Out-Null
        }
        Invoke-Vmctl cp -o $_.FullName "${Vm}:$dest" | Out-Null
    }
}

# ── wrap a guest command so it runs under a child powershell with the policy ──
# bypass supplied as a *launch flag*. The deployed *.psm1 / *.Tests.ps1 are
# unsigned, so a Restricted/AllSigned guest policy refuses to load them. Running
# powershell.exe with an inline -Command is not itself gated by execution policy
# (only loading script files is), and -ExecutionPolicy governs ONLY this child
# process — nothing persisted, no session state, no Set-ExecutionPolicy call.
# Inner commands use single-quoted paths, so the double-quoted -Command is clean.
function ConvertTo-BypassLaunch {
    param([string]$InnerCmd)
    return "powershell -NoProfile -ExecutionPolicy Bypass -Command `"$InnerCmd`""
}

# ── run the guest runner in the interactive session, copy its XML back ────────
# Returns the local path of the copied-back JUnit XML (or $null if none came back).
function Invoke-GuestRunner {
    param([string]$SeamTag, [string]$LocalXml)
    $guestXml = "$GuestRoot\results.xml"
    $seamArg = if ($SeamTag) { " -Seam '$SeamTag'" } else { '' }
    $inner = "Import-Module '$GuestRoot\guest\WinspaceTest.psm1' -Force; " +
             "Invoke-WinspaceSeams -TestPath '$GuestRoot\guest' -ResultsPath '$guestXml'$seamArg"
    $guestCmd = ConvertTo-BypassLaunch -InnerCmd $inner
    # exec -it propagates the guest exit code; a failing suite is surfaced by the XML
    # summary, so swallow the non-zero here rather than aborting the copy-back.
    try { Invoke-Vmctl exec -it $Vm $guestCmd } catch { Write-Host $_.Exception.Message -ForegroundColor DarkYellow }
    try { Invoke-Vmctl cp -o "${Vm}:$guestXml" $LocalXml | Out-Null } catch { return $null }
    return (Test-Path $LocalXml) ? $LocalXml : $null
}

# ── -Fresh discovery: ask the guest for the live (non-skipped) seam tags ──────
# Pester discovery in-guest (no winspace launch); the runner prints `TAGS:a,b,c`.
function Get-LiveSeamTags {
    $inner = "Import-Module '$GuestRoot\guest\WinspaceTest.psm1' -Force; " +
             "'TAGS:' + ((Get-WinspaceLiveSeamTags -TestPath '$GuestRoot\guest') -join ',')"
    $cmd = ConvertTo-BypassLaunch -InnerCmd $inner
    $out = Invoke-Vmctl exec -t $Vm $cmd
    $line = ($out -split "`r?`n" | Where-Object { $_ -match '^TAGS:' } | Select-Object -Last 1)
    if (-not $line) { throw "-Fresh: could not discover seam tags from the guest (no TAGS: line)." }
    return @(($line -replace '^TAGS:', '').Split(',', [StringSplitOptions]::RemoveEmptyEntries) |
             ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

# ── merge per-seam JUnit XMLs into one document Write-Summary can consume ──────
# Each per-seam file (from a -Seam run) carries only that seam's testcases, and the
# scaffold pass carries only the skipped ones, so there is no double-counting; we
# just graft every <testsuite> under one fresh <testsuites> root.
function Merge-JUnitXml {
    param([string[]]$Paths, [string]$OutPath)
    $doc = New-Object System.Xml.XmlDocument
    [void]$doc.AppendChild($doc.CreateElement('testsuites'))
    foreach ($p in $Paths) {
        if (-not $p -or -not (Test-Path $p)) { continue }
        [xml]$d = Get-Content -Path $p -Raw
        foreach ($ts in $d.SelectNodes('//testsuite')) {
            [void]$doc.DocumentElement.AppendChild($doc.ImportNode($ts, $true))
        }
    }
    $doc.Save($OutPath)
}

# ── 1. resolve + gate the exe ────────────────────────────────────────────────
$exe = (Resolve-Path -Path $ExePath -ErrorAction SilentlyContinue)?.Path
if (-not $exe) {
    throw "winspace.exe not found at '$ExePath'. Build it first: .\build.ps1 -Config release"
}
if (-not (Test-IsReleaseExe -Path $exe)) {
    throw "'$exe' links the debug CRT (/MDd) — the debug runtime is non-redistributable and won't " +
          "run in a clean guest. Rebuild Release: .\build.ps1 -Config release"
}
Write-Host "winspace VM seam tests — exe: $exe" -ForegroundColor White
Write-Host "  Release gate: OK" -ForegroundColor Green

# ── 2. preflight the VM/tooling environment before any state-mutating vmctl ───
Assert-Environment

# ── best-effort artifact copy-back (run.log + failure screenshots) ───────────
function Copy-Artifacts {
    try { Invoke-Vmctl cp -o "${Vm}:$GuestRoot\run.log" (Join-Path $PSScriptRoot 'run.log') | Out-Null } catch {}
    try {
        $pngs = Invoke-Vmctl exec -t $Vm "Get-ChildItem '$GuestRoot\failure-*.png' -ErrorAction SilentlyContinue | ForEach-Object Name"
        foreach ($name in ($pngs -split "`r?`n" | Where-Object { $_ -match '\.png$' })) {
            Invoke-Vmctl cp -o "${Vm}:$GuestRoot\$($name.Trim())" (Join-Path $PSScriptRoot $name.Trim()) | Out-Null
        }
    } catch {}
}

$resultsLocal = Join-Path $PSScriptRoot 'results.xml'

if (-not $Fresh) {
    # ── default: revert once, run every seam in one guest session ────────────
    if (-not $SkipReset) {
        Write-Host "reverting snapshot '$Snapshot' on '$Vm'…"
        Invoke-Vmctl snapshot reset $Vm $Snapshot | Out-Null
    }
    Invoke-Deploy -Exe $exe
    Write-Host 'running seams in the interactive guest session…'
    $xml = Invoke-GuestRunner -SeamTag $Seam -LocalXml $resultsLocal
    Copy-Artifacts
    if (-not $xml) {
        throw 'no results.xml came back from the guest — the runner did not complete (check autologon / the guest gate).'
    }
}
else {
    # ── -Fresh: a pristine snapshot per LIVE seam; scaffolds in one final pass ─
    # Discover the seam set first (deploy onto current state — discovery launches
    # no winspace, so it is safe without a revert), unless a single -Seam is pinned.
    Invoke-Deploy -Exe $exe
    $liveTags = if ($Seam) { @($Seam) } else { Get-LiveSeamTags }
    Write-Host "Fresh mode: reverting per live seam — $($liveTags -join ', ')" -ForegroundColor White

    $perSeam = [System.Collections.Generic.List[string]]::new()
    foreach ($tag in $liveTags) {
        Write-Host "── seam '$tag': fresh snapshot ──" -ForegroundColor Cyan
        Invoke-Vmctl snapshot reset $Vm $Snapshot | Out-Null
        Invoke-Deploy -Exe $exe
        $local = Join-Path $PSScriptRoot "results.$tag.xml"
        $xml = Invoke-GuestRunner -SeamTag $tag -LocalXml $local
        Copy-Artifacts
        if (-not $xml) {
            throw "no results.xml came back for seam '$tag' — the runner did not complete (check autologon / the guest gate)."
        }
        $perSeam.Add($xml)
    }

    # The 03–10 scaffolds are all -Skip (no VM state), so collect them once on the
    # last seam's state without another revert. Skipped when a single -Seam is pinned.
    if (-not $Seam) {
        Write-Host '── scaffolds (skipped, no revert) ──' -ForegroundColor Cyan
        $scaffoldXml = Invoke-GuestRunner -SeamTag 'scaffold' -LocalXml (Join-Path $PSScriptRoot 'results.scaffold.xml')
        if ($scaffoldXml) { $perSeam.Add($scaffoldXml) }
    }

    Merge-JUnitXml -Paths $perSeam -OutPath $resultsLocal
}

# ── summarise + set host exit code ───────────────────────────────────────────
$ok = Write-Summary -XmlPath $resultsLocal
exit ($ok ? 0 : 1)
