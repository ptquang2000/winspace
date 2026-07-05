<#
    SCAFFOLD — issue 03 (BSP tiling on one display: split + reclaim). Unbuilt: `It -Skip`
    placeholders, surfaced as SKIPPED. See scaffold/slice02-window-tracking.Tests.ps1 for
    the scaffold contract.

    Future Oracle probe: Get-WindowRects (GetWindowRect over EnumWindows-by-PID) checked
    for a non-overlapping partition of the work area — the BSP tiling invariant asserted
    on OS geometry, not on winspace's internal tree.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-03 (scaffold)' {
    It 'split: each new window bisects the focused tile, alternating orientation' -Skip -Tag 'scaffold', 'bsp' {
        # Oracle: N windows → N rects that tile the work area with no overlap and no gap.
    }
    It 'reclaim: destroying a window reflows the subtree to refill its space' -Skip -Tag 'scaffold', 'bsp' {
        # Oracle: after a close, the surviving rects again partition the work area exactly.
    }
}
