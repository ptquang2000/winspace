<#
    Autostart Smoke seam (VM harness, ADR-0005) — the live-only behaviour the pure
    Reducer seam structurally cannot reach (issue 10 / ADR-0013): the running,
    unelevated winspace registers/removes a per-user Task Scheduler LOGON task at
    \winspace\<username> to match the start_at_login flag, both at start and live on
    reload, entirely from the STA Worker via COM ITaskService.

    The pure half — Started{} / Reloaded{} each emit exactly one
    SyncAutostart{startAtLogin} and change no State — is unit-tested in
    reducer_test.cpp. This seam proves only what that cannot: that the Worker's
    SyncAutostart executor genuinely drives ITaskService, so the OS task store gains
    the task with the ADR-0013 shape when the flag is on, loses it when off, never
    duplicates on a repeated enable, and toggles live on a `reload` hotkey with no
    restart.

    Oracle policy (ADR-0005): assert on independent OS state — the Task Scheduler
    store via Get-ScheduledTask (the Windows-blessed autostart record itself), never
    winspace's own log. `reload` is Triggered by a synthesized Win+Shift+R chord
    (Send-Chord); $mod is SUPER (the Windows key), which only registers because
    NoWinKeys=1 is baked into the winspace-e2e-nowinkeys snapshot (scripts/PROVISIONING.md).

    Each seam stages its own task-store precondition (Remove-WinspaceAutostartTask)
    so run order never matters, mirroring Set-DesktopCount for the VD seams.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # start_at_login ON: the running winspace must register the logon task. A reload
    # bind rides along so the live-toggle seam can flip the flag without a restart. A
    # double-quoted here-string, so `$mod is escaped to survive to the file verbatim.
    $script:ConfigOn = @"
`$mod = SUPER
bind = `$mod SHIFT, R, reload
start_at_login = true
"@

    # start_at_login OFF: the running winspace must remove the task (a clean no-op if
    # already absent). Same reload bind so the toggle seam can flip either direction.
    $script:ConfigOff = @"
`$mod = SUPER
bind = `$mod SHIFT, R, reload
start_at_login = false
"@
}

AfterAll {
    Remove-WinspaceAutostartTask   # never leak a registered task into a later seam
    Clear-WinspaceConfig           # restore built-in-default behaviour for later seams
}

Describe 'autostart' {

    # Register-on-start. winspace boots with start_at_login = true, so its Started{}
    # SyncAutostart registers the logon task. Assert the task exists with the full
    # ADR-0013 shape: a logon trigger, RestartCount = 3, unlimited ExecutionTimeLimit
    # (PT0S), and a Limited (LUA) run level — the create half of the declarative sync.
    # NB: no angle-bracket tokens in the name — Pester v5 reads `<foo>` as a
    # <...> template placeholder and evaluates $foo to expand it, which throws
    # 'variable $foo not set' under the Set-StrictMode the runner enables.
    It 'start_at_login on: registers the per-user \winspace logon task with the ADR-0013 shape' -Tag 'autostart' {
        $winspace = $null
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            $winspace = Start-Winspace

            # The sync runs on Started{}, posted just after the ready ('adopted') line,
            # so poll the task store rather than assume it is already there.
            Wait-Until -Because 'winspace to register the autostart logon task' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            $task = Get-WinspaceAutostartTask
            $task | Should -Not -BeNullOrEmpty -Because 'start_at_login = true must register the task'

            $task.Triggers.CimClass.CimClassName | Should -Contain 'MSFT_TaskLogonTrigger' `
                -Because 'ADR-0013 specifies a logon trigger'
            $task.Settings.RestartCount | Should -Be 3 -Because 'restart-on-failure is RestartCount = 3'
            $task.Settings.ExecutionTimeLimit | Should -Be 'PT0S' `
                -Because 'a session daemon must run without a kill-timer (unlimited)'
            "$($task.Principal.RunLevel)" | Should -Be 'Limited' `
                -Because 'winspace runs unprivileged — TASK_RUNLEVEL_LUA'
        } catch {
            Save-FailureScreenshot -Name 'autostart-register'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # Idempotent re-register. TASK_CREATE_OR_UPDATE must rewrite the one task in place,
    # never append a second. Launch (registers), stop, launch again (re-syncs), and
    # assert the \winspace folder still holds exactly one task.
    It 'start_at_login on twice: re-registering never duplicates the task' -Tag 'autostart' {
        $first = $null; $second = $null
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null

            $first = Start-Winspace
            Wait-Until -Because 'the first launch to register the task' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            Stop-Winspace -Process $first; $first = $null

            $second = Start-Winspace
            Wait-Until -Because 'the second launch to re-sync the task' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            # Settle: a create-or-update could momentarily race; the count is stable
            # once the second launch has synced, so a short confirm window suffices.
            Get-WinspaceAutostartTaskCount | Should -Be 1 `
                -Because 'TASK_CREATE_OR_UPDATE rewrites the one task, it never duplicates'
        } catch {
            Save-FailureScreenshot -Name 'autostart-idempotent'
            throw
        } finally {
            Stop-Winspace -Process $first
            Stop-Winspace -Process $second
        }
    }

    # Remove-on-start with the flag off. With the task present, a launch whose config
    # has start_at_login = false must delete it; a second such launch, with the task
    # already gone, is a clean no-op (ERROR_FILE_NOT_FOUND counted as success).
    It 'start_at_login off: removes the task, and a second off-launch is a clean no-op' -Tag 'autostart' {
        $seed = $null; $off1 = $null; $off2 = $null
        try {
            # Precondition: get the task genuinely registered (by an on-launch), so the
            # off-launch has something real to remove.
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            $seed = Start-Winspace
            Wait-Until -Because 'the seed launch to register the task' -Condition {
                $null -ne (Get-WinspaceAutostartTask)
            }
            Stop-Winspace -Process $seed; $seed = $null

            # First off-launch: the task must disappear.
            Set-WinspaceConfig -Content $script:ConfigOff | Out-Null
            $off1 = Start-Winspace
            Wait-Until -Because 'the off-launch to remove the registered task' -Condition {
                $null -eq (Get-WinspaceAutostartTask)
            }
            $off1.HasExited | Should -BeFalse -Because 'removing autostart must not crash the WM'
            Stop-Winspace -Process $off1; $off1 = $null

            # Second off-launch with the task already absent: still a clean no-op — the
            # process stays up and the task stays gone.
            $off2 = Start-Winspace
            Start-Sleep -Milliseconds 600   # let Started{}'s SyncAutostart run
            $off2.HasExited | Should -BeFalse -Because 'a repeated disable is a clean no-op, not a crash'
            Get-WinspaceAutostartTask | Should -BeNullOrEmpty `
                -Because 'deleting an already-absent task counts ERROR_FILE_NOT_FOUND as success'
        } catch {
            Save-FailureScreenshot -Name 'autostart-remove'
            throw
        } finally {
            Stop-Winspace -Process $seed
            Stop-Winspace -Process $off1
            Stop-Winspace -Process $off2
        }
    }

    # Live-reload toggle. A single winspace process, no restart: it boots with the flag
    # off (no task), the config is rewritten to on and `reload` fired — the task
    # appears; the config is rewritten back to off and `reload` fired — the task
    # disappears. Both edges ride the Reloaded{} SyncAutostart on the live process.
    It 'reload toggles the task live (appears on -> off flip) without a restart' -Tag 'autostart' {
        $winspace = $null
        try {
            Remove-WinspaceAutostartTask
            Set-WinspaceConfig -Content $script:ConfigOff | Out-Null
            $winspace = Start-Winspace
            $null = $winspace.Handle   # settle ExitCode/liveness queries (see MoveToWorkspace seam)

            # Boots off — no task. (Started{}'s SyncAutostart deleted any stale one.)
            Wait-Until -Because 'the off boot to leave no autostart task' -Condition {
                $null -eq (Get-WinspaceAutostartTask)
            }

            # Flip to on and reload: the task must appear. Re-send the chord inside the
            # poll — the hotkey re-fire is idempotent, and a reload re-registers in place.
            Set-WinspaceConfig -Content $script:ConfigOn | Out-Null
            Wait-Until -Because 'the reload to register the task live' -Condition {
                Send-Chord 'Win+Shift+R'
                $null -ne (Get-WinspaceAutostartTask)
            }
            $winspace.HasExited | Should -BeFalse -Because 'a live reload must not restart the WM'

            # Flip back to off and reload: the task must disappear — again on the same
            # live process.
            Set-WinspaceConfig -Content $script:ConfigOff | Out-Null
            Wait-Until -Because 'the reload to remove the task live' -Condition {
                Send-Chord 'Win+Shift+R'
                $null -eq (Get-WinspaceAutostartTask)
            }
            $winspace.HasExited | Should -BeFalse -Because 'the WM stayed up across both live toggles'
        } catch {
            Save-FailureScreenshot -Name 'autostart-reload-toggle'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
