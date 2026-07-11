<#
    Window-rule place-once Smoke seam (VM harness, ADR-0005) — the live-only
    behaviour the pure Reducer seam structurally cannot reach (PRD 06 / issue 07):
    a `windowrule = workspace N, <field>:<pattern>` pins a matching real app to its
    target Virtual Desktop the moment the window appears — both on a LIVE SHOW
    (Appeared) and on startup ADOPTION (an app already open when winspace launches).

    The matcher, place-once state (`placed`), and the MoveWindowToWorkspace Effect are
    fully reducer-tested (reducer_test.cpp). This seam proves only what the unit tests
    cannot: that winspace's reintroduced SetWinEventHook adapter (src/io/window_hook.cpp)
    genuinely observes a foreign window's SHOW / EnumWindows adoption, Probes its
    identity, and drives the internal MoveViewToDesktop (ADR-0010) so the window lands
    on the rule's target desktop on a real session.

    Matching is by TITLE, not exe: Start-TestWindow's host process is powershell.exe
    (shared with the runner console and helpers), so an `exe:` rule is ambiguous; the
    test window's distinct -Title is an unambiguous Oracle key. The parser compiles a
    title rule as a std::regex, so the literal title matches.

    Oracle policy (ADR-0005): assert on independent OS state — the window's desktop
    GUID from the PUBLIC IVirtualDesktopManager::GetWindowDesktopId (Get-WindowDesktopId),
    checked against Get-VdState's registry GUID list — never on winspace's own log.

    NOTE: the *no-flash* guarantee (a cross-desktop pin never painting on the current
    desktop) is a purely visual property with no automatable observable, so it stays a
    manual smoke step. What this seam proves is the automatable half: the matching
    window ends up on the inactive target desktop and the active desktop is unchanged.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # A distinct title so the rule (and this test's Oracle) key on exactly this window,
    # never the runner console or a helper powershell.
    $script:RuleTitle = 'winspace-rule-pin'

    # Minimal seam config: the rule under test plus a bound quit for the teardown seam.
    # A double-quoted here-string, so `$mod is escaped to survive to the file while
    # $RuleTitle expands here. winspace re-seeds its default once the file is cleared.
    $script:RuleConfig = @"
`$mod = SUPER
bind = `$mod, Q, quit
windowrule = workspace 1, title:$script:RuleTitle
"@
}

AfterAll {
    Clear-WinspaceConfig   # restore built-in-default behaviour for later seams
}

Describe 'window-rules' {

    # A live SHOW: winspace is already up (rule seeded in the Worker ctor, hooks live)
    # when the matching window opens on the current desktop. The hook's Appeared drives
    # the pin to the inactive workspace 1 without switching the active desktop.
    It 'live pin: a windowrule reassigns a matching window to its target workspace on Appeared' -Tag 'window-rules' {
        $winspace = $null
        $window = $null
        try {
            # Two desktops; Win+Ctrl+D leaves us on the LAST created, i.e. desktop 2.
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to pin to'
            $desktop1Guid = $before.Guids[0]   # workspace 1 — the inactive pin target
            $desktop2Guid = $before.Guids[1]   # workspace 2 — the active desktop we stay on
            $before.CurrentGuid | Should -Be $desktop2Guid -Because 'Set-DesktopCount 2 leaves the active desktop on the last-created (2)'

            Set-WinspaceConfig -Content $script:RuleConfig | Out-Null

            # winspace adopts the two live desktops as workspaces 1..2 by GUID and seeds
            # the rule before any Appeared can arrive.
            $winspace = Start-Winspace

            # A real top-level window on the CURRENT desktop (2) whose title matches the
            # rule. Its SHOW reaches winspace through the live SetWinEventHook.
            $window = Start-TestWindow -Title $script:RuleTitle -X 200 -Y 160 -Width 480 -Height 320

            Wait-Until -Because 'the rule to pin the matching window to workspace 1' -Condition {
                (Get-WindowDesktopId -Hwnd $window.Hwnd) -eq $desktop1Guid
            }

            $after = Get-VdState
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $desktop1Guid `
                -Because 'the windowrule must reassign the matching window to workspace 1''s desktop'
            $after.CurrentGuid | Should -Be $desktop2Guid `
                -Because 'a cross-desktop pin must NOT switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'window-rule-live-pin'
            throw
        } finally {
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }

    # Startup ADOPTION: the matching window is already open when winspace launches. The
    # hook thread's EnumWindows posts a synthetic Appeared per top-level window through
    # the same path a live SHOW takes, so the rule pins the pre-existing window too.
    It 'adoption: an app already open when winspace starts is pinned by a windowrule' -Tag 'window-rules' {
        $winspace = $null
        $window = $null
        try {
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to pin to'
            $desktop1Guid = $before.Guids[0]
            $desktop2Guid = $before.Guids[1]
            $before.CurrentGuid | Should -Be $desktop2Guid

            Set-WinspaceConfig -Content $script:RuleConfig | Out-Null

            # The window exists FIRST, on the active desktop (2), before winspace.
            $window = Start-TestWindow -Title $script:RuleTitle -X 200 -Y 160 -Width 480 -Height 320
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $desktop2Guid `
                -Because 'the window opens on the active desktop before winspace adopts it'

            # winspace boots; adoption sweeps the pre-existing window into the rule.
            $winspace = Start-Winspace

            Wait-Until -Because 'adoption to pin the pre-existing window to workspace 1' -Condition {
                (Get-WindowDesktopId -Hwnd $window.Hwnd) -eq $desktop1Guid
            }

            $after = Get-VdState
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $desktop1Guid `
                -Because 'adoption must pin a pre-existing matching window to workspace 1''s desktop'
            $after.CurrentGuid | Should -Be $desktop2Guid `
                -Because 'adoption pin must NOT switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'window-rule-adoption'
            throw
        } finally {
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }

    # Clean teardown: a bound `quit` must unwind all three threads — Worker, Hotkey, and
    # the Hook thread that UnhookWinEvent's both hooks after its loop returns. The only
    # automatable observable for "no dangling SetWinEventHook" is that every join()
    # completes and the process exits 0; a hung thread or a hook left registered would
    # stall the exit and this Wait-Until would time out.
    It 'clean teardown: a bound quit exits winspace with no dangling hook' -Tag 'window-rules' {
        $winspace = $null
        try {
            Set-WinspaceConfig -Content $script:RuleConfig | Out-Null
            $winspace = Start-Winspace
            # Cache a native handle while the process is still alive. Without this a
            # Start-Process -PassThru object reads .ExitCode back as $null after exit
            # (it never opened a query-capable handle); touching .Handle now fixes that.
            $null = $winspace.Handle

            Send-Chord 'Win+Q'   # $mod, Q -> quit

            # Blocking WaitForExit (not a HasExited poll): it both proves no thread or
            # hook is left hanging AND settles the cached ExitCode — a polled HasExited
            # leaves .ExitCode $null on a Start-Process object.
            $exited = $winspace.WaitForExit(8000)
            $exited | Should -BeTrue `
                -Because 'the bound quit must unwind all three threads (a dangling hook would stall the exit)'
            $winspace.ExitCode | Should -Be 0 `
                -Because 'clean teardown returns 0 once the Hook thread has unhooked and all threads joined'
        } catch {
            Save-FailureScreenshot -Name 'window-rule-teardown'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
