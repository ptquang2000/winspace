<#
    Quit-cleanup Smoke seams (VM harness, ADR-0005) — the live half of ADR-0024 /
    PRD 0024: quitting winspace removes the Virtual Desktops IT created and leaves
    everything else exactly as it found it.

    The pure half — desktopsToCleanup filtering on Provenance, and reduce(Quit)
    emitting the ordered {CleanupWorkspaces, Exit} pair — is unit-tested at the
    reducer seam. Only the running binary can exhibit these: the desktop set that
    actually exists after the process is gone.

    Oracle policy (ADR-0005): the OS Virtual Desktop state (Get-VdState), read AFTER
    the process has exited — never winspace's own log. A cleanup feature that only
    its own logs can confirm is not confirmed. Assertions are on the DELTA against
    the pre-start set, and each seam stages its own desktop-count precondition so run
    order never matters.

    The case worth the most is 'foreign-survives': its failure mode is data loss.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop

    # Quit and wait for the process to be genuinely gone, so every assertion that
    # follows reads a settled desktop set rather than one mid-teardown.
    function script:Stop-WinspaceAndWait {
        param([Parameter(Mandatory)]$Process, [string]$Chord = 'Alt+Shift+Q')
        $wsPid = $Process.Id      # $PID is read-only (this runner's own id)
        Send-Chord $Chord
        Wait-Until -Because 'the winspace process to exit' -Condition {
            $Process.HasExited -or -not (Get-Process -Id $wsPid -ErrorAction SilentlyContinue)
        }
    }
}

Describe 'quit-cleanup' {

    # PRD 0024, the headline: the session ends with exactly the desktops that
    # existed before winspace started. Before this feature every session left its
    # materialized desktops behind for good.
    It 'created-removed: desktops winspace created are gone after quit' -Tag 'created-removed' {
        $winspace = $null
        try {
            # ── Arrange: two desktops, all foreign (adopted) ──────────────────
            Set-DesktopCount 2
            $before = Get-VdState
            $before.Count | Should -Be 2

            $winspace = Start-Winspace

            # winspace materializes two desktops on demand (logical 3 and 4 have no
            # binding, so each $mod chord creates exactly one at the tail).
            Send-Chord 'Alt+3'
            Wait-Until -Because 'winspace to materialize a 3rd desktop' -Condition {
                (Get-VdState).Count -eq 3
            }
            Send-Chord 'Alt+4'
            Wait-Until -Because 'winspace to materialize a 4th desktop' -Condition {
                (Get-VdState).Count -eq 4
            }

            # ── Act: quit down the clean bind path ────────────────────────────
            Stop-WinspaceAndWait -Process $winspace
            $winspace = $null

            # ── Assert: the OS desktop set matches the pre-start set exactly ──
            Wait-Until -Because 'the created desktops to be removed' -Condition {
                (Get-VdState).Count -eq $before.Count
            }
            $after = Get-VdState
            $after.Count | Should -Be $before.Count
            for ($i = 0; $i -lt $before.Count; $i++) {
                $after.Guids[$i] | Should -Be $before.Guids[$i]
            }
        } catch {
            Save-FailureScreenshot -Name 'quit-created-removed'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # The Provenance guarantee, live — the case whose failure is data loss. A
    # desktop the user made by hand is bound (addressable) but NOT ours, so cleanup
    # must leave it standing.
    It 'foreign-survives: a desktop created externally survives the quit' -Tag 'foreign-survives' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $winspace = Start-Winspace

            # winspace makes one (ours), the user makes one by hand (foreign).
            Send-Chord 'Alt+3'
            Wait-Until -Because 'winspace to materialize a 3rd desktop' -Condition {
                (Get-VdState).Count -eq 3
            }
            Send-Chord 'Win+Ctrl+D'
            Wait-Until -Because 'the externally-created desktop to appear' -Condition {
                (Get-VdState).Count -eq 4
            }
            $staged = Get-VdState
            $foreignGuid = $staged.Guids[-1]

            Stop-WinspaceAndWait -Process $winspace
            $winspace = $null

            # Exactly one desktop is removed: the one winspace made. The user's
            # survives — "clean up all workspaces" never means "delete your stuff".
            Wait-Until -Because 'the winspace-created desktop to be removed' -Condition {
                (Get-VdState).Count -eq 3
            }
            $after = Get-VdState
            $after.Count | Should -Be 3
            $after.Guids | Should -Contain $foreignGuid
        } catch {
            Save-FailureScreenshot -Name 'quit-foreign-survives'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # Windows are never lost: RemoveDesktop takes a fallback desktop and MIGRATES
    # the windows on the removed one onto it. Cleanup can only consolidate.
    It 'window-migrates: a window parked on a removed desktop still exists afterwards, on home' -Tag 'window-migrates' {
        $winspace = $null
        $window = $null
        try {
            # One desktop, so home is unambiguous and logical 2 has no binding —
            # the silent move materializes it, making that desktop OURS.
            Set-DesktopCount 1
            $winspace = Start-Winspace

            # The runner console is an Eligible top-level window that can hold the
            # foreground; hide it so the test window is unambiguously the Origin.
            Set-RunnerConsoleVisible $false
            $window = Start-TestWindow -Style sizable -X 200 -Y 160 -Width 480 -Height 320 `
                                       -Title 'winspace-cleanup-window'
            Wait-Until -Because 'the test window to be the foreground Origin' -Condition {
                (Get-ForegroundWindow) -eq $window.Hwnd
            }

            # Home is read off the WINDOW, not the registry. At a single desktop Windows
            # persists no VirtualDesktopIDs list and no CurrentVirtualDesktop value, so
            # Get-VdState reports Count 1 (floored) with an EMPTY GUID list and CurrentGuid
            # [guid]::Empty — reading home from there yields all-zeros and the final
            # assertion fails as `Expected 00000000-0000-0000-0000-000000000000`.
            # GetWindowDesktopId is a COM read that answers correctly at one desktop, and
            # the window's own pre-move desktop IS home by construction: it was created on
            # the only desktop there was. That is also exactly what the assertion means.
            $home = Get-WindowDesktopId -Hwnd $window.Hwnd

            # Alt+Shift+2 -> movetoworkspacesilent 2: winspace materializes desktop 2
            # (recorded as ours) and parks the window on it.
            Send-Chord 'Alt+Shift+2'
            Wait-Until -Because 'winspace to materialize a 2nd desktop for the move' -Condition {
                (Get-VdState).Count -eq 2
            }
            $parked = Get-VdState
            Wait-Until -Because 'the window to be reassigned to the created desktop' -Condition {
                (Get-WindowDesktopId -Hwnd $window.Hwnd) -eq $parked.Guids[1]
            }

            Stop-WinspaceAndWait -Process $winspace
            $winspace = $null

            Wait-Until -Because 'the created desktop to be removed' -Condition {
                (Get-VdState).Count -eq 1
            }

            # The window is still alive and CONSOLIDATED onto home — RemoveDesktop's
            # fallback migrates windows rather than destroying them.
            $window.Process.HasExited | Should -BeFalse
            (Get-VdState).Count | Should -Be 1
            (Get-WindowDesktopId -Hwnd $window.Hwnd) | Should -Be $home
        } catch {
            Save-FailureScreenshot -Name 'quit-window-migrates'
            throw
        } finally {
            Set-RunnerConsoleVisible $true
            Stop-TestWindow $window
            Stop-Winspace -Process $winspace
        }
    }

    # ADR-0024's amendment, live: the **Cleanup anchor**. Home used to do two jobs
    # that only looked like one, because until now it always survived. Destroy it by
    # hand in Task View and cleanup used to log and return — leaving EVERY
    # winspace-created desktop standing, which is the accumulation this ADR exists to
    # end. The anchor is derived instead, preferring a FOREIGN desktop so cleanup never
    # exempts one of its own by standing on it.
    It 'home-destroyed: quit still removes created desktops after home is destroyed by hand' -Tag 'home-destroyed' {
        $winspace = $null
        try {
            # Three desktops, all foreign. winspace starts on the LAST (Set-DesktopCount
            # leaves the active desktop there), so that one becomes home — and it is the
            # one we then destroy.
            Set-DesktopCount 3
            $before = Get-VdState
            $before.Count | Should -Be 3
            $homeGuid = $before.CurrentGuid
            $survivors = @($before.Guids | Where-Object { $_ -ne $homeGuid })

            $winspace = Start-Winspace

            # winspace materializes a 4th desktop: this is the one that must NOT be
            # left standing when home disappears.
            Send-Chord 'Alt+4'
            Wait-Until -Because 'winspace to materialize a 4th desktop' -Condition {
                (Get-VdState).Count -eq 4
            }

            # Land back on home and destroy it by hand — Win+Ctrl+F4 closes the ACTIVE
            # desktop, which is exactly the Task View action being modelled.
            Send-Chord 'Alt+3'
            Wait-Until -Because 'to be standing on the home desktop again' -Condition {
                (Get-VdState).CurrentGuid -eq $homeGuid
            }
            Send-Chord 'Win+Ctrl+F4'
            Wait-Until -Because 'the home desktop to be destroyed by hand' -Condition {
                (Get-VdState).Guids -notcontains $homeGuid
            }

            Stop-WinspaceAndWait -Process $winspace
            $winspace = $null

            # The created desktop is gone even though home is; the user's two survive.
            Wait-Until -Because 'the created desktop to be removed despite home being gone' -Condition {
                (Get-VdState).Count -eq $survivors.Count
            }
            $after = Get-VdState
            $after.Count | Should -Be $survivors.Count `
                -Because 'a destroyed home must not spare every winspace-created desktop'
            foreach ($guid in $survivors) {
                $after.Guids | Should -Contain $guid `
                    -Because 'foreign desktops survive quit untouched, anchor or not'
            }
        } catch {
            Save-FailureScreenshot -Name 'quit-home-destroyed'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # All three Quit paths converge on `reduce`, so each inherits cleanup for free.
    # The uninstall path matters most: uninstalling and leaving eight desktops
    # behind would be the worst version of this feature.
    It 'uninstall-cleans: the winspace uninstall path also removes created desktops' -Tag 'uninstall-cleans' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $before = Get-VdState
            $winspace = Start-Winspace

            Send-Chord 'Alt+3'
            Wait-Until -Because 'winspace to materialize a 3rd desktop' -Condition {
                (Get-VdState).Count -eq 3
            }

            # `winspace uninstall` sends the Control::Quit message to the live
            # Primary, which routes into the same reduce(Quit) path as the bind.
            $wsPid = $winspace.Id
            Start-Process -FilePath (Get-WinspaceExe) -ArgumentList 'uninstall' -Wait | Out-Null
            Wait-Until -Because 'the primary to exit on the uninstall control message' -Condition {
                $winspace.HasExited -or -not (Get-Process -Id $wsPid -ErrorAction SilentlyContinue)
            }
            $winspace = $null

            Wait-Until -Because 'the created desktop to be removed' -Condition {
                (Get-VdState).Count -eq $before.Count
            }
            (Get-VdState).Count | Should -Be $before.Count
        } catch {
            Save-FailureScreenshot -Name 'quit-uninstall-cleans'
            throw
        } finally {
            Stop-Winspace -Process $winspace
            Remove-WinspaceAutostartTask   # uninstall removes the logon task too
        }
    }
}
