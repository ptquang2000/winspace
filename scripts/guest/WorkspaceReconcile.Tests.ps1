<#
    Reconciliation Smoke seams (VM harness, ADR-0005) — the live half of
    ADR-0023 / PRD 0023: winspace keeps its `logical -> desktop` bindings true to a
    Virtual Desktop set the USER changes behind its back.

    The pure reconcile policy (lowest-free numbering, Provenance preservation,
    idempotence) is unit-tested at the reducer seam; only the running binary can
    exhibit these — a desktop created or destroyed with the native OS chords, and
    winspace's numbering agreeing with Task View afterwards.

    Desktops are not Displays, so the single-display VM (ADR-0008) is no obstacle
    here. Every assertion is on the OS Oracle (the Virtual Desktop registry state
    via Get-VdState) and on a DELTA around a Trigger, never an absolute — and every
    Trigger is real synthesized input, so the whole RegisterHotKey -> WM_HOTKEY ->
    Reducer -> COM-bridge path runs exactly as under a physical press.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
}

Describe 'workspace-reconcile' {

    # PRD 0023 stories 1-2. Before reconciliation this appended a desktop: logical 3
    # had no binding, so `workspace 3` materialized a brand-new one at the tail while
    # the user was already standing on the desktop they meant.
    It 'external-create: a desktop made with Win+Ctrl+D is landed on by $mod+N, not appended' -Tag 'external-create' {
        $winspace = $null
        try {
            # ── Arrange: two desktops, adopted as logical 1..2 ────────────────
            Set-DesktopCount 2
            $winspace = Start-Winspace
            $adopted = Get-VdState
            $adopted.Count | Should -Be 2

            # The user makes a third desktop by hand. winspace did not do this and
            # has no binding for it — until it reconciles.
            Send-Chord 'Win+Ctrl+D'
            Wait-Until -Because 'the externally-created desktop to appear' -Condition {
                (Get-VdState).Count -eq 3
            }
            $external = Get-VdState
            $newGuid = $external.Guids[-1]

            # Step off it, so landing back on it is a real switch and not a no-op.
            Send-Chord 'Alt+1'
            Wait-Until -Because 'current desktop to become the 1st' -Condition {
                (Get-VdState).CurrentGuid -eq $external.Guids[0]
            }

            # ── Act: the foreign desktop is bound at the lowest free number (3) ──
            Send-Chord 'Alt+3'
            Wait-Until -Because 'current desktop to become the externally-created one' -Condition {
                (Get-VdState).CurrentGuid -eq $newGuid
            }
            $after = Get-VdState

            # ── Assert: landed on it, and nothing was appended ────────────────
            $after.CurrentGuid | Should -Be $newGuid
            $after.Count       | Should -Be $external.Count   # 3, NOT 4
            for ($i = 0; $i -lt $external.Count; $i++) {
                $after.Guids[$i] | Should -Be $external.Guids[$i]
            }
        } catch {
            Save-FailureScreenshot -Name 'external-create'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD 0023 stories 3-4. A destroyed desktop must RELEASE its logical number, and
    # the next desktop must reuse it (lowest-free, not max+1). Observable because a
    # stale binding would make $mod+2 materialize a fourth desktop instead of landing
    # on the third that already exists.
    It 'external-destroy: a destroyed desktop frees its number and the next desktop reuses it' -Tag 'external-destroy' {
        $winspace = $null
        try {
            # ── Arrange: three desktops, adopted as logical 1..3 ──────────────
            Set-DesktopCount 3
            $winspace = Start-Winspace
            $staged = Get-VdState
            $staged.Count | Should -Be 3
            $first  = $staged.Guids[0]
            $second = $staged.Guids[1]
            $third  = $staged.Guids[2]

            # Destroy the MIDDLE desktop by hand, freeing logical 2 while 1 and 3
            # keep their numbers (bindings are GUID-anchored, ADR-0003).
            Send-Chord 'Alt+2'
            Wait-Until -Because 'current desktop to become the 2nd' -Condition {
                (Get-VdState).CurrentGuid -eq $second
            }
            Send-Chord 'Win+Ctrl+F4'
            Wait-Until -Because 'the 2nd desktop to be destroyed' -Condition {
                (Get-VdState).Count -eq 2
            }
            (Get-VdState).Guids | Should -Not -Contain $second

            # A fresh desktop appears at the TAIL — but the free NUMBER is 2, so
            # that is what reconciliation must bind it to.
            Send-Chord 'Win+Ctrl+D'
            Wait-Until -Because 'a replacement desktop to appear' -Condition {
                (Get-VdState).Count -eq 3
            }
            $replaced = Get-VdState
            $newGuid = $replaced.Guids[-1]
            $newGuid | Should -Not -Be $second

            Send-Chord 'Alt+1'
            Wait-Until -Because 'current desktop to become the 1st' -Condition {
                (Get-VdState).CurrentGuid -eq $first
            }

            # ── Act: the freed number now names the replacement desktop ───────
            Send-Chord 'Alt+2'
            Wait-Until -Because 'current desktop to become the replacement' -Condition {
                (Get-VdState).CurrentGuid -eq $newGuid
            }
            $after = Get-VdState

            # ── Assert: reused the freed number, appended nothing ─────────────
            $after.CurrentGuid | Should -Be $newGuid
            $after.Count       | Should -Be 3        # NOT 4 — no stale-binding append
            $after.Guids[0]    | Should -Be $first   # the survivors keep their slots
            $after.Guids[1]    | Should -Be $third
        } catch {
            Save-FailureScreenshot -Name 'external-destroy'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD 0023 story 5 + ADR-0023's live half. The user switches with the native
    # chord and winspace is told by the OS, with NO keypress of its own — so the
    # next winspace key behaves as if the user had been using winspace all along.
    # The observable is the round trip: walk away externally, then a winspace switch
    # to the desktop we walked FROM must land there without materializing anything.
    It 'external-switch: Win+Ctrl+Right leaves winspace correctly informed with no keypress' -Tag 'external-switch' {
        $winspace = $null
        try {
            # ── Arrange: three desktops adopted 1..3, standing on the 1st ─────
            Set-DesktopCount 3
            $winspace = Start-Winspace
            $staged = Get-VdState
            $first = $staged.Guids[0]
            $second = $staged.Guids[1]

            Send-Chord 'Alt+1'
            Wait-Until -Because 'current desktop to become the 1st' -Condition {
                (Get-VdState).CurrentGuid -eq $first
            }

            # ── Act: walk right with the OS's own chord — winspace is not asked ──
            Send-Chord 'Win+Ctrl+Right'
            Wait-Until -Because 'the OS to switch to the 2nd desktop' -Condition {
                (Get-VdState).CurrentGuid -eq $second
            }
            $external = Get-VdState
            $external.Count | Should -Be 3   # walking is not creating

            # ── Assert: winspace's bindings still name the desktops correctly ──
            # A stale or reordered map would send $mod+1 somewhere else, or append.
            Send-Chord 'Alt+1'
            Wait-Until -Because 'current desktop to return to the 1st' -Condition {
                (Get-VdState).CurrentGuid -eq $first
            }
            $after = Get-VdState
            $after.CurrentGuid | Should -Be $first
            $after.Count       | Should -Be 3
        } catch {
            Save-FailureScreenshot -Name 'external-switch'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
