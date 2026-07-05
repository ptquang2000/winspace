<#
    SCAFFOLD — issue 10 (autostart via Task Scheduler logon task). Unbuilt: `It -Skip`
    placeholders, surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1 for
    the scaffold contract.

    Future Oracle probe: Get-ScheduledTask — the Windows-blessed autostart record itself is
    the independent OS state (a logon task registered when start_at_login is on, removed when
    off). No hotkey/VD Trigger; the Oracle is the task store.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-10 (scaffold)' {
    It 'start_at_login on: a logon-triggered scheduled task is registered' -Skip -Tag 'scaffold', 'autostart' {
        # Oracle: Get-ScheduledTask shows a winspace logon task (restart-on-failure set).
    }
    It 'start_at_login off: the scheduled task is removed' -Skip -Tag 'scaffold', 'autostart' {
        # Oracle: Get-ScheduledTask no longer lists the winspace logon task.
    }
}
