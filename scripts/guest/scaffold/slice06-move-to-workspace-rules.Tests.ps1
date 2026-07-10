<#
    SCAFFOLD — the windowrule half of the move-to-workspace story (issue 07: rules on the
    reintroduced hook). Unbuilt: `It -Skip` placeholder, surfaced as SKIPPED. See
    scaffold/slice03-bsp-one-display.Tests.ps1 for the scaffold contract.

    The `movetoworkspace` / `movetoworkspacesilent` half (issue 06) is now LIVE — see
    guest/MoveToWorkspace.Tests.ps1, which drives the real dispatcher and asserts on the
    window's desktop GUID (Get-WindowDesktopId, the Oracle noted below).

    Future Oracle probe: IVirtualDesktopManager::GetWindowDesktopId per HWND (which desktop
    GUID a window lives on) checked against Get-VdState's GUID list — independent VD-membership
    truth. The no-flash guarantee (DWMWA_CLOAK around the move) has no automatable observable
    and stays a manual smoke step.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-06 (scaffold)' {
    It 'windowrule: a create-time rule places a matching app on its target workspace' -Skip -Tag 'scaffold', 'move-to-workspace' {
        # Oracle: the launched app's HWND desktop GUID == the rule's target workspace GUID.
    }
}
