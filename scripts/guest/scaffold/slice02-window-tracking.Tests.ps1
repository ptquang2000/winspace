<#
    SCAFFOLD — issue 02 (window tracking + eligibility + fill-one-window).
    Unbuilt: these are `It -Skip` placeholders so a real seam is a one-file drop-in
    (delete -Skip, add the Trigger + Oracle). They surface as SKIPPED in the JUnit XML,
    never as failures. Shared tag 'scaffold' lets -Fresh collect them in one no-VM pass.

    Future Oracle probe: Get-WindowRects (GetWindowRect over EnumWindows-by-PID) vs the
    focused monitor work area (SystemParametersInfo SPI_GETWORKAREA / EnumDisplayMonitors)
    — independent OS geometry, not winspace's own report.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-02 (scaffold)' {
    It 'fill-one: a single eligible window fills the focused monitor work area' -Skip -Tag 'scaffold', 'window-tracking' {
        # Oracle: Get-WindowRects == work-area rect (SPI_GETWORKAREA), within snap tolerance.
    }
    It 'eligibility: an ineligible (tool/owned) window is tracked floating and left untouched' -Skip -Tag 'scaffold', 'window-tracking' {
        # Oracle: the floating window's rect is unchanged after a tileable window appears.
    }
}
