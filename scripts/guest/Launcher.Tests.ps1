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
`$mod = SUPER
bind = `$mod, Q, quit
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
    # This is fully asserted at the pure Reducer seam (reducer_test.cpp). It stays a
    # -Skip here because the reload TRIGGER (file watch / `reload` dispatcher) is not
    # wired in this slice — only Started{} is posted (ADR-0011 / PRD 08). Once PRD 09
    # lands a reload event source, promote this to a live Get-Process-count-unchanged
    # assertion across a reload.
    It 'exec-once: a reload does not relaunch an already-running app' -Skip -Tag 'launcher' {
        Set-ItResult -Skipped -Because 'the reload trigger lands with PRD 09; only Started{} is wired this slice'
    }
}
