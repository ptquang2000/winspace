<#
    Installation Smoke seam (VM harness, ADR-0005) — the live-only packaging
    behaviour the pure command-parser seam structurally cannot reach (ADR-0019):
    single-instance enforcement, and the two headless `install` / `uninstall`
    commands the Scoop lifecycle hooks invoke around the file swap.

    The pure half — argv -> {Run|Install|Uninstall|Other} — is unit-tested in
    winspace_test.cpp ([command]). This seam proves only what that cannot: that the
    running binary actually enforces single-instance, that the headless commands
    genuinely drive the OS (register/remove the Logon task, quit a live
    Orchestrator, release the exe's file lock), and that the hybrid
    message-if-running / else-direct routing works both ways.

    Oracle policy (ADR-0005): assert on independent OS state — the Task Scheduler
    store via Get-ScheduledTask (the autostart record itself), process liveness
    (Get-Process / HasExited), and file deletability (the exe can be renamed iff no
    process still locks its image) — never winspace's own log. The snapshot is just
    winspace-e2e (stock); no Scoop is baked in (ADR-0017 / spec seam 3 — the Scoop
    lifecycle is verified locally, not here).

    Each seam stages its own task-store and process preconditions so run order never
    matters, mirroring the Autostart seam.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any live launch reaches the desktop

    # start_at_login ON / OFF. Minimal configs — these seams exercise the autostart
    # commands and single-instance, not hotkeys, so no binds are needed. A here-string
    # keeps `$mod escaped verbatim into the file.
    $script:ConfigOn = @"
`$mod = ALT
start_at_login = true
"@
    $script:ConfigOff = @"
`$mod = ALT
start_at_login = false
"@

    # Run a headless winspace subcommand (install / uninstall) to completion and
    # return its process exit code. These are /SUBSYSTEM:WINDOWS, so -Wait (not `&`)
    # is what actually blocks until the command has done its work and exited.
    function script:Invoke-WinspaceCmd {
        param([Parameter(Mandatory)][ValidateSet('install', 'uninstall')][string]$Verb)
        $p = Start-Process -FilePath (Get-WinspaceExe) -ArgumentList $Verb -Wait -PassThru
        return $p.ExitCode
    }

    # The number of LIVE winspace WM processes running from the e2e exe — the
    # single-instance Oracle. Headless install/uninstall helpers are short-lived and
    # -Wait'd out before any assertion, so this counts only persistent Orchestrators.
    function script:Get-RunningWinspaceCount {
        return @(Get-Process -Name 'winspace' -ErrorAction SilentlyContinue |
                Where-Object { $_.Path -eq (Get-WinspaceExe) }).Count
    }

    # File-lock release Oracle: the exe is deletable iff no process still locks its
    # image. A rename in place fails atomically while locked and is restored
    # immediately on success, so a green run leaves the deploy exe untouched.
    function script:Test-ExeUnlocked {
        $exe = Get-WinspaceExe
        $leaf = Split-Path $exe -Leaf
        $lockedName = "$leaf.locktest"
        try {
            Rename-Item -Path $exe -NewName $lockedName -ErrorAction Stop
            Rename-Item -Path (Join-Path (Split-Path $exe -Parent) $lockedName) -NewName $leaf -ErrorAction Stop
            return $true
        } catch {
            return $false
        }
    }
}

AfterAll {
    Remove-WinspaceAutostartTask   # never leak a registered task into a later seam
    Clear-WinspaceConfig           # restore built-in-default behaviour for later seams
}

Describe 'installation' {

    # Single-instance. The WM owns exclusive OS resources (hotkeys + the VD bridge),
    # so a second bare `winspace` must detect the live Orchestrator and exit rather
    # than raise a competing WM. Oracle: process count stays 1 and the second launch
    # exits promptly. NB: the second launch is NOT Start-Winspace — it never logs
    # 'adopted' (it exits before adoption), so it is launched directly and waited on.
    It 'a second run launch exits without raising a second WM' -Tag 'installation' {
        $first = $null; $second = $null
        try {
            Set-WinspaceConfig -Content $script:ConfigOff | Out-Null
            $first = Start-Winspace
            Get-RunningWinspaceCount | Should -Be 1 -Because 'the first launch is the sole Orchestrator'

            $second = Start-Process -FilePath (Get-WinspaceExe) -PassThru
            Wait-Until -Because 'the second (contending) launch to exit on the single-instance guard' -Condition {
                $second.HasExited
            }
            $second.ExitCode | Should -Be 0 -Because 'starting winspace twice is a successful no-op, not an error'
            $first.HasExited | Should -BeFalse -Because 'the original Orchestrator must survive the second launch'
            Get-RunningWinspaceCount | Should -Be 1 -Because 'the second launch must not add a competing WM'
        } catch {
            Save-FailureScreenshot -Name 'installation-single-instance'
            throw
        } finally {
            Stop-Winspace -Process $second
            Stop-Winspace -Process $first
        }
    }

    # `winspace install`, no Orchestrator, flag ON. The headless one-shot sync reads
    # the config directly and registers the per-user \winspace logon task. Oracle: the
    # task exists after the command; the command exits 0; no WM is left running.
    It 'install with no orchestrator and start_at_login on registers the logon task' -Tag 'installation' {
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null

            $code = Invoke-WinspaceCmd -Verb 'install'
            $code | Should -Be 0 -Because 'a successful direct autostart sync exits 0'

            Get-WinspaceAutostartTask | Should -Not -BeNullOrEmpty `
                -Because 'install must register the task when start_at_login = true'
            Get-RunningWinspaceCount | Should -Be 0 -Because 'install is headless — it never launches the WM'
        } catch {
            Save-FailureScreenshot -Name 'installation-install-on'
            throw
        } finally {
            Remove-WinspaceAutostartTask
        }
    }

    # `winspace install`, no Orchestrator, flag OFF — the never-enable-unbidden case.
    # With the task absent and the flag off, install must leave autostart untouched:
    # a fresh Scoop install does not start seizing logon hooks (user story 9).
    It 'install with no orchestrator and start_at_login off never enables autostart' -Tag 'installation' {
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOff | Out-Null

            $code = Invoke-WinspaceCmd -Verb 'install'
            $code | Should -Be 0 -Because 'a no-op sync (nothing to do) still exits 0'

            Get-WinspaceAutostartTask | Should -BeNullOrEmpty `
                -Because 'install must NEVER enable autostart on its own — the flag is the single source of truth'
        } catch {
            Save-FailureScreenshot -Name 'installation-install-off'
            throw
        }
    }

    # `winspace install` WITH an Orchestrator running — the hybrid message path. The
    # Orchestrator boots with the flag on (registering the task on Started), the task
    # is then removed out-of-band, and `winspace install` fires: it must route a
    # sync-autostart Control message to the LIVE Orchestrator, which re-registers the
    # task from its own live flag. Oracle: the task reappears, no second WM is spawned,
    # and the Orchestrator stays alive (install never quits it).
    It 'install with an orchestrator running drives the live instance to re-sync (no second WM)' -Tag 'installation' {
        $winspace = $null
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            $winspace = Start-Winspace
            Wait-Until -Because 'the orchestrator to register the task on start' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }

            # Delete it out-of-band, so a re-sync has visible work to do.
            Remove-WinspaceAutostartTask
            Get-WinspaceAutostartTask | Should -BeNullOrEmpty -Because 'precondition: the task is gone before install'

            $code = Invoke-WinspaceCmd -Verb 'install'
            $code | Should -Be 0 -Because 'messaging a live orchestrator succeeds'

            Wait-Until -Because 'the live orchestrator to re-register the task via the control channel' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            $winspace.HasExited | Should -BeFalse -Because 'install must not stop the running WM'
            Get-RunningWinspaceCount | Should -Be 1 -Because 'the sync rode the existing WM — no second instance'
        } catch {
            Save-FailureScreenshot -Name 'installation-install-running'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # `winspace uninstall` WITH an Orchestrator running — the file-lock release path,
    # and the `quit` Control message via the clean shutdown path. The command must
    # remove the task AND stop the live Orchestrator so its exe unlocks. Oracle: the
    # task is gone, the process exits, and the exe becomes renamable (the lock is
    # released iff no process holds the image).
    It 'uninstall with an orchestrator running removes the task and stops the WM (exe unlocks)' -Tag 'installation' {
        $winspace = $null
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            $winspace = Start-Winspace
            Wait-Until -Because 'the orchestrator to register the task on start' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            $null = $winspace.Handle   # settle HasExited/ExitCode queries

            $code = Invoke-WinspaceCmd -Verb 'uninstall'
            $code | Should -Be 0 -Because 'uninstall messaging a live orchestrator succeeds'

            Wait-Until -Because 'the orchestrator to exit via the clean quit path' -Condition {
                $winspace.HasExited
            }
            Get-WinspaceAutostartTask | Should -BeNullOrEmpty `
                -Because 'uninstall removes the \winspace\<user> logon task unconditionally'
            Get-RunningWinspaceCount | Should -Be 0 -Because 'uninstall stops the running Orchestrator'
            Test-ExeUnlocked | Should -BeTrue `
                -Because 'the stopped Orchestrator released its image lock — the exe is now deletable'
        } catch {
            Save-FailureScreenshot -Name 'installation-uninstall-running'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # `winspace uninstall` with NO Orchestrator — the direct path. With a task present
    # (seeded by an on-launch) and nothing running, uninstall removes the task
    # directly; a second uninstall with the task already gone is a clean no-op
    # (ERROR_FILE_NOT_FOUND counted as success).
    It 'uninstall with no orchestrator removes the task directly, then is a clean no-op' -Tag 'installation' {
        $seed = $null
        try {
            # Seed a genuinely-registered task via an on-launch, then stop the WM so
            # the uninstall runs with nothing live.
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            $seed = Start-Winspace
            Wait-Until -Because 'the seed launch to register the task' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            Stop-Winspace -Process $seed; $seed = $null
            Wait-Until -Because 'the seed WM to fully exit before the direct uninstall' -Condition {
                (Get-RunningWinspaceCount) -eq 0
            }

            (Invoke-WinspaceCmd -Verb 'uninstall') | Should -Be 0 -Because 'a direct task removal succeeds'
            Get-WinspaceAutostartTask | Should -BeNullOrEmpty -Because 'the direct path removed the task'

            # Repeat with the task already absent — still a clean success.
            (Invoke-WinspaceCmd -Verb 'uninstall') | Should -Be 0 `
                -Because 'removing an already-absent task counts ERROR_FILE_NOT_FOUND as success'
        } catch {
            Save-FailureScreenshot -Name 'installation-uninstall-direct'
            throw
        } finally {
            Stop-Winspace -Process $seed
        }
    }
}
