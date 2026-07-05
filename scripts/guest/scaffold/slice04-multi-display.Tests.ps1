<#
    SCAFFOLD — issue 04 (multi-display fill order + float overflow). Unbuilt: `It -Skip`
    placeholders, surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1 for
    the scaffold contract.

    Future Oracle probe: Get-WindowRects (GetWindowRect over EnumWindows-by-PID) mapped to
    monitors (EnumDisplayMonitors) — assert the fill order (empty display first, then BSP,
    then float on min-floor overflow) against OS geometry.

    MULTI-DISPLAY STAGING (wired here, NOT activated). This is the only scaffold that needs
    more than one monitor, so the staging lever is recorded here for the future author:
    the HOST adds a virtual display before the guest run via

        vmctl MKS.SetNumDisplays winspace-e2e 2      # host-side; not called by this file

    then reverts to 1 afterwards. It is deliberately inert until the seam is built — this
    file makes NO vmctl call and the seams below stay -Skip, so the default and -Fresh runs
    add no second monitor.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-04 (scaffold)' {
    It 'fill order: an empty display is filled before the focused display is BSP-split' -Skip -Tag 'scaffold', 'multi-display' {
        # Requires MKS.SetNumDisplays 2 (host-staged). Oracle: the Nth window lands on the
        # empty monitor's work area; only once all are occupied does a split occur.
    }
    It 'overflow: a would-be sub-floor tile opens floating instead of tiling' -Skip -Tag 'scaffold', 'multi-display' {
        # Oracle: the overflow window's rect is not part of the tiled partition (floated).
    }
}
