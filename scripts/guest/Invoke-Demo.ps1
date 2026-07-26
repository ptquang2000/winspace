<#
.SYNOPSIS
    Timed demo driver for winspace - a recordable reel, NOT a seam test.

.DESCRIPTION
    Runs INSIDE the guest, in the one interactive logged-in session (same session
    the seam runner uses). It drives the shipping Release winspace.exe through a
    scripted sequence of "beats" at a fixed dwell, showing an on-screen caption for
    each, so you can film the whole pass once with OBS and publish it with no editing.

    The reel shows a REAL session, not stock accessories: `exec-once` starts Zen
    Browser, PowerShell 7 and sioyek, and `windowrule` puts each on its own
    workspace (zen -> 1, pwsh -> 2, sioyek -> 3 pinned to the right half). Every
    later beat drives that session - the same layout a user actually logs into.

    This is a SIBLING of the Pester seam files, not one of them: it has no Oracle and
    makes no assertions. Invoke-SmokeSeams.ps1 never picks it up (it is a .ps1, not a
    *.Tests.ps1). It reuses WinspaceTest.psm1 for the real primitives - Send-Chord
    (genuine SendInput chords), Start-Winspace, Set-DesktopCount, Move-Window -
    exactly as a physical keypress / launch would drive winspace. No winspace source
    change.

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

    Environment: SINGLE-DISPLAY VM, snapshot `winspace-demo` - the winspace-e2e
    snapshot plus scoop-installed zen-browser, pwsh and sioyek (setup fails loud if
    any of the three is missing). The multi-monitor Distribute / cross-monitor focus
    story is out of scope here (it cannot show on one display). Revert the snapshot to
    re-take.

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

# -- persistent paths (deterministic, so separate step invocations share them) --
$TempDir       = [System.IO.Path]::GetTempPath()
$CaptionFile   = Join-Path $TempDir 'winspace-demo-caption.txt'
$LogPath       = Join-Path $TempDir 'winspace-demo.log'
$ConfigBakFile = Join-Path $TempDir 'winspace-demo-config.bak'    # the caller's original config
$ConfigState   = Join-Path $TempDir 'winspace-demo-config.state'  # 'exists' | 'none'
$ZenPrefState  = Join-Path $TempDir 'winspace-demo-zen-prefs.state'   # zen pref files WE created
$CaptionTitle  = 'winspace-demo-caption'

# Window-title substrings the beats aim at. zen titles every window
# "<page> - Zen Browser"; the pwsh console titles itself with the command line it
# was launched from (the shim path, which ends in pwsh.exe); sioyek with no
# argument opens its bundled tutorial.pdf and titles the window after it.
$ZenTitle    = 'Zen Browser'
$PwshTitle   = 'pwsh.exe'
$SioyekTitle = 'tutorial.pdf'
# (the git-bash window beat 3 opens is found by process, not title - see
# Get-DemoShellWindow for why)

# -- resolve the three session apps -------------------------------------------
# ABSOLUTE paths on purpose. winspace launches an exec entry with CreateProcessW,
# which searches the %PATH% winspace INHERITED - and on this snapshot the
# interactive session was logged in before scoop installed anything, so its PATH
# has no ~\scoop\shims (a bare `exec-once = zen` fails with "file not found"). The
# scoop shim is preferred over ...\apps\<app>\<version>\ so a scoop update does not
# invalidate the path. Quoted in the config for a profile path with spaces.
function Resolve-DemoApps {
    $shims = Join-Path $env:USERPROFILE 'scoop\shims'
    $want = @{ zen = 'zen.exe'; pwsh = 'pwsh.exe'; sioyek = 'sioyek.exe'; gitbash = 'git-bash.exe' }
    $resolved = @{}
    $missing = @()
    foreach ($name in @('zen', 'pwsh', 'sioyek', 'gitbash')) {
        $shim = Join-Path $shims $want[$name]
        if (Test-Path $shim) { $resolved[$name] = $shim; continue }
        $onPath = Get-Command $want[$name] -ErrorAction SilentlyContinue
        if ($onPath) { $resolved[$name] = $onPath.Source } else { $missing += $want[$name] }
    }
    if ($missing.Count -gt 0) {
        throw ("Invoke-Demo: session app(s) not found: {0}. This reel needs the " +
               "winspace-demo snapshot (winspace-e2e plus: scoop install zen-browser pwsh sioyek git)." -f
               ($missing -join ', '))
    }
    return $resolved
}

# -- the demo config ----------------------------------------------------------
# $mod = ALT registers on stock Windows 11 with no policy (ADR-0014). Rules key on
# exe basename (case-insensitive exact match) on the Appeared edge; the launcher is
# launch-only, so each exec-once line is PAIRED with a windowrule that places it
# (ADR-0011) - which is exactly the composition this reel is meant to show. Every rule
# carries an explicit `slot`, because a rule match opts the window OUT of Distribute
# (ADR-0020): a bare `workspace N` rule would move the window and leave it at whatever
# size the app chose. sioyek's right-half opens workspace 3 as "PDF on the right, shell
# on the left"; the git-bash window beat 3 opens (not an exec-once entry) claims that
# left half. Its rule matches mintty.exe, not git-bash.exe: git-bash is a launcher that
# spawns mintty, and rules key on the exe of the process that OWNS the window.
#
# File Explorer was the first choice for that left half and is deliberately NOT used: an
# Explorer window moved to another workspace snaps straight back to its old one the
# moment that workspace is activated (the shell re-homes its own windows), so the
# move-to-workspace beat had nothing to show. Any ordinary app moves and stays.
#
# The caption overlay is borderless (already Ineligible - no WS_CAPTION/WS_THICKFRAME)
# and the ignore rule is belt-and-suspenders.
function Get-DemoConfig {
    param([Parameter(Mandatory)][hashtable]$App)
    @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
bind = `$mod, H, focus, left
bind = `$mod, L, focus, right
bind = `$mod, T, tile
bind = `$mod, 1, workspace, 1
bind = `$mod, 2, workspace, 2
bind = `$mod, 3, workspace, 3
bind = `$mod, 4, workspace, 4
bind = `$mod SHIFT, 4, movetoworkspacesilent, 4
exec-once = "$($App.zen)"
exec-once = "$($App.pwsh)" -NoLogo -NoExit
exec-once = "$($App.sioyek)"
windowrule = workspace 1 slot maximized, exe:zen.exe
windowrule = workspace 2 slot maximized, exe:pwsh.exe
windowrule = workspace 3 slot right-half, exe:sioyek.exe
windowrule = workspace 3 slot left-half,  exe:mintty.exe
windowrule = ignore, title:$CaptionTitle
"@
}

# -- resolve the winspace binary ----------------------------------------------
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

# -- zen first-run: suppress the onboarding tour -------------------------------
# On a freshly reverted snapshot zen's first launch opens its full-screen "Welcome to
# a calmer internet" tour instead of a browser window - it would be the first thing
# the reel shows. The prefs that dismiss it are the ones zen itself writes once the
# tour is done (read back out of prefs.js afterwards), but they cannot be seeded as a
# profile `user.js`: on a fresh revert NO profile directory exists yet (zen creates it
# during that very first launch). So they go in through AutoConfig instead - a
# mozilla.cfg in the INSTALL directory, which every profile picks up, including one
# created seconds later. Idempotent: pre-existing files are left alone, and files we
# create are recorded so teardown removes them (the snapshot revert would too, but a
# -KeepAlive run on a live guest should not leave litter).
function Initialize-ZenFirstRun {
    $installDir = Join-Path $env:USERPROFILE 'scoop\apps\zen-browser\current'
    if (-not (Test-Path $installDir)) {
        Write-Warning "Initialize-ZenFirstRun: no zen install at '$installDir' - zen may show its first-run tour."
        return
    }
    # A fresh profile also has no record of which build it last ran, so zen greets it
    # with an "Update Complete! / What's new in Zen" panel over the page. Seeding the
    # two prefs zen compares against - taken from the install's OWN application.ini, so
    # this keeps working across a scoop update - stops that too.
    $ini = @{}
    $iniPath = Join-Path $installDir 'application.ini'
    if (Test-Path $iniPath) {
        foreach ($line in Get-Content -LiteralPath $iniPath) {
            if ($line -match '^\s*(BuildID|Version)\s*=\s*(.+?)\s*$') { $ini[$Matches[1]] = $Matches[2] }
        }
    }

    # AutoConfig is two files: a hook in defaults\pref\ that names the .cfg (and
    # turns OFF the byte-obscuring and the JS sandbox, so the .cfg is plain text and
    # may set any pref), and the .cfg itself - whose FIRST LINE IS IGNORED by the
    # parser, hence the leading comment. defaultPref (not lockPref) so the user can
    # still change any of them by hand.
    $cfg = [System.Collections.Generic.List[string]]::new()
    $cfg.Add('// winspace demo - first line of a mozilla.cfg is ignored by design')
    $cfg.Add('defaultPref("zen.welcome-screen.seen", true);')
    $cfg.Add('defaultPref("browser.aboutwelcome.enabled", false);')
    $cfg.Add('defaultPref("browser.startup.homepage_override.mstone", "ignore");')
    $cfg.Add('defaultPref("browser.shell.checkDefaultBrowser", false);')
    if ($ini.ContainsKey('BuildID')) {
        $cfg.Add("defaultPref(`"zen.updates.last-build-id`", `"$($ini['BuildID'])`");")
        $cfg.Add("defaultPref(`"zen.session-store.last-build-id`", `"$($ini['BuildID'])`");")
    }
    if ($ini.ContainsKey('Version')) {
        $cfg.Add("defaultPref(`"zen.updates.last-version`", `"$($ini['Version'])`");")
    }

    $files = @{
        (Join-Path $installDir 'defaults\pref\autoconfig.js') = @(
            'pref("general.config.filename", "mozilla.cfg");'
            'pref("general.config.obscure_value", 0);'
            'pref("general.config.sandbox_enabled", false);'
        )
        (Join-Path $installDir 'mozilla.cfg') = $cfg.ToArray()
    }
    $created = @()
    foreach ($path in $files.Keys) {
        if (Test-Path $path) { continue }
        $dir = Split-Path -Parent $path
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Set-Content -LiteralPath $path -Value ($files[$path] -join "`r`n") -Encoding ASCII
        $created += $path
    }
    if ($created.Count -gt 0) { Set-Content -LiteralPath $ZenPrefState -Value $created -Encoding UTF8 }
}

# -- caption overlay: a borderless, top-most, no-activate banner child process --
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

# -- small helpers ------------------------------------------------------------
# First top-level window whose title contains $TitleSub, or $null. The beats aim at
# windows winspace/exec-once created, so there is no Process handle to hold.
function Get-DemoWindow {
    param([Parameter(Mandatory)][string]$TitleSub)
    return @(Find-WindowsByTitle -Substring $TitleSub) | Select-Object -First 1
}

# Wait for a window title to exist, so the next beat does not race a launch. zen's
# cold start (first run, fresh profile) is the slow one - hence the generous default.
function Wait-DemoWindow {
    param(
        [Parameter(Mandatory)][string]$TitleSub,
        [int]$TimeoutSec = 45,
        [int]$SettleMs = 900
    )
    Wait-Until -TimeoutSec $TimeoutSec -Because "a '$TitleSub' window to appear" -Condition {
        @(Find-WindowsByTitle -Substring $TitleSub).Count -gt 0
    }
    Start-Sleep -Milliseconds $SettleMs   # let winspace's Appeared placement settle
}

# The git-bash window, found by PROCESS rather than title. mintty first titles its
# window with the command line it was launched from ("/usr/bin/bash --login -i") and
# only renames it to "MINGW64:<cwd>" once the login shell has finished starting - on a
# cold first run that can take longer than a title wait wants to sit for. The owning
# process is there from the first frame, so key the whole reel off that instead. Returns
# $null until the window exists; works across step invocations (no handle to carry).
function Get-DemoShellWindow {
    $p = Get-Process -Name 'mintty' -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { return [IntPtr]$p.MainWindowHandle }
    return $null
}

# Launch git-bash and wait for mintty's window (see Get-DemoShellWindow).
function Start-DemoShell {
    param([Parameter(Mandatory)][string]$Exe, [int]$TimeoutSec = 45)
    Start-Process -FilePath $Exe | Out-Null
    Wait-Until -TimeoutSec $TimeoutSec -Because 'the git-bash (mintty) window to appear' -Condition {
        $null -ne (Get-DemoShellWindow)
    }
    Start-Sleep -Milliseconds 900   # let winspace's Appeared placement settle
}

# Best-effort: bring a window to the foreground via a real synthesized click. Never
# throws - a demo must not hard-abort on a focus race; the following chord still fires
# from whatever the Origin becomes.
function Set-DemoFocus {
    param([Parameter(Mandatory)][AllowNull()][object]$Hwnd, [string]$Label = 'window')
    # The Widgets board auto-REOPENS over the demo's timeline (setup killed it ~20s
    # ago); a reopened flyout covers the left half of the screen and eats the click, so
    # kill it again right before we click. Confirmed root cause of a focus miss (the
    # isolated click works, the delayed one does not).
    Get-Process -Name 'Widgets' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 250
    if (-not $Hwnd) { Write-Warning "Set-DemoFocus: no $Label to focus; continuing."; return }
    $h = [IntPtr]$Hwnd
    Set-ForegroundByClick -Hwnd $h
    try {
        Wait-Until -TimeoutSec 6 -Because "the $Label to take the foreground" -Condition {
            (Get-ForegroundWindow) -eq $h
        }
    } catch { Write-Warning "Set-DemoFocus: the $Label did not take the foreground; continuing." }
}

# Send a movetoworkspace chord and CONFIRM the window ARRIVED on the target desktop,
# pressing again (up to three times) if it did not. Observed on a live guest: a chord
# sent seconds after the window appeared can land with nothing moving, and a reel that
# silently no-ops on its own key beat is worse than one that presses twice.
#
# The check compares against the TARGET desktop GUID, not merely "different from
# before": mid-move the window is cloaked, and a desktop-id read taken right then comes
# back as neither desktop - which a "has it changed?" test reads as success and then
# leaves the window where it started (that false pass is exactly how this was found).
# Workspace N is desktops[N-1] because startup adoption bound the logical numbers to the
# existing desktops in order (ADR-0003), and the reel creates/destroys none after that.
# Never throws - warn and let the reel continue.
function Move-DemoWindowToWorkspace {
    param(
        [Parameter(Mandatory)][AllowNull()][object]$Hwnd,
        [Parameter(Mandatory)][string]$Chord,
        [Parameter(Mandatory)][int]$Workspace,
        [string]$Label = 'window'
    )
    if (-not $Hwnd) { Write-Warning "Move-DemoWindowToWorkspace: no $Label to move; continuing."; return }
    $h = [IntPtr]$Hwnd
    $guids = @((Get-VdState).Guids)
    if ($guids.Count -lt $Workspace) {
        Write-Warning "Move-DemoWindowToWorkspace: only $($guids.Count) desktop(s); cannot verify workspace $Workspace."
        Send-Chord $Chord
        return
    }
    $target = $guids[$Workspace - 1]
    foreach ($attempt in 1..3) {
        Send-Chord $Chord
        try {
            Wait-Until -TimeoutSec 4 -Because "the $Label to arrive on workspace $Workspace" -Condition {
                (Get-WindowDesktopId -Hwnd $h) -eq $target
            }
            return
        } catch {
            Write-Warning "Move-DemoWindowToWorkspace: '$Chord' did not land (attempt $attempt); pressing again."
            Start-Sleep -Milliseconds 700
        }
    }
    Write-Warning "Move-DemoWindowToWorkspace: the $Label never reached workspace $Workspace; continuing."
}

# Best-effort close by process name. Never throws. Deliberately does NOT touch
# explorer.exe (that is the shell; killing it drops the taskbar).
function Stop-ByName {
    param([string[]]$Names)
    foreach ($n in $Names) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

# Set-DesktopCount leaves the active desktop on the LAST-created one, but the reel
# has to OPEN on workspace 1 - that is where zen lands, and the audience should watch
# it arrive rather than find it already there. Native VD hotkey only (no winspace
# bind: this runs before winspace starts), polling the same registry Oracle.
function Switch-ToFirstDesktop {
    for ($i = 0; $i -lt 12; $i++) {
        $state = Get-VdState
        $guids = @($state.Guids)
        if ($guids.Count -eq 0 -or $state.CurrentGuid -eq $guids[0]) { return }
        Send-Chord 'Win+Ctrl+Left'
        Start-Sleep -Milliseconds 500
    }
    throw 'Invoke-Demo: could not switch back to the first desktop.'
}

# -- setup / teardown (each usable standalone for step mode) -------------------
function Invoke-Setup {
    param([string]$Exe, [Parameter(Mandatory)][hashtable]$App)

    # Back up the caller's config to a side file (survives across step invocations).
    $cfgPath = Get-WinspaceConfigPath
    if (Test-Path $cfgPath) {
        Copy-Item -LiteralPath $cfgPath -Destination $ConfigBakFile -Force
        Set-Content -LiteralPath $ConfigState -Value 'exists' -Encoding ASCII
    } else {
        Set-Content -LiteralPath $ConfigState -Value 'none' -Encoding ASCII
    }
    Set-WinspaceConfig -Content (Get-DemoConfig -App $App) | Out-Null
    Initialize-ZenFirstRun

    # A cold, freshly-reverted guest auto-opens the Windows Widgets board - a full-
    # height left-side flyout that covers the left half of the screen and HOLDS the
    # foreground, so a center-click on a left-slotted window hits Widgets instead
    # (breaking the spatial-focus Origin). Kill its host; it does not reopen without a
    # user trigger. Same fix the SpatialFocus seam applies.
    Get-Process -Name 'Widgets' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    # Single-instance Primary: kill any running winspace first (a bare relaunch would
    # just exit against it). Then stage the OS state the reel needs BEFORE launching:
    # FOUR desktops so startup adoption binds workspaces 1..4 by GUID (ADR-0003) - three
    # for the session apps plus the empty one beat 5 sends a window to - and the active
    # desktop back on the first one. Any leftover session app is closed too, so the
    # exec-once launches are the ones on screen.
    Stop-ByName -Names 'winspace'
    Stop-ByName -Names 'zen', 'pwsh', 'sioyek', 'mintty'
    Start-Sleep -Milliseconds 500
    Set-DesktopCount 4
    Switch-ToFirstDesktop

    # Starting winspace IS beat 1: the spine posts Started{}, which fires all three
    # exec-once entries, and the paired windowrules place each window as it appears.
    $ws = Start-Winspace -Exe $Exe -LogPath $LogPath
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
    Stop-ByName -Names 'zen', 'pwsh', 'sioyek', 'mintty'
    Stop-ByName -Names 'winspace'

    # Drop only the zen AutoConfig files THIS driver created (see Initialize-ZenFirstRun).
    if (Test-Path $ZenPrefState) {
        foreach ($f in Get-Content -LiteralPath $ZenPrefState) {
            if ($f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
        }
        Remove-Item $ZenPrefState -Force -ErrorAction SilentlyContinue
    }

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

# -- the beats ----------------------------------------------------------------
# Each is a scriptblock reading the script-scope $DwellSec / titles / $CaptionFile.
# A PLAIN hashtable (not [ordered]): an OrderedDictionary indexed by an [int] does
# POSITIONAL (0-based) access, not key lookup - $Beats[2] would return the 3rd entry.
# Order comes from the explicit $beatNums loop below, so keyed access is what we want.
$Beats = @{
    1 = {
        Set-Caption '1/6  exec-once - winspace starts your session: zen, pwsh, sioyek'
        # Setup started winspace, which fired the three exec-once entries. zen lands
        # here on workspace 1; pwsh and sioyek are placed away by their rules.
        Wait-DemoWindow -TitleSub $ZenTitle -TimeoutSec 60
        Wait-DemoWindow -TitleSub $PwshTitle
        Wait-DemoWindow -TitleSub $SioyekTitle
        Start-Sleep -Seconds $DwellSec
    }
    2 = {
        Set-Caption '2/6  windowrule - each app on its own workspace (Alt+1 / 2 / 3)'
        Send-Chord 'Alt+2'                          # pwsh
        Start-Sleep -Seconds ([Math]::Max(3, [int]($DwellSec / 2)))
        Send-Chord 'Alt+3'                          # sioyek, pinned to the right half
        Start-Sleep -Seconds ([Math]::Max(3, [int]($DwellSec / 2)))
        Send-Chord 'Alt+1'                          # back to zen
        Start-Sleep -Seconds $DwellSec
    }
    3 = {
        Set-Caption '3/6  Slots - a windowrule snaps a shell into the free half'
        Send-Chord 'Alt+3'
        Start-Sleep -Seconds 1
        Start-DemoShell -Exe $App.gitbash
        Start-Sleep -Seconds $DwellSec
    }
    4 = {
        Set-Caption '4/6  Spatial focus - move focus by direction (Alt+H / Alt+L)'
        # deterministic Origin on the left half
        Set-DemoFocus -Hwnd (Get-DemoShellWindow) -Label 'git-bash window'
        Start-Sleep -Milliseconds 800
        Send-Chord 'Alt+L'                           # focus right -> sioyek
        Start-Sleep -Seconds ([Math]::Max(2, [int]($DwellSec / 2)))
        Send-Chord 'Alt+H'                           # focus left -> the shell
        Start-Sleep -Seconds $DwellSec
    }
    5 = {
        Set-Caption '5/6  Move to workspace - Alt+Shift+4 sends the shell away, you stay'
        $shell = Get-DemoShellWindow
        Set-DemoFocus -Hwnd $shell -Label 'git-bash window'
        Start-Sleep -Milliseconds 800
        # Workspace 4 is the empty one, and the beat deliberately does NOT follow the
        # window there (the `silent` half of movetoworkspacesilent is the point: the split
        # collapses to the PDF alone, in view, while you stay put).
        #
        # Following it would also undo itself on camera: place-once is bounded by a
        # window's visibility lifetime (ADR-0009), so switching to workspace 4 UNCLOAKS
        # the window, that uncloak is a fresh Appeared, its `workspace 3` rule matches
        # again - and winspace sends it straight back to 3. Any rule-placed window
        # behaves that way after a manual move; only an unruled one stays where it is put.
        Move-DemoWindowToWorkspace -Hwnd $shell -Chord 'Alt+Shift+4' -Workspace 4 `
                                   -Label 'git-bash window'
        Start-Sleep -Seconds $DwellSec
    }
    6 = {
        Set-Caption '6/6  Tile - Alt+T snaps a nudged layout back onto its rules'
        Send-Chord 'Alt+3'
        Start-Sleep -Seconds 1
        # winspace is place-once: it never re-asserts on a move (ADR-0009 /
        # EVENT_OBJECT_LOCATIONCHANGE is dropped), so shove sioyek out of its
        # right-half slot exactly as a user drag would - that is what Alt+T undoes.
        $sioyek = Get-DemoWindow -TitleSub $SioyekTitle
        if ($sioyek) {
            $wa = Get-WorkArea -Hwnd $sioyek
            Move-Window -Hwnd $sioyek -X ([int]($wa.left + 140)) -Y ([int]($wa.top + 160)) `
                        -Width 760 -Height 430
        } else {
            Write-Warning "beat 6: no '$SioyekTitle' window to nudge; continuing."
        }
        Start-Sleep -Seconds ([Math]::Max(3, [int]($DwellSec / 2)))
        Send-Chord 'Alt+T'
        Start-Sleep -Seconds $DwellSec
    }
}

# -- run ----------------------------------------------------------------------
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
# Resolved here, not inside setup: -Attach skips setup, and beat 3 still needs the
# git-bash path. Fails loud on the wrong snapshot before anything is touched.
$App = Resolve-DemoApps
$beatNums = if ($Beat -gt 0) { @($Beat) } else { 1..6 }
foreach ($n in $beatNums) { if (-not $Beats.Contains($n)) { throw "Invoke-Demo: no beat $n (valid: 1..6)." } }

try {
    if (-not $Attach) {
        Write-Host "winspace demo - driving $exe" -ForegroundColor Green
        Invoke-Setup -Exe $exe -App $App | Out-Null
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
