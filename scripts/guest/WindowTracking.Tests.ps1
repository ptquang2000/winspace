<#
    Window-tracking Smoke seams (VM harness, ADR-0005) — the live behaviors the
    running binary exhibits that the pure Reducer seam structurally cannot reach
    (issue 02.06). Each drives winspace on the guest desktop through the real
    win-event hook -> Probe -> Reducer -> Worker (SetWindowPos) path, with NO
    winspace source change or test hook: a genuine Win32 window (Start-TestWindow,
    a separate process) is exactly what EnumWindows and the hook see.

    Oracle policy (ADR-0005): assert on independent OS geometry where an external
    observable exists — a window's VISIBLE frame (DWMWA_EXTENDED_FRAME_BOUNDS) vs
    the monitor work area (GetMonitorInfo.rcWork), and process liveness — never on
    winspace's own log. The fill assertions are on the frame winspace compensates
    INTO: it grows the window past its invisible drop-shadow border so the visible
    edges land flush on rcWork, so frame-bounds == rcWork within a few pixels.

    Every fill seam stages a single Virtual Desktop (Set-DesktopCount 1) so the
    focused-monitor head-fill is unambiguous, and cleans up its fixtures in finally,
    so run order never matters. Multi-window BSP parity (03) and multi-Display (04)
    are out of scope.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
    # Hide the vmctl runner console for the duration: it is itself a Tileable
    # top-level window, so leaving it visible lets winspace adopt it and steal the
    # head-fill from the test form (see Set-RunnerConsoleVisible). Restored in AfterAll.
    Set-RunnerConsoleVisible $false
}

AfterAll {
    Set-RunnerConsoleVisible $true
}

Describe 'window-tracking' {

    # 02.01–02.05. A single eligible window that appears AFTER winspace is up must
    # be filled to the focused monitor's work area: the live SHOW -> hook Probe ->
    # Appeared -> layout -> PositionWindow path, with frame compensation landing the
    # visible frame flush on rcWork.
    It 'fill-one: an eligible window that opens fills the monitor work area (frame flush)' -Tag 'fill-one' {
        $winspace = $null
        $win = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace

            # Open small and off the work area, so a fill is an unmistakable move.
            $win = Start-TestWindow -Style sizable -X 120 -Y 120 -Width 480 -Height 340 -Title 'winspace-fill-one'
            Wait-Until -Because 'the opened window to be filled to the work area' -Condition {
                Test-RectNear (Get-FrameBounds $win.Hwnd) (Get-WorkArea $win.Hwnd)
            }

            $frame = Get-FrameBounds $win.Hwnd
            $work = Get-WorkArea $win.Hwnd
            Test-RectNear $frame $work | Should -BeTrue `
                -Because "the visible frame $(Format-Rect $frame) must land flush on rcWork $(Format-Rect $work)"
        } catch {
            Save-FailureScreenshot -Name 'fill-one'
            throw
        } finally {
            Stop-TestWindow $win
            Stop-Winspace -Process $winspace
        }
    }

    # 02.04 Adoption. A window already open when winspace STARTS must be inherited by
    # the startup sweep (EnumWindows -> synthetic Appeared) and filled — same path as
    # a live SHOW, so the adopted window ends up flush on rcWork just the same.
    It 'adoption: a window already open at launch is adopted and filled' -Tag 'adoption-fill' {
        $winspace = $null
        $win = $null
        try {
            Set-DesktopCount 1

            # Open BEFORE winspace, off the work area.
            $win = Start-TestWindow -Style sizable -X 160 -Y 160 -Width 500 -Height 360 -Title 'winspace-adoption'
            $spawned = Get-FrameBounds $win.Hwnd
            Test-RectNear $spawned (Get-WorkArea $win.Hwnd) | Should -BeFalse `
                -Because 'the fixture must start off the work area so adoption is an observable move'

            $winspace = Start-Winspace   # adoption sweep runs at startup
            Wait-Until -Because 'the pre-existing window to be adopted and filled' -Condition {
                Test-RectNear (Get-FrameBounds $win.Hwnd) (Get-WorkArea $win.Hwnd)
            }

            $frame = Get-FrameBounds $win.Hwnd
            $work = Get-WorkArea $win.Hwnd
            Test-RectNear $frame $work | Should -BeTrue `
                -Because "adoption must fill the pre-existing window: frame $(Format-Rect $frame) vs rcWork $(Format-Rect $work)"
        } catch {
            Save-FailureScreenshot -Name 'adoption-fill'
            throw
        } finally {
            Stop-TestWindow $win
            Stop-Winspace -Process $winspace
        }
    }

    # 02.02 reclaim (Vanished). With two windows tracked, closing the head must let
    # the survivor reclaim the fill. To make the reclaim OBSERVABLE (the survivor was
    # itself filled when it was the head), we shove it off rcWork first — winspace is
    # a place-once tiler and ignores the manual move — so its jump back to rcWork on
    # the head's close is a genuine reclaim, not a window that was already there.
    It 'reclaim: closing the head lets the survivor reclaim the fill' -Tag 'reclaim' {
        $winspace = $null
        $a = $null
        $b = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace

            $a = Start-TestWindow -Style sizable -X 100 -Y 100 -Width 480 -Height 340 -Title 'winspace-reclaim-a'
            Wait-Until -Because 'the first window to be filled' -Condition {
                Test-RectNear (Get-FrameBounds $a.Hwnd) (Get-WorkArea $a.Hwnd)
            }

            # Second window becomes the head and is filled; A is no longer the head.
            $b = Start-TestWindow -Style sizable -X 140 -Y 140 -Width 480 -Height 340 -Title 'winspace-reclaim-b'
            Wait-Until -Because 'the second window (new head) to be filled' -Condition {
                Test-RectNear (Get-FrameBounds $b.Hwnd) (Get-WorkArea $b.Hwnd)
            }

            # Shove the survivor OFF rcWork so its reclaim will be a real, visible move.
            Move-Window -Hwnd $a.Hwnd -X 60 -Y 60 -Width 300 -Height 220
            Test-RectNear (Get-FrameBounds $a.Hwnd) (Get-WorkArea $a.Hwnd) | Should -BeFalse `
                -Because 'the survivor must be off the work area before the head closes'

            # Close the head (B). Its Vanished re-runs the layout onto the new head (A).
            Close-TestWindow $b
            $b = $null
            Wait-Until -Because 'the survivor to reclaim the work-area fill' -Condition {
                Test-RectNear (Get-FrameBounds $a.Hwnd) (Get-WorkArea $a.Hwnd)
            }

            $frame = Get-FrameBounds $a.Hwnd
            $work = Get-WorkArea $a.Hwnd
            Test-RectNear $frame $work | Should -BeTrue `
                -Because "the survivor must reclaim the fill: frame $(Format-Rect $frame) vs rcWork $(Format-Rect $work)"
        } catch {
            Save-FailureScreenshot -Name 'reclaim'
            throw
        } finally {
            Stop-TestWindow $a
            Stop-TestWindow $b
            Stop-Winspace -Process $winspace
        }
    }

    # 02.02 Eligibility gate. An ineligible window (a tool window: WS_EX_TOOLWINDOW)
    # is never tracked and must be left exactly where it opened. The synchronization
    # is a real observable — not a fixed sleep: we open an ELIGIBLE window afterward
    # and wait for IT to fill, which proves winspace has processed the event stream
    # up to that point; the tool window would have moved by then had it been tracked.
    It 'ineligible: a tool window is tracked by nothing and left untouched' -Tag 'ineligible' {
        $winspace = $null
        $tool = $null
        $bait = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace

            $tool = Start-TestWindow -Style tool -X 90 -Y 90 -Width 360 -Height 260 -Title 'winspace-ineligible-tool'
            $before = Get-WindowRect $tool.Hwnd

            # Bait: an eligible window whose fill is the barrier that proves the stream
            # (including the tool window's own SHOW) has been fully processed.
            $bait = Start-TestWindow -Style sizable -X 200 -Y 200 -Width 480 -Height 340 -Title 'winspace-ineligible-bait'
            Wait-Until -Because 'the eligible bait window to fill (event stream drained)' -Condition {
                Test-RectNear (Get-FrameBounds $bait.Hwnd) (Get-WorkArea $bait.Hwnd)
            }

            $after = Get-WindowRect $tool.Hwnd
            Test-RectEqual $before $after | Should -BeTrue `
                -Because "the ineligible tool window must be untouched: was $(Format-Rect $before), now $(Format-Rect $after)"
        } catch {
            Save-FailureScreenshot -Name 'ineligible'
            throw
        } finally {
            Stop-TestWindow $tool
            Stop-TestWindow $bait
            Stop-Winspace -Process $winspace
        }
    }

    # 02.02 cloak filter (UWP). A Store app surfaces a visible, uncloaked frame
    # window (ApplicationFrameHost) PLUS a DWM-cloaked CoreWindow. winspace's gate
    # excludes the cloaked host (no phantom tile) and fills the real frame. Calculator
    # is the always-present Store app; found by caption (its stub launcher owns no
    # window). If Calculator is absent the seam FAILS loudly rather than passing empty.
    It 'cloaked-uwp: a Store app fills its real frame while the cloaked host is excluded' -Tag 'cloaked-uwp' {
        $winspace = $null
        $calc = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace

            $calc = Start-Process -FilePath 'calc.exe' -PassThru
            # The visible frame window: caption "Calculator", top-level, NOT cloaked.
            $frameHwnd = [IntPtr]::Zero
            Wait-Until -TimeoutSec 20 -Because 'the Calculator frame window to appear' -Condition {
                $hit = @(Find-WindowsByTitle 'Calculator' | Where-Object { -not (Test-WindowCloaked $_) })
                if ($hit.Count -ge 1) { $script:frameHwnd = $hit[0]; return $true }
                return $false
            }
            $frameHwnd = $script:frameHwnd
            $frameHwnd | Should -Not -Be ([IntPtr]::Zero) -Because 'the real Calculator frame must be found'

            # The real (uncloaked) frame is eligible and must be filled.
            Wait-Until -Because 'the real Calculator frame to fill the work area' -Condition {
                Test-RectNear (Get-FrameBounds $frameHwnd) (Get-WorkArea $frameHwnd)
            }
            $frame = Get-FrameBounds $frameHwnd
            $work = Get-WorkArea $frameHwnd
            Test-RectNear $frame $work | Should -BeTrue `
                -Because "the real UWP frame must fill rcWork: $(Format-Rect $frame) vs $(Format-Rect $work)"

            # No phantom tile: any cloaked Calculator-related window must NOT have been
            # moved onto rcWork (winspace never tracked it). Conditional — some builds
            # surface no separately-titled cloaked window, which is itself a clean pass.
            $cloaked = @(Find-WindowsByTitle 'Calculator' | Where-Object { Test-WindowCloaked $_ })
            foreach ($h in $cloaked) {
                Test-RectNear (Get-FrameBounds $h) (Get-WorkArea $h) | Should -BeFalse `
                    -Because 'a cloaked UWP host must never be tiled (no phantom tile)'
            }
        } catch {
            Save-FailureScreenshot -Name 'cloaked-uwp'
            throw
        } finally {
            if ($calc) { Stop-Process -Name 'CalculatorApp', 'Calculator', 'calc' -Force -ErrorAction SilentlyContinue }
            Stop-Winspace -Process $winspace
        }
    }

    # 02.04 clean unhook. On quit ($mod+Q) winspace must tear down cleanly: the
    # process exits AND its out-of-context win-event hook is unhooked (window_hook.cpp
    # UnhookWinEvent on the hooking thread). The OS removes a process's hooks on exit,
    # so process death is the primary Oracle; the behavioral proof that live tracking
    # truly stopped is that a window opened AFTER quit is NOT tiled.
    It 'clean-unhook: quit exits the process and stops all live window tracking' -Tag 'clean-unhook' {
        $winspace = $null
        $live = $null
        $post = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace
            $wsPid = $winspace.Id

            # Prove the hook is live first: a window opened now gets filled.
            $live = Start-TestWindow -Style sizable -X 130 -Y 130 -Width 480 -Height 340 -Title 'winspace-unhook-live'
            Wait-Until -Because 'live tracking to fill a window before quit' -Condition {
                Test-RectNear (Get-FrameBounds $live.Hwnd) (Get-WorkArea $live.Hwnd)
            }
            Stop-TestWindow $live
            $live = $null

            # Quit and confirm the process is gone (primary Oracle).
            Send-Chord 'Win+Q'
            Wait-Until -Because 'the winspace process to exit on quit' -Condition {
                $winspace.HasExited -or -not (Get-Process -Id $wsPid -ErrorAction SilentlyContinue)
            }
            Get-Process -Id $wsPid -ErrorAction SilentlyContinue | Should -BeNullOrEmpty `
                -Because 'quit must exit the process cleanly'

            # Behavioral proof the hook is truly gone: a window opened after quit, off
            # the work area, is NOT tiled. A bounded poll IS the observable for "nothing
            # happened" — live tiling lands in well under a second, so a 3s window with
            # no move means the hook is dead.
            $post = Start-TestWindow -Style sizable -X 70 -Y 70 -Width 320 -Height 240 -Title 'winspace-unhook-post'
            $tiled = $false
            try {
                Wait-Until -TimeoutSec 3 -Because '(expected NOT to happen) a post-quit window to be tiled' -Condition {
                    Test-RectNear (Get-FrameBounds $post.Hwnd) (Get-WorkArea $post.Hwnd)
                }
                $tiled = $true
            } catch { $tiled = $false }
            $tiled | Should -BeFalse -Because 'after a clean unhook no new window is tracked or tiled'
        } catch {
            Save-FailureScreenshot -Name 'clean-unhook'
            throw
        } finally {
            Stop-TestWindow $live
            Stop-TestWindow $post
            Stop-Winspace -Process $winspace
        }
    }
}
