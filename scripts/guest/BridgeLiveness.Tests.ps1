<#
    Virtual Desktop bridge liveness Smoke seams (VM harness, ADR-0005) — the live
    half of ADR-0025 / PRD 0027: winspace treats the COM bridge as a CONNECTION that
    can drop and be rebuilt, rather than a capability acquired once at birth.

    Nothing here can be reached from the core seam. The pure half — reconcile
    preserving Provenance across a rebuild, and the cleanup anchor's preference order
    — is unit-tested in winspace_test.cpp. What only the running binary can exhibit is
    whether a shell restart actually ends in a working `workspace N`, and whether a
    placement issued during an outage actually lands afterwards.

    Two triggers, deliberately different in kind:

      * Restart-Shell is the GENUINE fault. It destroys the process hosting the
        ImmersiveShell, so winspace's proxies go stale exactly as they do when the
        shell crashes on its own — no simulation anywhere in the loop.
      * WINSPACE_FORCE_VD_UNAVAILABLE_MS is a FIXTURE on this same seam, mirroring
        the existing WINSPACE_FORCE_VD_VARIANT hook and existing for the same reason:
        to make an otherwise unreachable I/O-layer state observable. It is the only
        way to cover the Pending-move path — a shell restart cannot, because no
        windows are appearing during one, and a login-timed test would pass or fail
        on machine speed.

    Oracle policy (ADR-0005): the OS Virtual Desktop state (Get-VdState) and the
    public IVirtualDesktopManager::GetWindowDesktopId (Get-WindowDesktopId), never
    winspace's own log. The one exception is deliberate and narrow: the seams that
    must know WHEN the outage window has passed read the log to sequence themselves,
    and still assert on OS state.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # Long enough that everything a seam does after Start-Winspace lands INSIDE the
    # outage, short enough that no seam waits on it for long. winspace reaches
    # Connected on its own the moment it lapses — no keypress, no restart.
    #
    # The window is measured from winspace's OWN startup, so a seam must not spend it
    # on setup: the two placement seams create their test window BEFORE launching
    # winspace, because spawning a WinForms child on a cold guest can take seconds and
    # would otherwise let the outage lapse before the window ever appeared. Doing the
    # setup first also exercises the more faithful path — startup rule adoption, which
    # is exactly what happens at login when `exec-once` apps are already up.
    $script:OutageMs = 10000

    # A winspace that is up but DISCONNECTED never reaches the default ready line, so
    # the forced-outage launches wait on the disconnected diagnostic instead.
    $script:DisconnectedPattern = 'bridge: disconnected'
    # `(re)?` on purpose: a winspace that was BORN disconnected (the forced-outage
    # fixture, and the real logon race) reaches Connected for the FIRST time when it
    # recovers, so it logs 'connected'; one that lost a live connection logs
    # 'reconnected'. Both are the milestone these seams sequence on, and neither
    # matches 'disconnected'.
    $script:ConnectedPattern = 'bridge: (re)?connected'

    function script:Wait-Connected {
        param([int]$TimeoutSec = 40)
        Wait-Until -TimeoutSec $TimeoutSec -Because 'the bridge to reach Connected' -Condition {
            (Get-WinspaceLogText) -match $script:ConnectedPattern
        }
    }

    function script:Stop-WinspaceAndWait {
        param([Parameter(Mandatory)]$Process, [string]$Chord = 'Alt+Shift+Q')
        $wsPid = $Process.Id
        Send-Chord $Chord
        Wait-Until -Because 'the winspace process to exit' -Condition {
            $Process.HasExited -or -not (Get-Process -Id $wsPid -ErrorAction SilentlyContinue)
        }
    }
}

Describe 'bridge-liveness' {

    # THE headline. Before this work, one shell restart disabled every Workspace
    # operation for the rest of the session: winspace stayed up, every hotkey stayed
    # registered, focus/tile/movetodisplay kept working, and only `workspace N` and
    # placement were silently dead. The user's only recovery was to notice, diagnose,
    # and restart winspace by hand.
    It 'shell-restart: workspace N switches again after the shell is restarted' -Tag 'shell-restart' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $winspace = Start-Winspace
            $staged = Get-VdState
            $staged.Count | Should -Be 2

            # The genuine fault: the process hosting the ImmersiveShell dies.
            Restart-Shell
            Wait-Connected

            # Alt+1 — a bound workspace, so this is a pure switch with nothing created.
            Send-Chord 'Alt+1'
            Wait-Until -Because 'the switch to land on workspace 1 after recovery' -Condition {
                (Get-VdState).CurrentGuid -eq $staged.Guids[0]
            }
            (Get-VdState).CurrentGuid | Should -Be $staged.Guids[0] `
                -Because 'a shell restart must not disable workspace switching for the session'

            # Logical numbers still address the same desktops — muscle memory survives.
            Send-Chord 'Alt+2'
            Wait-Until -Because 'the switch to land on workspace 2 after recovery' -Condition {
                (Get-VdState).CurrentGuid -eq $staged.Guids[1]
            }

            # And winspace is still the same process: alive, windowless, holding its
            # hotkeys (which it just demonstrated by responding to two of them).
            $winspace.HasExited | Should -BeFalse -Because 'recovery must not require a restart'
            # No @() wrapper — Get-WinspaceWindows already comma-protects its array.
            $windows = Get-WinspaceWindows -ProcessId $winspace.Id
            $windows.Count | Should -Be 0 `
                -Because 'recovery must not put a console or a taskbar button on screen'
        } catch {
            Save-FailureScreenshot -Name 'bridge-shell-restart'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # Recovery restores the whole bridge, not just the switch path: create-on-demand
    # needs CreateDesktop off the re-acquired manager, and the new desktop must be
    # bound and recorded as winspace's.
    It 'recovery-create: creating a Workspace on demand works again after a shell restart' -Tag 'recovery-create' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $winspace = Start-Winspace

            Restart-Shell
            Wait-Connected

            # Logical 3 has no binding, so the switch materializes exactly one desktop
            # at the tail and lands on it.
            Send-Chord 'Alt+3'
            Wait-Until -Because 'winspace to materialize a 3rd desktop after recovery' -Condition {
                (Get-VdState).Count -eq 3
            }
            $after = Get-VdState
            $after.Count | Should -Be 3
            $after.CurrentGuid | Should -Be $after.Guids[2] `
                -Because 'create-on-demand switches to the desktop it just made'
        } catch {
            Save-FailureScreenshot -Name 'bridge-recovery-create'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # THE most important seam in this work. Provenance loss is silent, it surfaces
    # only at quit, and its two directions are "leaves the user's desktops littered
    # with winspace's" and "deletes the user's desktops". Reconstructing the bridge on
    # reconnect — the smallest possible diff — would re-run Adoption and tag every
    # desktop foreign, after which this seam's created desktop would survive the quit.
    It 'recovery-provenance: a desktop created before a shell restart is still cleaned up at quit' -Tag 'recovery-provenance' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2

            $winspace = Start-Winspace

            # Ours: winspace materializes it, so Provenance says createdByWinspace.
            Send-Chord 'Alt+3'
            Wait-Until -Because 'winspace to materialize a 3rd desktop' -Condition {
                (Get-VdState).Count -eq 3
            }

            Restart-Shell
            Wait-Connected

            Stop-WinspaceAndWait -Process $winspace
            $winspace = $null

            # Exactly the pre-start set survives: the created desktop is gone (its
            # Provenance came through the rebuild), the user's two are untouched.
            Wait-Until -Because 'the created desktop to be removed at quit' -Condition {
                (Get-VdState).Count -eq $before.Count
            }
            $after = Get-VdState
            $after.Count | Should -Be $before.Count
            for ($i = 0; $i -lt $before.Count; $i++) {
                $after.Guids[$i] | Should -Be $before.Guids[$i] `
                    -Because 'a reconnect must not turn the user''s desktops into winspace''s, or vice versa'
            }
        } catch {
            Save-FailureScreenshot -Name 'bridge-recovery-provenance'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # The other half of the reported bug, and the reason the Pending move exists.
    # Place-once attempts a Place rule's Workspace move EXACTLY ONCE, on the window's
    # first Eligible Appeared. At login the Launch entries fire immediately and their
    # windows appear while the shell is still coming up — so the move failed, was
    # never retried, and the app sat on the wrong Workspace for the whole session.
    It 'outage-queued-move: an app that appears during an outage lands on its rule''s Workspace after recovery' -Tag 'outage-queued-move' {
        $winspace = $null
        $window = $null
        try {
            Set-DesktopCount 2
            $staged = Get-VdState
            $target = $staged.Guids[0]        # workspace 1 — inactive; we sit on 2

            Set-WinspaceConfig -Content @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
windowrule = workspace 1, title:winspace-outage-window
"@ | Out-Null

            # The window FIRST, so none of the outage is spent on spawn latency. Its
            # Appeared then reaches winspace via startup rule adoption — the same path
            # an `exec-once` app that is already up takes at login.
            Set-RunnerConsoleVisible $false
            $window = Start-TestWindow -Style sizable -X 200 -Y 160 -Width 480 -Height 320 `
                                       -Title 'winspace-outage-window'
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Not -Be $target `
                -Because 'the window must start somewhere other than its rule target'

            # Born disconnected. Every Workspace operation is unavailable until the
            # forced window lapses; everything else works normally throughout.
            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_UNAVAILABLE_MS = "$script:OutageMs" } `
                                       -ReadyPattern $script:DisconnectedPattern

            # The move was attempted and could not land — it is queued, not lost.
            Wait-Until -Because 'the move to be queued while disconnected' -Condition {
                (Get-WinspaceLogText) -match 'queued the move of window'
            }
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Not -Be $target `
                -Because 'the bridge is disconnected, so nothing can have moved yet'

            Wait-Connected

            # The queued move replays on reconnection, so the window ends up exactly
            # where its rule always said it should be.
            Wait-Until -TimeoutSec 15 -Because 'the queued move to replay onto workspace 1' -Condition {
                (Get-WindowDesktopId -Hwnd $window.Hwnd) -eq $target
            }
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $target `
                -Because 'a placement deferred by disconnection must still be applied'
        } catch {
            Save-FailureScreenshot -Name 'bridge-outage-queued-move'
            throw
        } finally {
            Set-RunnerConsoleVisible $true
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
            Clear-WinspaceConfig
        }
    }

    # Closing a window must never resurrect it somewhere else. The queue is pruned at
    # REPLAY time rather than on the Vanished edge, so the Reducer stays unaware the
    # queue exists at all.
    It 'outage-closed-window: a queued move for a window closed during the outage is discarded' -Tag 'outage-closed-window' {
        $winspace = $null
        $window = $null
        try {
            Set-DesktopCount 2
            $staged = Get-VdState
            $target = $staged.Guids[0]

            Set-WinspaceConfig -Content @"
`$mod = ALT
bind = `$mod SHIFT, Q, quit
windowrule = workspace 1, title:winspace-outage-closed
"@ | Out-Null

            # Window first (see the OutageMs note), so the queued move is recorded
            # promptly and the whole outage is left for closing the window in.
            Set-RunnerConsoleVisible $false
            $window = Start-TestWindow -Style sizable -X 200 -Y 160 -Width 480 -Height 320 `
                                       -Title 'winspace-outage-closed'

            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_UNAVAILABLE_MS = "$script:OutageMs" } `
                                       -ReadyPattern $script:DisconnectedPattern

            Wait-Until -Because 'the move to be queued while disconnected' -Condition {
                (Get-WinspaceLogText) -match 'queued the move of window'
            }

            # Close it BEFORE recovery: the queued move now names a dead window.
            Close-TestWindow -Window $window
            $closed = $window
            $window = $null

            Wait-Connected
            # Give the replay a bounded window to do something wrong in.
            Start-Sleep -Milliseconds 800

            # Nothing was resurrected, nothing changed hands, and winspace is fine.
            $closed.Process.HasExited | Should -BeTrue
            (Get-VdState).Count | Should -Be 2 -Because 'a discarded move must create nothing'
            (Get-VdState).CurrentGuid | Should -Be $staged.CurrentGuid
            $target | Should -Not -BeNullOrEmpty
            $winspace.HasExited | Should -BeFalse -Because 'replaying a stale queue must not fault'
        } catch {
            Save-FailureScreenshot -Name 'bridge-outage-closed-window'
            throw
        } finally {
            Set-RunnerConsoleVisible $true
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
            Clear-WinspaceConfig
        }
    }

    # Moves queue; switches DROP. A move is a promise made to a window on an edge
    # nobody is watching, with no second chance under Place-once. A switch is a live
    # response to a keypress — replaying it after the outage is not a delayed success
    # but the desktop moving out from under someone who already gave up.
    It 'outage-switch-dropped: a workspace press made during an outage is not replayed after recovery' -Tag 'outage-switch-dropped' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $staged = Get-VdState
            $current = $staged.CurrentGuid

            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_UNAVAILABLE_MS = "$script:OutageMs" } `
                                       -ReadyPattern $script:DisconnectedPattern

            # Press into the outage. Alt+1 names the OTHER desktop, so a switch that
            # happened — then or later — is unmistakable.
            Send-Chord 'Alt+1'
            Start-Sleep -Milliseconds 500
            (Get-VdState).CurrentGuid | Should -Be $current `
                -Because 'a press made while disconnected is a no-op'

            Wait-Connected
            Start-Sleep -Milliseconds 800    # a replay would have landed by now

            (Get-VdState).CurrentGuid | Should -Be $current `
                -Because 'a dropped press must never be replayed after recovery'

            # And the bridge really did come back — the very next press works.
            Send-Chord 'Alt+1'
            Wait-Until -Because 'a press made AFTER recovery to switch' -Condition {
                (Get-VdState).CurrentGuid -eq $staged.Guids[0]
            }
        } catch {
            Save-FailureScreenshot -Name 'bridge-outage-switch-dropped'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
