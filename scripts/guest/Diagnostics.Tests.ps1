<#
    Diagnostics file-sink Smoke seam (VM harness, ADR-0005) — the live half of
    ADR-0025's second sink.

    ADR-0004's degrade-and-log strategy is load-bearing throughout winspace, and in
    every SHIPPED configuration it had nowhere to log to: the Logon task starts a
    /SUBSYSTEM:WINDOWS binary and discards its stderr, so "degrade and log" was really
    degrade SILENTLY, and a fault that happened at login could not be diagnosed at all.

    This seam is what makes the observability change itself observable. It is
    deliberately the ONE place where winspace's own log IS the Oracle — because the
    log's existence is the feature. Note it does NOT read run.log (the harness's own
    stderr redirect): it reads the file winspace writes on its own, which is precisely
    what a user who redirected nothing would have.

    The degraded operation is produced by the existing forced-variant hook, the same
    genuine loud path the error-handling seam drives.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession
}

Describe 'diagnostics-file-sink' {

    It 'log-file: winspace writes its diagnostics to a size-capped file in local application data' -Tag 'log-file' {
        $winspace = $null
        try {
            $logPath = Get-WinspaceFileLog
            # Start from nothing, so the assertion cannot pass on a file some earlier
            # seam left behind.
            Remove-Item $logPath -Force -ErrorAction SilentlyContinue
            Remove-Item "$logPath.1" -Force -ErrorAction SilentlyContinue

            # A forced stub variant is a genuine degraded operation: the bridge refuses
            # to call through an uncaptured vtable and says so, loudly, exactly once.
            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_VARIANT = '23h2-kb' } `
                                       -ReadyPattern 'NOT YET IMPLEMENTED'

            Wait-Until -Because 'winspace to create its diagnostics file' -Condition {
                Test-Path $logPath
            }
            Test-Path $logPath | Should -BeTrue `
                -Because 'a fault that happens at login must be diagnosable afterwards'

            $text = Get-WinspaceFileLogText
            $text | Should -Match 'NOT YET IMPLEMENTED' `
                -Because 'the file must carry the same diagnostic the terminal would have shown'
            $text | Should -Match '^\[(INFO|WARN|ERROR)\]' `
                -Because 'the file uses the same leveled grammar as stderr'

            # No ANSI colouring: escape codes are a terminal affordance and make the
            # file harder to read. 0x1B is ESC.
            $text.Contains([char]0x1B) | Should -BeFalse `
                -Because 'ANSI level colouring must not reach the file'

            # Every line that reached stderr also reached the file, at the same level
            # and with the same text. Compare the level+message pairs, ignoring the
            # colour codes stderr adds.
            $stderrLines = (Get-WinspaceLogText) -split "`r?`n" |
                Where-Object { $_ } |
                ForEach-Object { ($_ -replace "$([char]0x1B)\[[0-9;]*m", '').Trim() }
            $fileLines = $text -split "`r?`n" | Where-Object { $_ } | ForEach-Object { $_.Trim() }
            foreach ($line in $stderrLines) {
                $fileLines | Should -Contain $line `
                    -Because 'the two sinks sit behind ONE choke point, so neither can drift'
            }

            # Still windowless — a second sink must not put a console on screen.
            # No @() wrapper: Get-WinspaceWindows already comma-protects its array, so
            # re-wrapping would nest it and count 1 for every possible input.
            $windows = Get-WinspaceWindows -ProcessId $winspace.Id
            $windows.Count | Should -Be 0
        } catch {
            Save-FailureScreenshot -Name 'diagnostics-log-file'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
