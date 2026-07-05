<#
    SCAFFOLD — issue 07 (place-once behaviors: togglefloat / drag-pops-to-float /
    fullscreen). Unbuilt: `It -Skip` placeholders, surfaced as SKIPPED. See
    scaffold/slice02-window-tracking.Tests.ps1 for the scaffold contract.

    Future Oracle probe: Get-WindowRects deltas across a reflow — a floated window's rect
    is proven untouched while the tiled set reflows around it (place-once, not enforce).
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-07 (scaffold)' {
    It 'togglefloat: converts the focused window between tiled and floating' -Skip -Tag 'scaffold', 'place-once' {
        # Oracle: after togglefloat, the window drops out of / rejoins the tiled partition.
    }
    It 'fullscreen: a borderless/fullscreen window is auto-floated and excluded from reflow' -Skip -Tag 'scaffold', 'place-once' {
        # Oracle: the fullscreen rect == the monitor bounds and is unchanged by later reflows.
    }
}
