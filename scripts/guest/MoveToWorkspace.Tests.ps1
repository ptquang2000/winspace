<#
    Move-to-workspace Smoke seam (VM harness, ADR-0005) — the live-only behaviour
    the pure Reducer seam structurally cannot reach (issue 06): on a real
    `movetoworkspacesilent N` chord the focused window is actually reassigned to an
    inactive Virtual Desktop, while the active desktop stays put. The follow-vs-
    silent branching is fully reducer-tested (reducer_test.cpp); this seam proves
    only that winspace's MoveForegroundWindowToWorkspace Effect — the public
    IVirtualDesktopManager::MoveWindowToDesktop path (ADR-0010), wrapped in the DWM
    cloak/uncloak — lands the window on the target desktop on a real session.

    Drives winspace through the real RegisterHotKey -> WM_HOTKEY -> Reducer ->
    Worker (GetForegroundWindow -> cloak -> bridge move -> uncloak) path with NO
    winspace source change: a genuine Win32 window (Start-TestWindow, a separate
    process) is exactly what GetForegroundWindow and MoveWindowToDesktop see.

    Oracle policy (ADR-0005): assert on independent OS state — the window's desktop
    GUID from the PUBLIC IVirtualDesktopManager::GetWindowDesktopId (Get-WindowDesktopId),
    checked against Get-VdState's registry GUID list — never on winspace's own log.

    NOTE: the *no-flash* guarantee itself (the window never painting on the current
    desktop mid-move) is a purely visual property with no automatable observable, so
    it stays a manual smoke step. What this seam proves is the automatable half: the
    window ends up on the inactive target desktop and the active desktop is unchanged
    (silent). The cloak/uncloak wrapping is exercised on the path either way.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
    # The vmctl runner console is an Eligible top-level window that can hold the
    # foreground; hide it so the test window is unambiguously the move's Origin
    # (GetForegroundWindow). Restored in AfterAll.
    Set-RunnerConsoleVisible $false
}

AfterAll {
    Set-RunnerConsoleVisible $true
}

Describe 'move-to-workspace' {

    # The seeded default config (src/win32.cpp) binds Alt+Shift+<n> to
    # `movetoworkspacesilent N` (Alt+Shift+<digit> registers on a stock Windows 11 with
    # no policy, ADR-0014, as do the Alt+<n> workspace chords; the snapshot is just
    # winspace-e2e). This seam drives the real default `movetoworkspacesilent 1` binding.
    It 'silent move: the focused window is reassigned to the inactive target desktop, and the active desktop stays' -Tag 'move-to-workspace' {
        $winspace = $null
        $window = $null
        try {
            # Two desktops; Win+Ctrl+D leaves us on the LAST created, i.e. desktop 2.
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2 -Because 'the seam needs an inactive desktop to move to'
            $desktop1Guid = $before.Guids[0]   # workspace 1 — the inactive target
            $desktop2Guid = $before.Guids[1]   # workspace 2 — the active desktop we stay on
            $before.CurrentGuid | Should -Be $desktop2Guid -Because 'Set-DesktopCount 2 leaves the active desktop on the last-created (2)'

            # winspace adopts the two live desktops as workspaces 1..2 by GUID.
            $winspace = Start-Winspace

            # A real top-level window on the CURRENT desktop (2), foreground, so the
            # move (ungated by Eligibility) targets exactly it.
            $window = Start-TestWindow -Style sizable -X 200 -Y 160 -Width 480 -Height 320 -Title 'winspace-move-silent'

            Wait-Until -Because 'the test window to be the foreground Origin' -Condition {
                (Get-ForegroundWindow) -eq $window.Hwnd
            }
            # Precondition: it starts on the active desktop (2).
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $desktop2Guid `
                -Because 'the window opens on the current (active) desktop before the move'

            # Act: Alt+Shift+1 -> movetoworkspacesilent 1. winspace resolves the
            # foreground window, cloaks it (target 1 != current 2), moves it to
            # desktop 1's GUID via the public manager, then uncloaks — without switching.
            Send-Chord 'Alt+Shift+1'

            Wait-Until -Because 'the window to be reassigned to the inactive desktop 1' -Condition {
                (Get-WindowDesktopId -Hwnd $window.Hwnd) -eq $desktop1Guid
            }

            # Assert the move landed AND the active desktop never moved (silent).
            $after = Get-VdState
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $desktop1Guid `
                -Because 'movetoworkspacesilent 1 must reassign the window to workspace 1''s desktop'
            $after.CurrentGuid | Should -Be $desktop2Guid `
                -Because 'the silent form must NOT switch the active desktop'
        } catch {
            Save-FailureScreenshot -Name 'move-silent'
            throw
        } finally {
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }
}
