<#
.SYNOPSIS
    Timed demo driver for winspace - a recordable reel, NOT a seam test.

.DESCRIPTION
    Runs INSIDE the guest, in the one interactive logged-in session (same session
    the seam runner uses). It drives the shipping Release winspace.exe through a
    scripted sequence of "beats" at a fixed dwell, showing an on-screen caption for
    each, so you can film the whole pass once with OBS and publish it with no editing.

    This is a SIBLING of the Pester seam files, not one of them: it has no Oracle and
    makes no assertions. Invoke-SmokeSeams.ps1 never picks it up (it is a .ps1, not a
    *.Tests.ps1). It reuses WinspaceTest.psm1 for the real primitives - Send-Chord
    (genuine SendInput chords), Start-Winspace, Set-DesktopCount - exactly as a
    physical keypress / launch would drive winspace. No winspace source change.

    Saved as UTF-8 WITH BOM on purpose: the guest runs Windows PowerShell 5.1, which
    decodes a BOM-less file as ANSI and mangles every non-ASCII character. The BOM
    makes 5.1 (and 7+) decode it as UTF-8. Keep the BOM if you re-save.

    TWO WAYS TO RUN
      * Full pass (default): setup -> all 6 beats -> teardown, in one process.
            .\Invoke-Demo.ps1                 # 7s/beat
            .\Invoke-Demo.ps1 -DwellSec 5
      * Step by step (for screenshot verification): run beats individually across
        separate invocations. State (winspace, caption, apps) lives in separate
        processes, so it persists between calls. -KeepAlive skips teardown; -Attach
        skips setup and reuses what -KeepAlive left running; -Teardown cleans up.
            .\Invoke-Demo.ps1 -Beat 1 -KeepAlive             # setup + beat 1, leave up
            .\Invoke-Demo.ps1 -Attach -Beat 2 -KeepAlive     # beat 2, leave up
            ...
            .\Invoke-Demo.ps1 -Attach -Beat 6 -KeepAlive     # beat 6, leave up
            .\Invoke-Demo.ps1 -Teardown                      # cleanup

    Environment: SINGLE-DISPLAY VM. The multi-monitor Distribute / cross-monitor
    focus story is out of scope here (it cannot show on one display). The reel covers
    Distribute (auto-maximize), Slots, same-screen spatial focus, workspace switch,
    move-to-workspace, and tile. Prepare a dedicated VM demo snapshot; revert to re-take.

.PARAMETER DwellSec
    Seconds to hold each beat so a viewer can read the caption and see the result
    settle. Default 7. Use 0 in step mode (you screenshot instead of holding).

.PARAMETER WinspaceExe
    Path to the Release winspace.exe. Default: the harness deploy path
    (C:\winspace-e2e\winspace.exe), else `winspace` on PATH.

.PARAMETER Beat
    Run only this single beat (1..6). Default 0 = run all six in order. A single
    [int] binds reliably under `powershell -File` (an [int[]] does not).

.PARAMETER Attach
    Skip setup; drive the winspace + caption a prior -KeepAlive call left running.

.PARAMETER KeepAlive
    Skip teardown; leave winspace, caption, and apps up for the next step / a screenshot.

.PARAMETER Teardown
    Run cleanup only (stop winspace, close caption + apps, restore config), then exit.
#>
[CmdletBinding()]
param(
    [int]$DwellSec = 7,
    [string]$WinspaceExe,
    [int]$Beat = 0,
    [switch]$Attach,
    [switch]$KeepAlive,
    [switch]$Teardown
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force

# ── persistent paths (deterministic, so separate step invocations share them) ─
$TempDir       = [System.IO.Path]::GetTempPath()
$CaptionFile   = Join-Path $TempDir 'winspace-demo-caption.txt'
$LogPath       = Join-Path $TempDir 'winspace-demo.log'
$ConfigBakFile = Join-Path $TempDir 'winspace-demo-config.bak'    # the caller's original config
$ConfigState   = Join-Path $TempDir 'winspace-demo-config.state'  # 'exists' | 'none'
$CaptionTitle  = 'winspace-demo-caption'

# The neutral app for the WS-2 beat: NO window rule (so Distribute just maximizes it
# on the current Workspace) and a fresh top-level window each launch. Swap freely.
$Ws2App = 'mspaint.exe'

# ── the demo config (Q9) ─────────────────────────────────────────────────────
# $mod = ALT registers on stock Windows 11 with no policy (ADR-0014). The slot rules
# key on exe basename (case-insensitive exact match): a File Explorer window runs
# under the shell explorer.exe PID, so exe:explorer.exe matches on its Appeared edge.
# The caption overlay is borderless (already Ineligible - no WS_CAPTION/WS_THICKFRAME),
# and the ignore rule is belt-and-suspenders.
$DemoConfig = @'
$mod = ALT
bind = $mod SHIFT, Q, quit
bind = $mod, H, focus, left
bind = $mod, L, focus, right
bind = $mod, T, tile
bind = $mod, 1, workspace, 1
bind = $mod, 2, workspace, 2
bind = $mod SHIFT, 2, movetoworkspacesilent, 2
windowrule = workspace 1 slot left-half,  exe:notepad.exe
windowrule = workspace 1 slot right-half, exe:explorer.exe
windowrule = ignore, title:winspace-demo-caption
'@

# ── resolve the winspace binary ──────────────────────────────────────────────
function Resolve-WinspaceExe {
    param([string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path $Explicit)) { throw "Invoke-Demo: -WinspaceExe '$Explicit' does not exist." }
        return (Resolve-Path $Explicit).Path
    }
    $deployed = Get-WinspaceExe
    if (Test-Path $deployed) { return $deployed }
    $onPath = Get-Command 'winspace' -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "Invoke-Demo: no winspace.exe found. Pass -WinspaceExe, deploy to '$deployed', or put winspace on PATH."
}

# ── caption overlay: a borderless, top-most, no-activate banner child process ─
# Spawned as a CHILD (mirrors Start-TestWindow) so its WinForms message loop runs
# without blocking this driver, and survives this process exiting (step mode). The
# driver writes the current caption to a file; the child polls it. WS_EX_NOACTIVATE
# keeps the banner from ever stealing the foreground (which would break the spatial-
# focus Origin mid-beat); WS_EX_TOOLWINDOW keeps it out of Alt-Tab. Sentinel
# '__CLOSE__' tells the child to exit (how teardown closes it with no Process handle).
function Start-Caption {
    if (@(Find-WindowsByTitle -Substring $CaptionTitle).Count -gt 0) { return }   # already up (Attach)
    if (Test-Path $CaptionFile) { Remove-Item $CaptionFile -Force }
    Set-Content -LiteralPath $CaptionFile -Value ' ' -Encoding UTF8

    $child = @"
`$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Windows.Forms;
public class CaptionForm : Form {
    protected override CreateParams CreateParams {
        get {
            var cp = base.CreateParams;
            cp.ExStyle |= 0x08000000 /* WS_EX_NOACTIVATE */
                       |  0x00000080 /* WS_EX_TOOLWINDOW  */
                       |  0x00000008 /* WS_EX_TOPMOST     */;
            return cp;
        }
    }
    protected override bool ShowWithoutActivation { get { return true; } }
}
'@
Add-Type -AssemblyName System.Drawing
`$f = New-Object CaptionForm
`$f.Text = '$CaptionTitle'
`$f.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
`$f.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
`$f.ShowInTaskbar = `$false
`$f.TopMost = `$true
`$f.BackColor = [System.Drawing.Color]::FromArgb(20, 20, 24)
`$f.Opacity = 0.88
`$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
`$w = [Math]::Min(1000, `$wa.Width - 80)
`$f.Size = New-Object System.Drawing.Size(`$w, 88)
`$f.Location = New-Object System.Drawing.Point(([int](`$wa.X + (`$wa.Width - `$w) / 2)), (`$wa.Y + 28))
`$lbl = New-Object System.Windows.Forms.Label
`$lbl.Dock = [System.Windows.Forms.DockStyle]::Fill
`$lbl.ForeColor = [System.Drawing.Color]::White
`$lbl.Font = New-Object System.Drawing.Font('Segoe UI', 22, [System.Drawing.FontStyle]::Bold)
`$lbl.TextAlign = [System.Drawing.ContentAlignment]::MiddleCenter
`$f.Controls.Add(`$lbl)
`$t = New-Object System.Windows.Forms.Timer
`$t.Interval = 200
`$t.Add_Tick({
    try { `$txt = (Get-Content -LiteralPath '$CaptionFile' -Raw -ErrorAction Stop) } catch { return }
    if (`$null -eq `$txt) { return }
    `$txt = `$txt.Trim()
    if (`$txt -eq '__CLOSE__') { `$t.Stop(); [System.Windows.Forms.Application]::Exit(); return }
    if (`$lbl.Text -ne `$txt) { `$lbl.Text = `$txt }
})
`$t.Start()
[System.Windows.Forms.Application]::Run(`$f)
"@
    $enc = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($child))
    Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden `
        -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $enc | Out-Null
}

# Update the banner text (a lone WriteAllText is one OS write; the child re-polls on
# any transient read error).
function Set-Caption {
    param([Parameter(Mandatory)][string]$Text)
    [System.IO.File]::WriteAllText($CaptionFile, $Text)
    Write-Host "  $Text" -ForegroundColor Cyan
}

# ── small helpers ────────────────────────────────────────────────────────────
# Launch an app and wait for a top-level window whose title contains $TitleSub, so
# the next beat does not race the window's creation. For apps with an unpredictable
# title (Explorer, Paint) pass no -TitleSub and it falls back to a settle sleep.
function Start-DemoApp {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$ArgumentList = @(),
        [string]$TitleSub,
        [int]$TimeoutSec = 12
    )
    if ($ArgumentList.Count -gt 0) { Start-Process -FilePath $Exe -ArgumentList $ArgumentList | Out-Null }
    else { Start-Process -FilePath $Exe | Out-Null }
    if ($TitleSub) {
        Wait-Until -TimeoutSec $TimeoutSec -Because "'$Exe' window ('$TitleSub') to appear" -Condition {
            @(Find-WindowsByTitle -Substring $TitleSub).Count -gt 0
        }
        Start-Sleep -Milliseconds 700   # let winspace's Appeared placement settle
    } else {
        Start-Sleep -Milliseconds 2200
    }
}

# Best-effort: bring the first window whose title contains $TitleSub to the
# foreground via a real synthesized click. Never throws - a demo must not hard-abort
# on a focus race; the following chord still fires from whatever the Origin becomes.
function Set-DemoFocus {
    param([Parameter(Mandatory)][string]$TitleSub)
    # The Widgets board auto-REOPENS over the demo's timeline (setup killed it ~20s
    # ago); a reopened flyout covers the left-half slot and eats the click, so kill it
    # again right before we click. Confirmed root cause of the focus miss (probe4 vs
    # the full run - the isolated click works, the delayed one does not).
    Get-Process -Name 'Widgets' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 250
    $h = @(Find-WindowsByTitle -Substring $TitleSub) | Select-Object -First 1
    if (-not $h) { Write-Warning "Set-DemoFocus: no '$TitleSub' window found; continuing."; return }
    Set-ForegroundByClick -Hwnd $h
    try {
        Wait-Until -TimeoutSec 6 -Because "'$TitleSub' to take the foreground" -Condition {
            (Get-ForegroundWindow) -eq $h
        }
    } catch { Write-Warning "Set-DemoFocus: '$TitleSub' did not take the foreground; continuing." }
}

# Best-effort close by process name. Never throws. Deliberately does NOT touch
# explorer.exe (that is the shell; killing it drops the taskbar) - on a throwaway
# demo snapshot the launched Explorer window is left for the revert.
function Stop-ByName {
    param([string[]]$Names)
    foreach ($n in $Names) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

# ── setup / teardown (each usable standalone for step mode) ───────────────────
function Invoke-Setup {
    param([string]$Exe)
    # Back up the caller's config to a side file (survives across step invocations).
    $cfgPath = Get-WinspaceConfigPath
    if (Test-Path $cfgPath) {
        Copy-Item -LiteralPath $cfgPath -Destination $ConfigBakFile -Force
        Set-Content -LiteralPath $ConfigState -Value 'exists' -Encoding ASCII
    } else {
        Set-Content -LiteralPath $ConfigState -Value 'none' -Encoding ASCII
    }
    Set-WinspaceConfig -Content $DemoConfig | Out-Null

    # A cold, freshly-reverted guest auto-opens the Windows Widgets board - a full-
    # height left-side flyout that covers the left-half Slot and HOLDS the foreground,
    # so a center-click on a left-slotted window hits Widgets instead (breaking the
    # spatial-focus Origin). Kill its host; it does not reopen without a user trigger.
    # Same fix the SpatialFocus seam applies (WinspaceTest / SpatialFocus.Tests.ps1).
    Get-Process -Name 'Widgets' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    # Single-instance Primary: kill any running winspace first (a bare relaunch would
    # just exit against it), then start fresh so the seeded rules are live.
    Stop-ByName -Names 'winspace'
    Start-Sleep -Milliseconds 500
    $ws = Start-Winspace -Exe $Exe -LogPath $LogPath

    Set-DesktopCount 1          # known clean floor: one empty Workspace
    Start-Caption
    Start-Sleep -Milliseconds 600
    return $ws
}

function Invoke-Teardown {
    # Close the caption via its file sentinel (works with no Process handle).
    if (Test-Path $CaptionFile) {
        try { [System.IO.File]::WriteAllText($CaptionFile, '__CLOSE__') } catch {}
        Start-Sleep -Milliseconds 600
        Remove-Item $CaptionFile -Force -ErrorAction SilentlyContinue
    }
    Stop-ByName -Names 'notepad', 'CalculatorApp', 'Calculator', 'mspaint'
    Stop-ByName -Names 'winspace'

    # Restore the caller's original config from the side file (or remove ours).
    $cfgPath = Get-WinspaceConfigPath
    $state = if (Test-Path $ConfigState) { (Get-Content -LiteralPath $ConfigState -Raw).Trim() } else { 'none' }
    if ($state -eq 'exists' -and (Test-Path $ConfigBakFile)) {
        Copy-Item -LiteralPath $ConfigBakFile -Destination $cfgPath -Force
    } else {
        Clear-WinspaceConfig
    }
    Remove-Item $ConfigBakFile, $ConfigState -Force -ErrorAction SilentlyContinue
    Set-RunnerConsoleVisible $true   # undo the setup-time hide
    Write-Host 'teardown complete.' -ForegroundColor Green
}

# ── the beats ────────────────────────────────────────────────────────────────
# Each is a scriptblock reading the script-scope $DwellSec / $Ws2App / $CaptionFile.
# A PLAIN hashtable (not [ordered]): an OrderedDictionary indexed by an [int] does
# POSITIONAL (0-based) access, not key lookup - $Beats[2] would return the 3rd entry.
# Order comes from the explicit $beatNums loop below, so keyed access is what we want.
$Beats = @{
    1 = {
        Set-Caption '1/6  Distribute - new windows place and maximize automatically'
        Start-DemoApp -Exe 'calc.exe' -TitleSub 'Calculator'
        Start-Sleep -Seconds $DwellSec
    }
    2 = {
        Set-Caption '2/6  Slots - window rules snap apps into place'
        Start-DemoApp -Exe 'notepad.exe' -TitleSub 'Notepad'
        Start-Sleep -Seconds 1
        Start-DemoApp -Exe 'explorer.exe' -ArgumentList @('C:\')
        Start-Sleep -Seconds $DwellSec
    }
    3 = {
        Set-Caption '3/6  Spatial focus - move focus by direction (Alt+H / Alt+L)'
        Set-DemoFocus -TitleSub 'Notepad'          # deterministic Origin on the left half
        Start-Sleep -Milliseconds 800
        Send-Chord 'Alt+L'                          # focus right -> Explorer
        Start-Sleep -Seconds ([Math]::Max(2, [int]($DwellSec / 2)))
        Send-Chord 'Alt+H'                          # focus left -> Notepad
        Start-Sleep -Seconds $DwellSec
    }
    4 = {
        Set-Caption '4/6  Workspaces - switch with Alt+1 .. Alt+5'
        Send-Chord 'Alt+2'
        Start-Sleep -Seconds 1
        Start-DemoApp -Exe $Ws2App                  # unruled -> Distribute maximizes it on WS 2
        Start-Sleep -Seconds $DwellSec
        Send-Chord 'Alt+1'                          # back to the split, still intact
        Start-Sleep -Seconds $DwellSec
    }
    5 = {
        Set-Caption '5/6  Move to workspace - send a window with Alt+Shift+2'
        Set-DemoFocus -TitleSub 'Notepad'
        Start-Sleep -Milliseconds 800
        Send-Chord 'Alt+Shift+2'                    # Notepad leaves the split for WS 2
        Start-Sleep -Seconds $DwellSec
    }
    6 = {
        Set-Caption '6/6  Tile - rebalance everything on demand (Alt+T)'
        Send-Chord 'Alt+T'
        Start-Sleep -Seconds $DwellSec
    }
}

# ── run ──────────────────────────────────────────────────────────────────────
Assert-InteractiveSession   # loud gate: without an interactive desktop, SendInput reaches nothing

# Hide THIS process's own console window. It is a real WS_THICKFRAME|WS_CAPTION
# top-level window, so winspace would treat it as Eligible - track it, tile it, and
# (worse) let it steal the foreground so a `focus`/`movetoworkspace` chord acts on the
# console instead of the intended app. Hiding it (SW_HIDE -> not WS_VISIBLE -> Ineligible)
# removes it from winspace's view AND keeps a real OBS recording clean. stdout is still
# captured, so console text output is unaffected. Same fix the seams apply via
# Set-RunnerConsoleVisible (WinspaceTest.psm1). Restored by teardown.
Set-RunnerConsoleVisible $false

if ($Teardown) { Invoke-Teardown; return }

$exe = Resolve-WinspaceExe -Explicit $WinspaceExe
$beatNums = if ($Beat -gt 0) { @($Beat) } else { 1..6 }
foreach ($n in $beatNums) { if (-not $Beats.Contains($n)) { throw "Invoke-Demo: no beat $n (valid: 1..6)." } }

$ranSetup = $false
try {
    if (-not $Attach) {
        Write-Host "winspace demo - driving $exe" -ForegroundColor Green
        Invoke-Setup -Exe $exe | Out-Null
        $ranSetup = $true
    }

    foreach ($n in $beatNums) { & $Beats[$n] }

    if (-not $KeepAlive -and $Beat -eq 0) {
        Set-Caption 'winspace'
        Start-Sleep -Seconds 2
        Write-Host 'demo complete.' -ForegroundColor Green
    }
}
finally {
    if (-not $KeepAlive) { Invoke-Teardown }
}
