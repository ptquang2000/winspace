<#
    SCAFFOLD — issue 05 (spatial directional focus + move). Unbuilt: `It -Skip`
    placeholders, surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1 for
    the scaffold contract.

    Future Oracle probe: GetForegroundWindow (focus) and Get-WindowRects deltas (move) —
    directional resolution asserted on OS focus + geometry across monitor boundaries.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-05 (scaffold)' {
    It 'focus: "focus right" moves foreground to the nearest window to the right' -Skip -Tag 'scaffold', 'spatial' {
        # Oracle: GetForegroundWindow == the expected neighbor HWND after the dispatcher.
    }
    It 'move: "movewindow up" swaps the focused tile with its upper neighbor and reflows' -Skip -Tag 'scaffold', 'spatial' {
        # Oracle: the two windows' rects exchange; the rest of the partition is preserved.
    }
}
