<#
    SCAFFOLD — issue 09 (full config grammar + reload). Unbuilt: `It -Skip` placeholders,
    surfaced as SKIPPED. See scaffold/slice03-bsp-one-display.Tests.ps1 for the scaffold
    contract.

    Future Oracle probe: after writing a new config and firing `reload`, a Send-Chord of a
    newly-bound hotkey produces its Get-VdState effect (re-registration proven by behavior);
    an invalid config leaves winspace alive (Get-Process) with a diagnostic in run.log
    (Read-WinspaceLog) — last-good config retained, no crash.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\WinspaceTest.psm1') -Force
}

Describe 'slice-09 (scaffold)' {
    It 'reload: re-parses config and re-registers hotkeys live' -Skip -Tag 'scaffold', 'config-reload' {
        # Oracle: a hotkey bound only in the new config fires (Get-VdState switch) after reload.
    }
    It 'reload: invalid config keeps the last-good config and logs diagnostics (no crash)' -Skip -Tag 'scaffold', 'config-reload' {
        # Oracle: process alive (Get-Process) + a config diagnostic line in run.log.
    }
}
