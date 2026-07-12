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

    This file also carries the two Eligibility seams (`ineligible`, `uwp-frame`) folded in
    from the retired WindowTracking.Tests.ps1 when tiling was dropped (ADR-0007): those
    seams once asserted fill-to-work-area — a behaviour that no longer exists — but their
    surviving intent is the live Probe's classification, which the pure reducer cannot
    reach. Rewritten onto this same rules path: `ineligible` proves a tool window that
    matches a rule is NOT pinned (an eligible bait's pin is the barrier that proves the
    stream drained), and `uwp-frame` proves a REAL UWP ApplicationFrameHost IS classified
    eligible and pinned. Cloak EXCLUSION is not asserted here — VD-pinning re-cloaks the
    pinned frame and UWP startup cloak flips are transient, so it has no stable observable
    on this path; it stays covered by the isEligible reducer unit tests.

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

    # Eligibility-gate fixtures (the two seams folded in from the retired
    # WindowTracking.Tests.ps1 when tiling was dropped, ADR-0007). Each proves the live
    # Probe's classification on the rules path: a rule that WOULD pin a matching window
    # does NOT pin an ineligible one. A distinct title keys the Oracle on exactly the
    # fixture, never the runner console or a helper powershell.
    $script:IneligibleTitle = 'winspace-eligibility-probe'
    $script:IneligibleConfig = @"
`$mod = SUPER
bind = `$mod, Q, quit
windowrule = workspace 1, title:$script:IneligibleTitle
"@
    # UWP frame eligibility keys on Calculator's frame caption; the parser compiles a title
    # rule as std::regex, so the literal "Calculator" matches the real frame window.
    $script:UwpConfig = @"
`$mod = SUPER
bind = `$mod, Q, quit
windowrule = workspace 1, title:Calculator
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

    # Eligibility gate (live Probe). An ineligible window — a tool window
    # (WS_EX_TOOLWINDOW) — is skipped by the reducer's isEligible (reducer.cpp: the
    # Appeared arm returns early when !isEligible) even when its title matches a
    # windowrule, so it is NEVER pinned. Post-ADR-0007 there is no fill, so this is proven
    # on the rules path: an eligible BAIT with the same title, pinned to the target
    # desktop, is the barrier proving the hook stream drained PAST the tool window's own
    # Appeared (opened first, so ordered before the bait). The tool must then still sit on
    # the active desktop. (Rewritten from the fill-based `ineligible` seam of 02.06.)
    It 'ineligible: a tool window matching a rule is not pinned (Eligibility gate)' -Tag 'eligibility-ineligible' {
        $winspace = $null
        $tool = $null
        $bait = $null
        try {
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to pin to'
            $desktop1Guid = $before.Guids[0]   # rule target (inactive)
            $desktop2Guid = $before.Guids[1]   # active desktop the windows open on
            $before.CurrentGuid | Should -Be $desktop2Guid

            Set-WinspaceConfig -Content $script:IneligibleConfig | Out-Null
            $winspace = Start-Winspace

            # A tool window whose title matches the rule: ineligible, so never pinned.
            $tool = Start-TestWindow -Style tool -Title $script:IneligibleTitle -X 120 -Y 120 -Width 360 -Height 260
            (Get-WindowDesktopId -Hwnd $tool.Hwnd) | Should -Be $desktop2Guid `
                -Because 'the tool window opens on the active desktop'

            # Eligible bait, same matching title, opened AFTER the tool: its pin is the
            # barrier proving winspace has processed the stream past the tool's Appeared.
            $bait = Start-TestWindow -Title $script:IneligibleTitle -X 200 -Y 200 -Width 480 -Height 320
            Wait-Until -Because 'the eligible bait to be pinned to workspace 1 (stream drained)' -Condition {
                (Get-WindowDesktopId -Hwnd $bait.Hwnd) -eq $desktop1Guid
            }

            # The tool window was skipped by isEligible: still on the active desktop.
            $after = Get-VdState
            (Get-WindowDesktopId -Hwnd $tool.Hwnd) | Should -Be $desktop2Guid `
                -Because "an ineligible tool window must not be pinned by the rule"
            $after.CurrentGuid | Should -Be $desktop2Guid `
                -Because 'pinning the bait must not switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'eligibility-ineligible'
            throw
        } finally {
            Stop-TestWindow $tool
            Stop-TestWindow $bait
            Stop-Winspace -Process $winspace
        }
    }

    # UWP frame eligibility (live Probe). A Store app (Calculator) surfaces a real
    # ApplicationFrameHost frame window PLUS DWM-cloaked siblings (a CoreWindow). This seam
    # proves the one thing reliably observable on the rules path that the pure reducer
    # cannot reach: the live Probe classifies a REAL UWP frame as Eligible, so a
    # `title:Calculator` rule pins it — driving an actual ApplicationFrameHost (not a
    # synthetic WindowAttrs) through Probe -> isEligible -> MoveViewToDesktop. Calculator is
    # the always-present Store app; if it is absent the seam FAILS loudly, not empty.
    #
    # It deliberately does NOT assert cloak EXCLUSION. On a real UWP app the cloak is
    # transient — a cloaked-SHOW CoreWindow is re-evaluated on its later UNCLOAKED edge
    # (reducer.cpp) and can legitimately become eligible and be pinned too — and VD-pinning
    # re-cloaks the eligible frame itself (off-desktop cloak). So there is no stable "stays
    # cloaked, never pinned" observable on this path (an earlier attempt flagged the
    # legitimately-uncloaked CoreWindow as a false phantom pin). Cloak exclusion is covered
    # deterministically by the isEligible reducer unit tests; the live negative-classification
    # path is covered by the `ineligible` seam above (a tool window a rule never pins).
    #
    # Uses the ADOPTION path (launch, capture the uncloaked frame's HWND, THEN start
    # winspace) so the frame is identified before the pin cloaks it as an off-desktop window.
    It 'uwp-frame: a rule pins a real UWP app frame (adoption)' -Tag 'eligibility-uwp-frame' {
        $winspace = $null
        $calc = $null
        try {
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to pin to'
            $desktop1Guid = $before.Guids[0]
            $desktop2Guid = $before.Guids[1]
            $before.CurrentGuid | Should -Be $desktop2Guid

            Set-WinspaceConfig -Content $script:UwpConfig | Out-Null

            # Launch Calculator BEFORE winspace; capture the uncloaked frame while it is
            # still on the active desktop (no pin yet, so no off-desktop cloak).
            $calc = Start-Process -FilePath 'calc.exe' -PassThru
            $script:frameHwnd = [IntPtr]::Zero
            Wait-Until -TimeoutSec 30 -Because 'the Calculator frame window to appear (uncloaked, active desktop)' -Condition {
                $hit = @(Find-WindowsByTitle 'Calculator' | Where-Object { -not (Test-WindowCloaked $_) })
                if ($hit.Count -ge 1) { $script:frameHwnd = $hit[0]; return $true }
                return $false
            }
            $frameHwnd = $script:frameHwnd
            $frameHwnd | Should -Not -Be ([IntPtr]::Zero) -Because 'the real Calculator frame must be found'
            (Get-WindowDesktopId -Hwnd $frameHwnd) | Should -Be $desktop2Guid `
                -Because 'the frame opens on the active desktop before winspace adopts it'

            # winspace boots; startup adoption sweeps the pre-existing frame into the rule.
            $winspace = Start-Winspace
            Wait-Until -Because 'adoption to pin the real Calculator frame to workspace 1' -Condition {
                (Get-WindowDesktopId -Hwnd $frameHwnd) -eq $desktop1Guid
            }

            # The eligible real UWP frame was classified and pinned to the target desktop.
            (Get-WindowDesktopId -Hwnd $frameHwnd) | Should -Be $desktop1Guid `
                -Because "the eligible UWP frame must be pinned to workspace 1's desktop"
            (Get-VdState).CurrentGuid | Should -Be $desktop2Guid `
                -Because 'pinning the frame must not switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'eligibility-uwp-frame'
            throw
        } finally {
            if ($calc) { Stop-Process -Name 'CalculatorApp', 'Calculator', 'calc' -Force -ErrorAction SilentlyContinue }
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
