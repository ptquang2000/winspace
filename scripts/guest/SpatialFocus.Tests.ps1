<#
    Spatial-focus Smoke seam (VM harness, ADR-0005) — the one live-only behavior
    the pure Reducer seam structurally cannot reach (issue 05): on a real `focus`
    keypress the foreground window actually changes to the spatial neighbour. The
    directional-resolution brain itself is fully reducer-tested (reducer_test.cpp),
    including cross-monitor; this seam proves only that winspace's
    SetForegroundWindow Effect lands on a real session — the thing a unit test
    cannot observe (Win32's foreground-lock rules).

    Drives winspace through the real RegisterHotKey -> WM_HOTKEY -> Reducer ->
    Worker (EnumWindows Probe -> SetForegroundWindow) path with NO winspace source
    change: two genuine Win32 windows (Start-TestWindow, separate processes) are
    exactly what EnumWindows and GetForegroundWindow see.

    Oracle policy (ADR-0005): assert on independent OS state — GetForegroundWindow
    before/after the chord — never on winspace's own log. The VM is single-display,
    so this covers same-screen movement only; cross-monitor stays reducer-tested.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
    # The vmctl runner console is itself an Eligible (WS_THICKFRAME|WS_CAPTION)
    # top-level window, so leaving it visible would make it a stray focus Candidate.
    # Hide it for the duration; restored in AfterAll.
    Set-RunnerConsoleVisible $false
    # A cold, freshly-reverted guest can auto-open the Widgets board — a full-height
    # left-side flyout that HOLDS the foreground and covers where the test windows
    # open, so the left window can never become the foreground Origin (confirmed by a
    # failure screenshot). Kill its host so the desktop is clear before staging windows;
    # it does not reopen without a user trigger. This was the real cause of the
    # intermittent cold-guest failure, not the Start-TestWindow / Origin timeouts.
    Get-Process -Name 'Widgets' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    # Distribute (ADR-0020) auto-maximizes every UNMATCHED eligible window. Left
    # unmanaged, BOTH fixture windows would be maximized to identical full-work-area
    # rects — neither is spatially "ahead" of the other, so `focus right` resolves to
    # nothing (resolveFocus finds no Candidate) and the foreground can never move: the
    # seam would deadlock waiting on the right window. The fixtures must therefore keep
    # (a) the rects they open at (so left/right is a real spatial relation) AND (b)
    # their standing as focus Candidates.
    #
    # An `ignore` rule (the obvious first reach) fails (b): ADR-0020 widened Ignore to
    # "don't touch at all", which drops the window as a Spatial-focus target — resolveFocus
    # skips every id in `ignored` (winspace.cpp). A workspace-only Place rule (NO slot) is
    # exactly right on all three counts: a matched rule opts the window OUT of Distribute
    # (so it is never maximized); a slot-less Place emits no PositionWindow (so the window
    # stays byte-for-byte where it opened); and being Place — not Ignore — it remains a
    # focus Candidate. On this single desktop the rule's workspace 1 IS the current one,
    # so the Workspace move is a no-op. Title matches by regex_search (substring), so the
    # single `winspace-focus` pattern covers both `-left` and `-right`. The `focus` binds
    # are re-declared here (Alt+H/J/K/L, identical to the seeded default) because a custom
    # config replaces the default whole; the chord under test (Alt+L → focus right) is
    # unchanged. A double-quoted here-string escapes `$mod so it survives to the file.
    $script:FocusConfig = @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
bind = `$mod, H, focus, left
bind = `$mod, J, focus, down
bind = `$mod, K, focus, up
bind = `$mod, L, focus, right
windowrule = workspace 1, title:winspace-focus
"@
}

AfterAll {
    Set-RunnerConsoleVisible $true
    Clear-WinspaceConfig   # restore built-in-default behaviour for later seams
}

Describe 'spatial-focus' {

    # The config binds vim-style $mod + h/j/k/l to `focus` (Alt+L → focus right),
    # where $mod = ALT (ADR-0014), identical to the seeded default (src/win32.cpp).
    # Alt+<letter> registers on stock Windows 11 with no policy — verified by direct
    # RegisterHotKey probe on this guest. The paired `workspace 1` Place rules keep the
    # two fixtures out of Distribute while leaving their rects and focus candidacy
    # intact (see $script:FocusConfig for the full rationale).
    It 'focus-right: a "focus right" chord moves the foreground to the window on the right' -Tag 'spatial-focus' {
        $winspace = $null
        $left = $null
        $right = $null
        try {
            Set-DesktopCount 1   # single desktop: the rules' workspace 1 is the current one
            Set-WinspaceConfig -Content $script:FocusConfig | Out-Null
            $winspace = Start-Winspace

            # Two eligible windows side by side, well clear of the screen edges. Open
            # the RIGHT one first, then the LEFT one, so the last-shown LEFT window is
            # the foreground Origin the press measures from.
            $right = Start-TestWindow -Style sizable -X 520 -Y 120 -Width 360 -Height 300 -Title 'winspace-focus-right'
            $left = Start-TestWindow -Style sizable -X 80 -Y 120 -Width 360 -Height 300 -Title 'winspace-focus-left'

            # Pin the Origin deterministically: on a cold guest the last-shown window
            # (a separate process) is NOT guaranteed the foreground — focus-stealing
            # prevention blocks a background process's activation. A synthesized click
            # is real input, so it legitimately brings the left window forward.
            Set-ForegroundByClick -Hwnd $left.Hwnd
            Wait-Until -TimeoutSec 15 -Because 'the left window to be the foreground Origin' -Condition {
                (Get-ForegroundWindow) -eq $left.Hwnd
            }

            # Act: Alt+L → focus right. winspace probes live rects, resolves the right
            # window as the nearest Eligible Candidate ahead, brings it foreground.
            Send-Chord 'Alt+L'

            Wait-Until -Because 'the foreground to move to the right window' -Condition {
                (Get-ForegroundWindow) -eq $right.Hwnd
            }

            # Assert on the delta: the foreground is now the right window, not the left.
            $fg = Get-ForegroundWindow
            $fg | Should -Be $right.Hwnd -Because 'the "focus right" chord must land the foreground on the right neighbour'
            $fg | Should -Not -Be $left.Hwnd
        } catch {
            Save-FailureScreenshot -Name 'focus-right'
            throw
        } finally {
            Stop-TestWindow $left
            Stop-TestWindow $right
            Stop-Winspace -Process $winspace
        }
    }
}
