<#
    SCAFFOLD — issue 06 (move-to-workspace + place-once rules with cloak-move). Unbuilt:
    `It -Skip` placeholders, surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1
    for the scaffold contract.

    Future Oracle probe: IVirtualDesktopManager::GetWindowDesktopId per HWND (which desktop
    GUID a window lives on) checked against Get-VdState's GUID list — independent VD-membership
    truth. The no-flash guarantee (DWMWA_CLOAK around the move) has no automatable observable
    and stays a manual smoke step.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-06 (scaffold)' {
    It 'movetoworkspace N: the focused window lands on desktop N (by GUID)' -Skip -Tag 'scaffold', 'move-to-workspace' {
        # Oracle: GetWindowDesktopId(hwnd) == Get-VdState.Guids[N-1] after the dispatcher.
    }
    It 'windowrule: a create-time rule places a matching app on its target workspace' -Skip -Tag 'scaffold', 'move-to-workspace' {
        # Oracle: the launched app's HWND desktop GUID == the rule's target workspace GUID.
    }
}
