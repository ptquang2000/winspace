<#
    Config-reload Smoke seam (VM harness, ADR-0005) — the live-only behaviour the
    pure Reducer/parser seams structurally cannot reach (issue 09 / ADR-0012): a
    bound `reload` hotkey re-reads the on-disk config and re-applies it live — with
    no restart — across the two I/O threads that hold config-derived state.

    The parse (reload dispatcher, windowrule ignore, start_at_login, removed-with-
    tiling diagnostics) and the pure Reducer (Reload{} -> exactly one ReloadConfig{},
    no State change) are fully unit-tested (config_test.cpp / reducer_test.cpp). This
    seam proves only what those cannot: that the Worker's ReloadConfig executor
    genuinely re-reads + re-parses the file on its thread, hands the new Binds to the
    Hotkey thread (which rebuilds its HotkeyTable via RegisterHotKey), and applies
    the swap ATOMICALLY — a clean file re-registers hotkeys, a broken file keeps the
    last-good running config live and logs the errors (nothing partially applied).

    Oracle policy (ADR-0005): assert on independent OS state — the Virtual Desktop
    registry GUID list / current-desktop GUID (Get-VdState) around a synthesized
    chord (Send-Chord), and process liveness (the cached process handle) — and on
    winspace's own run.log only where no external observable exists (the keep-last-
    good "keeping the running config" diagnostic, which by design changes nothing).

    $mod is ALT (the Alt key); Alt+<n> (and Alt+Shift+<n>) chords register on a stock
    Windows 11 with no policy (ADR-0014), as for every other workspace-switch seam. The
    snapshot is just winspace-e2e (stock). Each seam stages its own desktop-count
    precondition (Set-DesktopCount) so run order never matters.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # Initial config: Alt+1 / Alt+2 switch, Alt+Shift+R reloads. NO Alt+3 bind yet —
    # the reload adds it. A double-quoted here-string so `$mod is escaped to survive
    # to the file. winspace re-seeds its default once the file is cleared (AfterAll).
    $script:ConfigInitial = @"
`$mod = ALT
bind = `$mod, 1, workspace, 1
bind = `$mod, 2, workspace, 2
bind = `$mod SHIFT, R, reload
"@

    # Clean edited config: Alt+2 is GONE and Alt+3 is NEW; the reload bind persists so
    # the user can keep reloading. A clean parse, so reload applies it whole.
    $script:ConfigAddWin3 = @"
`$mod = ALT
bind = `$mod, 1, workspace, 1
bind = `$mod, 3, workspace, 3
bind = `$mod SHIFT, R, reload
"@

    # Broken config: the `teleport` dispatcher is unknown, so parse() yields a
    # Diagnostic and the WHOLE file is rejected on reload (atomic keep-last-good). It
    # also tries to add Alt+3 — which must NOT take effect, since nothing is applied.
    $script:ConfigBroken = @"
`$mod = ALT
bind = `$mod, 1, workspace, 1
bind = `$mod, 2, workspace, 2
bind = `$mod SHIFT, R, reload
bind = `$mod, 3, teleport, 3
"@
}

AfterAll {
    Clear-WinspaceConfig   # restore built-in-default behaviour for later seams
}

Describe 'config-reload' {

    # Apply-new-bind. winspace boots on the initial config (no Alt+3), the file is
    # rewritten to add Alt+3 and drop Alt+2, and `reload` fires. The Worker re-reads
    # + re-parses on its thread and hands the new Binds to the Hotkey thread, which
    # re-registers: Alt+3 (newly added) now switches, and Alt+2 (removed) no longer
    # does. Both halves are the live re-registration the unit tests cannot reach.
    It 'reload: re-parses config and re-registers hotkeys live (new bind fires, removed bind does not)' -Tag 'config-reload' {
        $winspace = $null
        try {
            Set-DesktopCount 3
            $vd = Get-VdState
            $vd.Count | Should -Be 3 -Because 'the seam switches among workspaces 1..3'
            $guid1 = $vd.Guids[0]
            $guid3 = $vd.Guids[2]

            Set-WinspaceConfig -Content $script:ConfigInitial | Out-Null
            $winspace = Start-Winspace

            # Rewrite the file, then trigger the reload. The Worker logs
            # "reload: applied new config" once it has fanned out to both threads.
            Set-WinspaceConfig -Content $script:ConfigAddWin3 | Out-Null
            Send-Chord 'Alt+Shift+R'
            Wait-Until -Because 'the Worker to apply the clean reloaded config' -Condition {
                (Get-WinspaceLogText) -match 'reload: applied new config'
            }

            # The newly-added Alt+3 fires. Re-send inside the poll: the Hotkey thread
            # rebuilds its table asynchronously just after the Worker's log line, so
            # the first chord can precede re-registration by a hair — a workspace
            # switch is idempotent, so re-sending until it lands is safe.
            Wait-Until -Because 'the newly-added Alt+3 bind to switch to workspace 3' -Condition {
                Send-Chord 'Alt+3'
                (Get-VdState).CurrentGuid -eq $guid3
            }
            (Get-VdState).CurrentGuid | Should -Be $guid3 -Because 'the reload re-registered the added bind'

            # The removed Alt+2 no longer fires. Land on workspace 1, press Alt+2, and
            # confirm the current desktop does NOT move (a negative — a bounded settle
            # window, since there is no positive edge to wait on).
            Wait-Until -Because 'to return to workspace 1 before testing the removed bind' -Condition {
                Send-Chord 'Alt+1'
                (Get-VdState).CurrentGuid -eq $guid1
            }
            Send-Chord 'Alt+2'
            Start-Sleep -Milliseconds 600
            (Get-VdState).CurrentGuid | Should -Be $guid1 `
                -Because 'the removed Alt+2 bind was unregistered on reload, so it switches nothing'
        } catch {
            Save-FailureScreenshot -Name 'config-reload-apply'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # Keep-last-good. A broken edited file is rejected WHOLE: the running config stays
    # live (the previously-working Alt+2 still switches), the process never dies, and
    # a diagnostic is logged. Then a good file reloads cleanly on top — proving the
    # `reload` bind itself survived the bad reload so the user can fix and retry.
    It 'reload: an invalid config keeps the last-good config live and logs diagnostics (no crash)' -Tag 'config-reload' {
        $winspace = $null
        try {
            Set-DesktopCount 3
            $vd = Get-VdState
            $guid1 = $vd.Guids[0]
            $guid2 = $vd.Guids[1]
            $guid3 = $vd.Guids[2]

            Set-WinspaceConfig -Content $script:ConfigInitial | Out-Null
            $winspace = Start-Winspace
            $null = $winspace.Handle   # settle ExitCode/liveness queries (see MoveToWorkspace seam)

            # Baseline: Alt+2 works before the bad reload.
            Wait-Until -Because 'the initial Alt+2 bind to switch to workspace 2' -Condition {
                Send-Chord 'Alt+2'
                (Get-VdState).CurrentGuid -eq $guid2
            }

            # Write the broken file and trigger reload. The Worker logs the diagnostic
            # and "keeping the running config" — nothing is applied.
            Set-WinspaceConfig -Content $script:ConfigBroken | Out-Null
            Send-Chord 'Alt+Shift+R'
            Wait-Until -Because 'the Worker to reject the broken file and keep the last-good config' -Condition {
                (Get-WinspaceLogText) -match 'keeping the running config'
            }

            # The process is still alive — a bad reload never crashes.
            $winspace.HasExited | Should -BeFalse -Because 'a rejected reload must not take winspace down'

            # The previously-working Alt+2 STILL fires (last-good retained); the broken
            # file's attempted Alt+3 does NOT (nothing was applied). Land on 1 first.
            Wait-Until -Because 'to return to workspace 1' -Condition {
                Send-Chord 'Alt+1'
                (Get-VdState).CurrentGuid -eq $guid1
            }
            Send-Chord 'Alt+3'
            Start-Sleep -Milliseconds 600
            (Get-VdState).CurrentGuid | Should -Be $guid1 `
                -Because 'the broken file was rejected whole, so its Alt+3 bind was never registered'
            Wait-Until -Because 'the retained Alt+2 bind to still switch to workspace 2' -Condition {
                Send-Chord 'Alt+2'
                (Get-VdState).CurrentGuid -eq $guid2
            }
            (Get-VdState).CurrentGuid | Should -Be $guid2 -Because 'the last-good config stayed live'

            # The reload bind survived the bad reload: a good file now reloads cleanly
            # and its newly-added Alt+3 fires.
            Set-WinspaceConfig -Content $script:ConfigAddWin3 | Out-Null
            Send-Chord 'Alt+Shift+R'
            Wait-Until -Because 'the Worker to apply the good config after the bad one' -Condition {
                (Get-WinspaceLogText) -match 'reload: applied new config'
            }
            Wait-Until -Because 'the re-fixed Alt+3 bind to switch to workspace 3' -Condition {
                Send-Chord 'Alt+3'
                (Get-VdState).CurrentGuid -eq $guid3
            }
            (Get-VdState).CurrentGuid | Should -Be $guid3 `
                -Because 'the reload bind survived the bad reload, so a fix-and-reload works'
        } catch {
            Save-FailureScreenshot -Name 'config-reload-keep-last-good'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
