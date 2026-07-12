<#
    Workspace-switch Smoke seams (VM harness, ADR-0005) — the six-step Manual smoke
    script for the config-driven workspace-switch slice (docs/adr/0002, docs/adr/0003)
    run unattended: create-on-demand,
    windowless, adoption, GUID-stability (+ a skipped literal reorder), clean quit,
    and the variant diagnostic.

    Each asserts on independent OS state where an external observable exists
    (Get-VdState registry deltas, Get-WinspaceWindows enumeration, Get-Process
    liveness) and on winspace's own log only where none does (the resolved vtable
    variant). Assertions are on the DELTA around a Trigger, never an absolute.
    winspace source is untouched: every chord goes through the real
    RegisterHotKey -> WM_HOTKEY -> Reducer -> COM-bridge path, and each seam stages
    its own desktop-count precondition (Set-DesktopCount) so run order never matters.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
}

Describe 'workspace-switch' {

    # $mod is ALT (the Alt key) in the seeded default config (src/io/config_io.cpp),
    # so the workspace chords are Alt+<n>. Alt+<n> register on a stock Windows 11 with
    # no policy (ADR-0014), so the snapshot is just winspace-e2e (stock). It does NOT
    # touch the native OS Virtual Desktop hotkeys (Win+Ctrl+D / Win+Ctrl+F4), which
    # still work and are used only to stage/unstage the precondition.
    It 'create-on-demand: $mod+5 spawns one desktop at the tail and switches to it' -Tag 'create-on-demand' {
        $winspace = $null
        try {
            # ── Arrange: stage exactly 3 desktops, resetting from whatever the
            # previous seam left, so this seam passes at any position in the suite.
            Set-DesktopCount 3
            (Get-VdState).Count | Should -Be 3

            # winspace must adopt the 3 staged desktops (logical 1..3) at startup,
            # so launch it only now.
            $winspace = Start-Winspace

            # Focus an existing workspace first ($mod+2) so we are demonstrably
            # not sitting on the tail when create-on-demand fires.
            $staged = Get-VdState
            Send-Chord 'Alt+2'
            Wait-Until -Because 'current desktop to become the 2nd (adoption)' -Condition {
                (Get-VdState).CurrentGuid -eq $staged.Guids[1]
            }

            # ── Act: $mod+5 — logical 5 has no binding, so the bridge
            # materializes exactly one new desktop at the tail and switches ────
            $before = Get-VdState
            Send-Chord 'Alt+5'
            Wait-Until -Because 'a new desktop to appear at the tail' -Condition {
                (Get-VdState).Count -eq $before.Count + 1
            }
            $after = Get-VdState

            # ── Assert: on the delta, never an absolute ──────────────────────
            $after.Count       | Should -Be ($before.Count + 1)          # 3 -> 4
            $after.CurrentGuid  | Should -Be $after.Guids[-1]            # tail is current
            $after.CurrentGuid  | Should -Not -BeIn $before.Guids        # a genuinely new GUID
            # the pre-existing desktops are untouched (create appends, never reorders)
            for ($i = 0; $i -lt $before.Count; $i++) {
                $after.Guids[$i] | Should -Be $before.Guids[$i]
            }
        } catch {
            Save-FailureScreenshot -Name 'create-on-demand'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD step 1. The OS-state proof of the /SUBSYSTEM:WINDOWS + message-only-window
    # design: winspace must own no window the shell would surface.
    It 'windowless: winspace owns no visible top-level window (no taskbar / Alt-Tab entry)' -Tag 'windowless' {
        $winspace = $null
        try {
            Set-DesktopCount 1
            $winspace = Start-Winspace
            # EnumWindows-by-PID sees only genuine top-level windows; winspace's lone
            # window is HWND_MESSAGE (message-only), which is never enumerated — so a
            # correctly windowless winspace surfaces zero taskbar/Alt-Tab windows.
            $windows = Get-WinspaceWindows -ProcessId $winspace.Id
            $windows.Count | Should -Be 0 -Because 'winspace is windowless: no taskbar button, no Alt-Tab entry'
        } catch {
            Save-FailureScreenshot -Name 'windowless'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD step 2. Adoption binds the desktops that already exist to logical 1..N by
    # GUID; $mod+2 must land on the pre-existing 2nd desktop and spawn nothing.
    It 'adoption: $mod+2 focuses the pre-existing 2nd desktop and spawns nothing' -Tag 'adoption' {
        $winspace = $null
        try {
            Set-DesktopCount 3
            $staged = Get-VdState
            $secondGuid = $staged.Guids[1]         # the 2nd desktop that exists before launch

            $winspace = Start-Winspace
            $before = Get-VdState

            Send-Chord 'Alt+2'
            Wait-Until -Because 'current desktop to become the pre-existing 2nd' -Condition {
                (Get-VdState).CurrentGuid -eq $secondGuid
            }
            $after = Get-VdState

            $after.CurrentGuid | Should -Be $secondGuid       # bound to the existing GUID (adoption)
            $after.Count       | Should -Be $before.Count      # no new desktop spawned
            $after.Guids[1]    | Should -Be $secondGuid        # the 2nd slot itself is untouched
        } catch {
            Save-FailureScreenshot -Name 'adoption'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD step 4, restart-adoption proxy. NOTE — deliberate deviation from the
    # 12.03 ticket's literal "$mod+5 again". winspace never persists its
    # logical->GUID map (CONTEXT.md: State is rebuilt on start), and adoption
    # rebinds desktops to logical 1..N *positionally by GUID order* every launch
    # (src/io/vd_bridge.cpp adopt()). So after a restart the tail desktop we
    # created with $mod+5 is re-adopted at logical == Count, and pressing $mod+5
    # again would materialize a brand-new desktop — not resolve the old one. The
    # invariant PRD step 4 actually asserts (a materialized desktop's GUID identity
    # survives, and a logical number re-binds to it) is proven faithfully by
    # switching to that re-adopted logical number and getting the SAME GUID back.
    It 'guid-stability: a materialized desktop keeps its GUID across a winspace restart (restart-adoption proxy)' -Tag 'guid-stability' {
        $winspace = $null
        try {
            Set-DesktopCount 3
            $winspace = Start-Winspace

            # Materialize a new tail desktop ($mod+5 over 3 adopted) and record its GUID.
            $before = Get-VdState
            Send-Chord 'Alt+5'
            Wait-Until -Because 'a new tail desktop to be created' -Condition {
                (Get-VdState).Count -eq $before.Count + 1
            }
            $created   = Get-VdState
            $tailGuid  = $created.Guids[-1]
            $tailCount = $created.Count          # after re-adoption the tail sits at logical == Count

            # Quit + relaunch. winspace destroys no desktops on exit, so all persist;
            # adoption re-binds them by GUID (independent of winspace's own state).
            Stop-Winspace -Process $winspace
            Wait-Until -Because 'winspace to exit before relaunch' -Condition { $winspace.HasExited }
            $winspace = Start-Winspace

            $readopted = Get-VdState
            $readopted.Count | Should -Be $tailCount      # re-adopted, not duplicated
            $g1 = $readopted.Guids[0]

            # Switch away, then to the tail's re-adopted logical number: it must land
            # on the SAME physical desktop (same GUID) it had before the restart.
            Send-Chord 'Alt+1'
            Wait-Until -Because 'current desktop to become the 1st' -Condition {
                (Get-VdState).CurrentGuid -eq $g1
            }
            Send-Chord "Alt+$tailCount"
            Wait-Until -Because 'current desktop to become the re-adopted tail' -Condition {
                (Get-VdState).CurrentGuid -eq $tailGuid
            }
            $after = Get-VdState
            $after.CurrentGuid | Should -Be $tailGuid     # SAME GUID survives the restart
            $after.Count       | Should -Be $tailCount     # and no spurious desktop appeared
        } catch {
            Save-FailureScreenshot -Name 'guid-stability'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD step 4, literal variant. No synthesizable Trigger reorders desktops in
    # Task View — it is a mouse drag-drop against Explorer-owned UI, and the
    # registry GUID order is written by the shell, not by any hotkey. Proven by
    # manual smoke step 4; the restart-adoption proxy above exercises the same
    # GUID-anchoring invariant automatically.
    It 'guid-stability: literal Task-View reorder keeps the workspace-to-GUID binding' -Skip -Tag 'guid-stability' {
    }

    # PRD step 5. $mod SHIFT+Q -> the process exits cleanly; winspace reaps no desktops,
    # so it leaves exactly the desktops the test staged.
    It 'quit: $mod SHIFT+Q exits the process cleanly and orphans no desktops' -Tag 'quit' {
        $winspace = $null
        try {
            Set-DesktopCount 2
            $staged = Get-VdState
            $winspace = Start-Winspace
            $wsPid = $winspace.Id            # $PID is read-only (this runner's own id)

            Send-Chord 'Alt+Shift+Q'
            Wait-Until -Because 'the winspace process to exit' -Condition {
                $winspace.HasExited -or -not (Get-Process -Id $wsPid -ErrorAction SilentlyContinue)
            }
            Get-Process -Id $wsPid -ErrorAction SilentlyContinue | Should -BeNullOrEmpty

            (Get-VdState).Count | Should -Be $staged.Count   # no orphan desktops beyond those staged
        } catch {
            Save-FailureScreenshot -Name 'quit'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # PRD step 6. No OS-state observable exists for "which vtable did winspace
    # pick", so this seam asserts on the log Oracle (per ADR-0005): the resolved
    # 24H2 IID on a normal launch, and the loud "not yet implemented" line when a
    # stubbed variant is forced. The forced launch's bridge is null, so it never
    # logs 'adopted' — it is ready when its diagnostic line appears.
    It 'variant-diagnostic: the log names the resolved 24H2 IID, and a forced stub logs "not yet implemented"' -Tag 'variant-diagnostic' {
        $winspace = $null
        try {
            $winspace = Start-Winspace
            $log = Read-WinspaceLog -Text (Get-WinspaceLogText)
            $log.ResolvedVariantIID | Should -BeTrue -Because 'winspace must name the 24H2 IID it resolved'
            Stop-Winspace -Process $winspace
            $winspace = $null

            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_VARIANT = '23h2-kb' } `
                                       -ReadyPattern 'NOT YET IMPLEMENTED'
            $forced = Read-WinspaceLog -Text (Get-WinspaceLogText)
            $forced.ForcedVariantNotImplemented | Should -BeTrue -Because 'a forced stub variant must fail loudly'
        } catch {
            Save-FailureScreenshot -Name 'variant-diagnostic'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }
}
