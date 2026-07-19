<#
    Slot-placement + Distribute Smoke seams (VM harness, ADR-0005 / ADR-0016 / ADR-0020) —
    the live geometry write the pure Reducer seam structurally cannot reach. A
    `windowrule = workspace N slot <name>, <field>:<pattern>` drops a matching real app
    into its Slot the moment it appears (place-once); an unmatched window is auto-placed
    by **Distribute** (maximized on its Display, ADR-0020); an `ignore` rule leaves a
    window entirely alone; and a bound `tile` re-runs the pipeline over everything open
    on demand. (The single-display VM cannot exercise cross-monitor balancing; that is
    covered by the pure `pickDistributeTarget` / `TileResolve` reducer tests.)

    The Slot vocabulary, rectForSlot arithmetic, the PositionWindow Effect, and the
    tile round-trip are all reducer-tested (winspace_test.cpp). These seams prove only
    what a unit test cannot: that winspace's single geometry-writing adapter
    (positionWindow in src/win32.cpp) genuinely resolves the monitor work area,
    compensates the visible frame, and drives SetWindowPos so a foreign window's
    VISIBLE edges land flush to the computed Slot on a real session.

    Matching is by TITLE, not exe: Start-TestWindow's host process is powershell.exe
    (shared with the runner console and helpers), so an `exe:` rule is ambiguous; the
    test window's distinct -Title is an unambiguous Oracle key. The parser compiles a
    title rule as a std::regex, so the literal title matches.

    Oracle policy (ADR-0005): assert on independent OS state — the window's VISIBLE
    frame (DWMWA_EXTENDED_FRAME_BOUNDS via Get-FrameBounds) against the Slot rect this
    harness computes from the same monitor work area (Get-WorkArea), mirroring
    rectForSlot — never on winspace's own log. The tolerance absorbs DWM rounding /
    the Win11 rounded-corner allowance on the frame inset.

    The rule targets the CURRENT workspace (N = 1 on a single desktop), so the placed
    window stays visible on the active desktop where its rect is readable — the seams
    prove geometry, not the cross-desktop pin (that is WindowRules.Tests.ps1's job).
#>

# Mirror of winspace::rectForSlot (src/winspace.cpp) in PowerShell — the same
# integer midpoint arithmetic, so the harness computes the identical expected rect
# the adapter targets. Only the Slots the seams below use are implemented.
function Get-ExpectedSlotRect {
    param([Parameter(Mandatory)]$WorkArea, [Parameter(Mandatory)][string]$Slot)
    $midX = $WorkArea.left + [int][math]::Floor(($WorkArea.right - $WorkArea.left) / 2)
    $midY = $WorkArea.top + [int][math]::Floor(($WorkArea.bottom - $WorkArea.top) / 2)
    switch ($Slot) {
        'left-half'   { return [pscustomobject]@{ left = $WorkArea.left; top = $WorkArea.top; right = $midX; bottom = $WorkArea.bottom } }
        'right-half'  { return [pscustomobject]@{ left = $midX; top = $WorkArea.top; right = $WorkArea.right; bottom = $WorkArea.bottom } }
        'top-half'    { return [pscustomobject]@{ left = $WorkArea.left; top = $WorkArea.top; right = $WorkArea.right; bottom = $midY } }
        'bottom-half' { return [pscustomobject]@{ left = $WorkArea.left; top = $midY; right = $WorkArea.right; bottom = $WorkArea.bottom } }
        'maximized'   { return [pscustomobject]@{ left = $WorkArea.left; top = $WorkArea.top; right = $WorkArea.right; bottom = $WorkArea.bottom } }
        default { throw "Get-ExpectedSlotRect: unhandled slot '$Slot'" }
    }
}

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # A distinct title so the rule (and each seam's Oracle) keys on exactly this
    # window, never the runner console or a helper powershell.
    $script:SlotTitle = 'winspace-slot-pin'
    $script:Slot = 'right-half'
    # A slot-bearing Place rule targeting the CURRENT workspace (1) so the placed
    # window stays visible; plus a bound tile and quit for the sweep and teardown.
    # A double-quoted here-string, so `$mod is escaped to survive to the file while
    # $SlotTitle expands here.
    $script:SlotConfig = @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
bind = `$mod SHIFT, T, tile
windowrule = workspace 1 slot $script:Slot, title:$script:SlotTitle
"@

    # The frame-inset tolerance: the visible-frame compensation is integer-exact at
    # 100% DPI, so a handful of pixels covers DWM rounding and the Win11 rounded-corner
    # allowance (mirrors the fills tolerance in Test-RectNear).
    $script:SlotTolerance = 6
}

AfterAll {
    Clear-WinspaceConfig   # restore built-in-default behaviour for later seams
}

Describe 'slot-placement' {

    # place-on-appear-with-slot: a rule-matched app launched while winspace is up lands
    # in its Slot on open. Its SHOW reaches winspace through the live SetWinEventHook;
    # the Appeared's Slot-bearing Place rule emits PositionWindow, and the adapter lands
    # the VISIBLE frame flush to the computed Slot rect on the window's monitor.
    It 'place-on-appear-with-slot: a matched app lands in its Slot on open' -Tag 'slot-placement' {
        $winspace = $null
        $window = $null
        try {
            Set-DesktopCount 1   # single desktop: the rule's workspace 1 is the current one
            Set-WinspaceConfig -Content $script:SlotConfig | Out-Null
            $winspace = Start-Winspace

            $window = Start-TestWindow -Title $script:SlotTitle -X 200 -Y 160 -Width 480 -Height 320

            # The monitor work area the window opened on — the same one the adapter
            # resolves (MONITOR_DEFAULTTONEAREST). The Slot rect is computed from it.
            $work = Get-WorkArea -Hwnd $window.Hwnd
            $expected = Get-ExpectedSlotRect -WorkArea $work -Slot $script:Slot

            Wait-Until -Because 'the matched window to land in its Slot' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $window.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            $frame = Get-FrameBounds -Hwnd $window.Hwnd
            (Test-RectNear -Actual $frame -Expected $expected -Tolerance $script:SlotTolerance) | Should -BeTrue `
                -Because "the placed window's visible frame $(Format-Rect $frame) must match the Slot rect $(Format-Rect $expected)"
        } catch {
            Save-FailureScreenshot -Name 'slot-place-on-appear'
            throw
        } finally {
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }

    # tile-command: after the user has dragged a placed window off its Slot, pressing
    # the bound `tile` sweeps every open window and returns each matched one to its
    # Slot. Proves the two-phase Tile -> ResolveTile -> TileResolve round-trip drives a
    # live PositionWindow on demand.
    It 'tile-command: the on-demand sweep returns a moved window to its Slot' -Tag 'slot-placement' {
        $winspace = $null
        $window = $null
        try {
            Set-DesktopCount 1
            Set-WinspaceConfig -Content $script:SlotConfig | Out-Null
            $winspace = Start-Winspace

            $window = Start-TestWindow -Title $script:SlotTitle -X 200 -Y 160 -Width 480 -Height 320
            $work = Get-WorkArea -Hwnd $window.Hwnd
            $expected = Get-ExpectedSlotRect -WorkArea $work -Slot $script:Slot

            # First the place-once put it in the Slot.
            Wait-Until -Because 'the initial place-once to land the window in its Slot' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $window.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            # The user drags it well away from the Slot (winspace does NOT snap it back
            # — place-once — which is exactly why an explicit re-tile is needed).
            Move-Window -Hwnd $window.Hwnd -X ($work.left + 40) -Y ($work.top + 40) -Width 360 -Height 240
            Wait-Until -Because 'the window to actually leave its Slot before the re-tile' -Condition {
                -not (Test-RectNear -Actual (Get-FrameBounds -Hwnd $window.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance)
            }

            # Press the bound tile: the sweep re-places every matched live window.
            Send-Chord 'Alt+Shift+T'   # $mod SHIFT, T -> tile

            Wait-Until -Because 'the tile sweep to return the window to its Slot' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $window.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            $frame = Get-FrameBounds -Hwnd $window.Hwnd
            (Test-RectNear -Actual $frame -Expected $expected -Tolerance $script:SlotTolerance) | Should -BeTrue `
                -Because "tile must return the window's visible frame $(Format-Rect $frame) to the Slot rect $(Format-Rect $expected)"
        } catch {
            Save-FailureScreenshot -Name 'slot-tile-command'
            throw
        } finally {
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }

    # drag-away-not-yanked: a placed window the user moves is NEVER re-placed by a later
    # lifecycle edge — proves place-once has no continuous enforcement (the
    # EVENT_OBJECT_LOCATIONCHANGE follow-hook is not adopted). The barrier that proves
    # winspace kept processing the stream (rather than merely being idle) is a SECOND
    # matched window that opens and IS placed: once it lands in the Slot, the first —
    # moved — window must still be where the user left it.
    It 'drag-away-not-yanked: a moved placed window is not re-placed' -Tag 'slot-placement' {
        $winspace = $null
        $first = $null
        $second = $null
        try {
            Set-DesktopCount 1
            Set-WinspaceConfig -Content $script:SlotConfig | Out-Null
            $winspace = Start-Winspace

            $first = Start-TestWindow -Title $script:SlotTitle -X 200 -Y 160 -Width 480 -Height 320
            $work = Get-WorkArea -Hwnd $first.Hwnd
            $expected = Get-ExpectedSlotRect -WorkArea $work -Slot $script:Slot

            Wait-Until -Because 'the first window to be placed in its Slot' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $first.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            # The user drags the placed window to a distinct spot.
            $movedX = $work.left + 40
            $movedY = $work.top + 40
            Move-Window -Hwnd $first.Hwnd -X $movedX -Y $movedY -Width 360 -Height 240
            Wait-Until -Because 'the first window to leave its Slot' -Condition {
                -not (Test-RectNear -Actual (Get-FrameBounds -Hwnd $first.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance)
            }
            $movedFrame = Get-FrameBounds -Hwnd $first.Hwnd

            # Barrier: a second matched window opens and is placed. Its placement proves
            # winspace processed subsequent Appeared events — so if place-once were
            # instead continuous enforcement, the first window would have snapped back.
            $second = Start-TestWindow -Title $script:SlotTitle -X 260 -Y 220 -Width 480 -Height 320
            Wait-Until -Because 'the second matched window to be placed in its Slot' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $second.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            # The first window is still where the user left it — never yanked back.
            $nowFrame = Get-FrameBounds -Hwnd $first.Hwnd
            (Test-RectNear -Actual $nowFrame -Expected $movedFrame -Tolerance $script:SlotTolerance) | Should -BeTrue `
                -Because "the moved window $(Format-Rect $nowFrame) must stay where the user left it $(Format-Rect $movedFrame)"
            (Test-RectNear -Actual $nowFrame -Expected $expected -Tolerance $script:SlotTolerance) | Should -BeFalse `
                -Because 'place-once must never re-place a window the user moved away'
        } catch {
            Save-FailureScreenshot -Name 'slot-drag-away-not-yanked'
            throw
        } finally {
            Stop-TestWindow $first
            Stop-TestWindow $second
            Stop-Winspace -Process $winspace
        }
    }

    # distribute-on-appear: a window matching NO rule is auto-placed by Distribute
    # (ADR-0020) — moved to the least-occupied Display and MAXIMIZED. On a single
    # desktop that is the window's own Display, so the Oracle is the maximized frame
    # (≈ the monitor work area). This is the reversal of the retired ADR-0016
    # tile-allowlist: managed is now the default, not an opt-in.
    It 'distribute-on-appear: an unmatched window is maximized on its display' -Tag 'slot-placement' {
        $winspace = $null
        $unmatched = $null
        try {
            Set-DesktopCount 1
            Set-WinspaceConfig -Content $script:SlotConfig | Out-Null
            $winspace = Start-Winspace

            # A window whose title does NOT match the slot rule — Distribute manages it.
            $unmatched = Start-TestWindow -Title 'winspace-slot-unmatched' -X 120 -Y 120 -Width 400 -Height 300

            # Distribute maximizes it on its own (only) Display; the Oracle is the
            # maximized rect — the full work area, computed the same way the adapter does.
            $work = Get-WorkArea -Hwnd $unmatched.Hwnd
            $expected = Get-ExpectedSlotRect -WorkArea $work -Slot 'maximized'

            Wait-Until -Because 'the unmatched window to be Distributed (maximized) on its display' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $unmatched.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            $frame = Get-FrameBounds -Hwnd $unmatched.Hwnd
            (Test-RectNear -Actual $frame -Expected $expected -Tolerance $script:SlotTolerance) | Should -BeTrue `
                -Because "a Distributed window's visible frame $(Format-Rect $frame) must fill its display's work area $(Format-Rect $expected)"
        } catch {
            Save-FailureScreenshot -Name 'slot-distribute-on-appear'
            throw
        } finally {
            Stop-TestWindow $unmatched
            Stop-Winspace -Process $winspace
        }
    }

    # ignore-untouched: an Ignore-matched window is left ENTIRELY alone — never placed by
    # Distribute (ADR-0020 widened Ignore to "don't touch at all"), even though every
    # unmatched window IS auto-maximized. The barrier is an unmatched sibling that gets
    # Distributed (maximized): once it lands, the ignored window must still be exactly
    # where it opened.
    It 'ignore-untouched: an Ignore-matched window is neither placed nor resized' -Tag 'slot-placement' {
        $winspace = $null
        $ignored = $null
        $unmatched = $null
        try {
            Set-DesktopCount 1
            $ignoreTitle = 'winspace-slot-ignored'
            # A double-quoted here-string: `$mod is escaped to survive to the file while
            # $ignoreTitle expands here.
            $cfg = @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
windowrule = ignore, title:$ignoreTitle
"@
            Set-WinspaceConfig -Content $cfg | Out-Null
            $winspace = Start-Winspace

            # The ignored window opens at a distinct spot; winspace must not touch it.
            $ignored = Start-TestWindow -Title $ignoreTitle -X 120 -Y 120 -Width 400 -Height 300
            $before = Get-FrameBounds -Hwnd $ignored.Hwnd

            # Barrier: an unmatched sibling opens and IS Distributed (maximized), proving
            # winspace processed the Appeared stream past the ignored window's own Appeared.
            $unmatched = Start-TestWindow -Title 'winspace-slot-unmatched' -X 200 -Y 160 -Width 480 -Height 320
            $work = Get-WorkArea -Hwnd $unmatched.Hwnd
            $expected = Get-ExpectedSlotRect -WorkArea $work -Slot 'maximized'
            Wait-Until -Because 'the unmatched sibling to be Distributed (maximized)' -Condition {
                Test-RectNear -Actual (Get-FrameBounds -Hwnd $unmatched.Hwnd) -Expected $expected -Tolerance $script:SlotTolerance
            }

            # The ignored window never moved — its frame is byte-for-byte unchanged.
            $after = Get-FrameBounds -Hwnd $ignored.Hwnd
            (Test-RectEqual -A $after -B $before) | Should -BeTrue `
                -Because "an Ignore-matched window $(Format-Rect $after) must be left exactly where it opened $(Format-Rect $before)"
        } catch {
            Save-FailureScreenshot -Name 'slot-ignore-untouched'
            throw
        } finally {
            Stop-TestWindow $ignored
            Stop-TestWindow $unmatched
            Stop-Winspace -Process $winspace
        }
    }
}
