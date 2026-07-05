<#
    SCAFFOLD — issue 08 (launcher: exec / exec-once with PID-match placement). Unbuilt:
    `It -Skip` placeholders, surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1
    for the scaffold contract.

    Future Oracle probe: Get-Process (the CreateProcess'd child) + GetWindowDesktopId of its
    first PID-owned top-level window — placement proven by PID match on OS state, and
    exec-once idempotence proven by a single process instance across a reload.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-08 (scaffold)' {
    It 'exec: a launched app is placed on its target workspace by PID match' -Skip -Tag 'scaffold', 'launcher' {
        # Oracle: GetWindowDesktopId(first PID-owned HWND) == the entry's target workspace GUID.
    }
    It 'exec-once: a reload does not relaunch an already-running app' -Skip -Tag 'scaffold', 'launcher' {
        # Oracle: Get-Process count for the exec-once target is unchanged across reload.
    }
}
