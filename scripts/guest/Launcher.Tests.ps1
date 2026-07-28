<#
    Launcher Smoke seam (VM harness, ADR-0005) — the live-only behaviour the pure
    Reducer/parser seams structurally cannot reach (PRD 08 / issue 08, launch-only
    per ADR-0011): winspace actually STARTS a declared `exec-once` app via
    CreateProcessW at startup, and — because placement is NOT the launcher's job —
    a paired `windowrule = workspace N, exe:…` pins that launched window to its
    target Virtual Desktop the moment it appears.

    The parser (ExecEntry vector, source order, once flag) and the reducer
    (Started{} -> LaunchApp for every entry; Reloaded{} -> exec-only) are fully
    unit-tested (config_test.cpp / reducer_test.cpp). This seam proves only what
    those cannot: that the Worker's CreateProcessW adapter genuinely spawns a
    detached child on a real session, and that the launch composes with the landed
    windowrule placement path (PRD 07) end-to-end — launch here, place there.

    Launch target: msinfo32.exe (System Information, System32) — a classic
    single-process Win32 app whose main window is RESIZABLE (WS_THICKFRAME) and
    captioned, so it clears winspace's Eligibility gate (isEligible requires
    thickFrame + caption; reducer.cpp), which the windowrule placement path demands
    (an ineligible window is never placed). Its window is owned by a process whose exe
    is exactly `msinfo32.exe`, so an `exe:msinfo32.exe` rule matches it unambiguously
    (reducer.cpp: exe compares exact, ASCII-case-insensitive, on the basename). Being
    single-window, its HWND is read straight off the process (MainWindowHandle) purely
    to LOCATE it for the Oracle — never to place it (placement is driven by the exe
    rule, the feature under test). This is the target charmap.exe could NOT be — charmap
    is single-process but fixed-size (no thickFrame), so it is ineligible and the rule
    never fires; the Store-packaged notepad/mspaint are the opposite failure, surfacing
    their window under a different process than CreateProcess returned. msinfo32 is both
    single-process AND eligible, so nothing about it is fragile here.

    Oracle policy (ADR-0005): assert on independent OS state — the child is running
    (Get-Process) and its window's desktop GUID from the PUBLIC
    IVirtualDesktopManager::GetWindowDesktopId (Get-WindowDesktopId), checked against
    Get-VdState's registry GUID list — never on winspace's own log.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any launched window reaches the desktop

    $script:LaunchExe = 'msinfo32'           # bare name; System32 is on %PATH%

    # Minimal seam config: launch msinfo32 once, and pin it to workspace 1 by exe.
    # A double-quoted here-string so `$mod is escaped to survive to the file while
    # $LaunchExe expands here. winspace re-seeds its default once the file is cleared.
    $script:LauncherConfig = @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
bind = `$mod SHIFT, R, reload
exec-once = $script:LaunchExe
windowrule = workspace 1, exe:$script:LaunchExe.exe
"@
}

AfterAll {
    Clear-WinspaceConfig   # restore built-in-default behaviour for later seams
}

Describe 'launcher' {

    # Launch + place, end-to-end. winspace boots, its Started{} fires the exec-once
    # launch (CreateProcessW spawns msinfo32), the launched window appears on the
    # current desktop (2), the live SetWinEventHook observes its Appeared, and the
    # paired exe windowrule pins it to the inactive workspace 1 — without switching
    # the active desktop. The two independent halves the unit tests cannot reach:
    # the child actually started, and it landed on the rule's target desktop.
    It 'exec-once: a launched app starts and lands on the workspace named by its paired exe windowrule' -Tag 'launcher' {
        $winspace = $null
        try {
            # Two desktops; Win+Ctrl+D leaves us on the LAST created, i.e. desktop 2.
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to pin the launched app to'
            $desktop1Guid = $before.Guids[0]   # workspace 1 — the inactive pin target
            $desktop2Guid = $before.Guids[1]   # workspace 2 — the active desktop we stay on
            $before.CurrentGuid | Should -Be $desktop2Guid -Because 'Set-DesktopCount 2 leaves the active desktop on the last-created (2)'

            # No msinfo32 must be running yet, or a stale window would spoof the Oracle.
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the seam proves winspace STARTED msinfo32, so none may pre-exist'

            Set-WinspaceConfig -Content $script:LauncherConfig | Out-Null

            # winspace adopts the two live desktops as workspaces 1..2 by GUID, seeds
            # the exec entry + rule in the Worker ctor, then posts Started{} — which
            # emits the LaunchApp Effect the Worker runs as CreateProcessW.
            $winspace = Start-Winspace

            # The launch half: msinfo32 actually started, and it owns a top-level window.
            # Read the HWND straight off the process (MainWindowHandle) — msinfo32 is a
            # single-window classic app, so this is unambiguous and sidesteps EnumWindows.
            # Cache it while still on the current desktop; the handle stays valid after
            # the move (a cross-desktop window stays WS_VISIBLE, cloaked not un-styled),
            # so polling GetWindowDesktopId on it never races the pin.
            $script:LaunchedHwnd = [IntPtr]::Zero
            Wait-Until -Because 'winspace to launch msinfo32 and its window to appear' -Condition {
                $p = Get-Process $script:LaunchExe -ErrorAction SilentlyContinue
                if (-not $p) { return $false }
                $h = @($p)[0].MainWindowHandle
                if ($h -ne [IntPtr]::Zero) { $script:LaunchedHwnd = $h; return $true }
                return $false
            }

            (Get-Process $script:LaunchExe -ErrorAction SilentlyContinue) |
                Should -Not -BeNullOrEmpty -Because 'the exec-once entry must have started the child via CreateProcessW'

            # The placement half: the paired exe windowrule pins the launched window
            # to workspace 1's desktop, on the Appeared edge, without switching away.
            Wait-Until -Because 'the exe windowrule to pin the launched window to workspace 1' -Condition {
                (Get-WindowDesktopId -Hwnd $script:LaunchedHwnd) -eq $desktop1Guid
            }

            $after = Get-VdState
            (Get-WindowDesktopId -Hwnd $script:LaunchedHwnd) | Should -Be $desktop1Guid `
                -Because 'the paired windowrule must place the launched app on workspace 1''s desktop'
            $after.CurrentGuid | Should -Be $desktop2Guid `
                -Because 'placing a launched app on an inactive workspace must NOT switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'launcher-exec-once-place'
            throw
        } finally {
            # The launched child is detached by design (outlives winspace), so this
            # seam owns its cleanup explicitly — kill every msinfo32 before the next seam.
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Stop-Winspace -Process $winspace
        }
    }

    # exec-once idempotency across a reload: the reducer emits LaunchApp for exec-once
    # ONLY on Started{}, never on Reloaded{}, so a reload must not spawn a second copy.
    # The pure half is reducer-tested (reducer_test.cpp); PRD 09 landed the reload
    # TRIGGER (the `reload` dispatcher), so this is now a live Get-Process-count seam:
    # boot with an exec-once app, fire `reload`, and assert exactly one copy survives.
    It 'exec-once: a reload does not relaunch an already-running app' -Tag 'launcher' {
        $winspace = $null
        try {
            Set-DesktopCount 1
            # No msinfo32 may pre-exist, or a stale window spoofs the count Oracle.
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the seam counts the copies winspace itself launched'

            Set-WinspaceConfig -Content $script:LauncherConfig | Out-Null
            $winspace = Start-Winspace

            # Started{} fires the exec-once launch — exactly one msinfo32 comes up.
            Wait-Until -Because 'winspace to launch the single exec-once msinfo32' -Condition {
                @(Get-Process $script:LaunchExe -ErrorAction SilentlyContinue).Count -eq 1
            }

            # Fire `reload` on the SAME (unchanged) config. Reloaded{} re-launches only
            # `exec` entries, never `exec-once`, so the count must stay at one.
            Send-Chord 'Alt+Shift+R'
            Wait-Until -Because 'the Worker to apply the reload' -Condition {
                (Get-WinspaceLogText) -match 'reload: applied new config'
            }

            # Give any (erroneous) relaunch a bounded window to surface, then assert
            # the count is unchanged — exec-once did not spawn a second copy.
            Start-Sleep -Milliseconds 600
            @(Get-Process $script:LaunchExe -ErrorAction SilentlyContinue).Count | Should -Be 1 `
                -Because 'exec-once is launched on Started{} only, never re-launched on reload'
        } catch {
            Save-FailureScreenshot -Name 'launcher-reload-idempotent'
            throw
        } finally {
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Stop-Winspace -Process $winspace
        }
    }

    # ADR-0025's launcher half: `exec-once = msedge` used to fail with "file not
    # found" even though msedge starts fine from the Run dialog. CreateProcessW parses
    # the command line itself and searches %PATH% — but NOT the shell's registered
    # application paths, which is where installers put most normally-installed
    # applications (browsers, Office, developer tools). The fallback widens WHERE
    # Win32 looks; the command is still stored unparsed and handed over verbatim.
    #
    # The registration is a FIXTURE the seam creates, not an app it hopes the guest
    # has: a synthetic bare name — on no PATH anywhere, so CreateProcessW must fail
    # not-found first — registered in HKCU App Paths pointing at msinfo32. That makes
    # the fallback the only possible route to a running process, and it exercises the
    # HKCU-before-HKLM precedence too. Discovering a real installed app instead was
    # tried and is a trap: an arbitrary App Paths entry is as likely to be a
    # fire-and-exit diagnostic tool as a long-running app, so the Oracle would be
    # racing the target's own exit.
    It 'apppath: a Launch entry naming a registered application not on PATH starts it' -Tag 'launcher-apppath' {
        $winspace = $null
        $bare = 'winspace-apppath-probe'
        $key = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$bare.exe"
        try {
            $realExe = Join-Path $env:SystemRoot "System32\$script:LaunchExe.exe"
            Test-Path $realExe | Should -BeTrue -Because 'the fixture points the registration at a real image'

            # The bare name must genuinely be unresolvable, or the fallback is not what
            # started anything.
            Get-Command $bare -CommandType Application -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the whole point is a name PATH cannot resolve'
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the seam proves winspace STARTED it, so none may pre-exist'

            New-Item -Path $key -Force | Out-Null
            Set-ItemProperty -Path $key -Name '(default)' -Value $realExe

            Set-WinspaceConfig -Content @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
exec-once = $bare
"@ | Out-Null

            $winspace = Start-Winspace

            # The Oracle is the running process — not the log. Bare unresolvable name
            # in, real process out; only the application-path fallback can do that.
            Wait-Until -TimeoutSec 20 -Because "winspace to start '$bare' via its registered application path" -Condition {
                [bool](Get-Process $script:LaunchExe -ErrorAction SilentlyContinue)
            }
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue |
                Should -Not -BeNullOrEmpty -Because 'exec-once must work for a bare registered application name'
        } catch {
            Save-FailureScreenshot -Name 'launcher-apppath'
            throw
        } finally {
            Get-Process $script:LaunchExe -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue
            Stop-Winspace -Process $winspace
        }
    }

    # The fallback must widen WHERE Win32 looks without touching WHAT it was given.
    # The retry passes the resolved image as lpApplicationName and the ORIGINAL command
    # line unchanged, so arguments have to survive — the registration here points at
    # cmd.exe and the arguments are what actually produce the observable process.
    It 'apppath: a Launch entry WITH ARGUMENTS still works when resolved through the fallback' -Tag 'launcher-apppath' {
        $winspace = $null
        $bare = 'winspace-apppath-args'
        $key = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$bare.exe"
        try {
            Get-Command $bare -CommandType Application -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the bare name must be unresolvable on PATH'
            Get-Process 'PING' -ErrorAction SilentlyContinue |
                Should -BeNullOrEmpty -Because 'the Oracle process must not pre-exist'

            New-Item -Path $key -Force | Out-Null
            Set-ItemProperty -Path $key -Name '(default)' -Value (Join-Path $env:SystemRoot 'System32\cmd.exe')

            # Only the ARGUMENTS can produce a ping; if they were dropped or rewritten,
            # cmd would start with no command and nothing would be observable.
            Set-WinspaceConfig -Content @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
exec-once = $bare /c ping -n 60 127.0.0.1
"@ | Out-Null

            $winspace = Start-Winspace

            Wait-Until -TimeoutSec 20 -Because 'the arguments to survive the fallback retry' -Condition {
                [bool](Get-Process 'PING' -ErrorAction SilentlyContinue)
            }
            Get-Process 'PING' -ErrorAction SilentlyContinue |
                Should -Not -BeNullOrEmpty -Because 'the command line is passed through verbatim on the retry'
        } catch {
            Save-FailureScreenshot -Name 'launcher-apppath-args'
            throw
        } finally {
            Get-Process 'PING', 'cmd' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue
            Stop-Winspace -Process $winspace
        }
    }

    # The negative half: something that exists nowhere still fails, and the diagnostic
    # names the CAUSE — not on PATH and not a registered application — rather than
    # only echoing the raw Win32 error. One bad entry never takes the WM down.
    It 'apppath: a Launch entry naming nothing that exists fails with a diagnostic that names the cause' -Tag 'launcher-apppath' {
        $winspace = $null
        try {
            Set-WinspaceConfig -Content @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
exec-once = winspace-no-such-program-xyz
"@ | Out-Null

            # winspace still starts and still reaches Connected — a failed launch
            # degrades and continues (ADR-0004).
            $winspace = Start-Winspace
            Wait-Until -Because 'the launch failure to be diagnosed' -Condition {
                (Get-WinspaceLogText) -match 'not found on PATH and is not a registered application'
            }
            $winspace.HasExited | Should -BeFalse -Because 'one bad exec entry must never take down the WM'
        } catch {
            Save-FailureScreenshot -Name 'launcher-apppath-missing'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
