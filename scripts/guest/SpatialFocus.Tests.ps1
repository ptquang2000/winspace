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
}

AfterAll {
    Set-RunnerConsoleVisible $true
}

Describe 'spatial-focus' {

    # The seeded default config (src/io/app.cpp) binds vim-style Win+L to `focus
    # right` (Win+<letter> registers under the same NoWinKeys policy baked into the
    # winspace-e2e-nowinkeys snapshot as the Win+<n> workspace chords). On stock
    # Windows, Win+L is reserved for Lock Workstation and can't be captured — the
    # snapshot disables that (DisableLockWorkstation), so the chord reaches
    # winspace and this seam can drive the real default `focus right` binding.
    It 'focus-right: a "focus right" chord moves the foreground to the window on the right' -Tag 'spatial-focus' {
        $winspace = $null
        $left = $null
        $right = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace

            # Two eligible windows side by side, well clear of the screen edges. Open
            # the RIGHT one first, then the LEFT one, so the last-shown LEFT window is
            # the foreground Origin the press measures from.
            $right = Start-TestWindow -Style sizable -X 520 -Y 120 -Width 360 -Height 300 -Title 'winspace-focus-right'
            $left = Start-TestWindow -Style sizable -X 80 -Y 120 -Width 360 -Height 300 -Title 'winspace-focus-left'

            # Confirm the Origin: the left window holds the keyboard before the press.
            Wait-Until -Because 'the left window to be the foreground Origin' -Condition {
                (Get-ForegroundWindow) -eq $left.Hwnd
            }

            # Act: Win+L → focus right. winspace probes live rects, resolves the right
            # window as the nearest Eligible Candidate ahead, and brings it foreground.
            Send-Chord 'Win+L'

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
