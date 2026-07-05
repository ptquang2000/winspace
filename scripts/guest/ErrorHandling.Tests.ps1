<#
    Error-handling runtime seams (VM harness, ADR-0005) — the two *runtime* behaviors
    of the Win32 error-handling slice (docs/adr/0004-win32-error-handling.md) run unattended:
    formatError diagnostic quality and degrade-don't-crash.

    The error-handling slice's other gates — build-clean under /W4 /WX /permissive- and the pure-TU
    purity check (11.04 steps 1–2) — are compile/unit-time, so they stay on the host
    build.ps1 and are deliberately OUT OF SCOPE here (ADR-0005: only compiled Release
    runtime behavior crosses into the VM).

    Oracle policy is unchanged: OS state where an external observable exists (process
    liveness via Get-Process, VD deltas via Get-VdState), the winspace stderr log only
    where none does (the formatError text, the skip-and-log line) — parsed through the
    pure Read-WinspaceLog predicates from 12.02, never winspace's own say-so.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot 'WinspaceTest.psm1') -Force
    Assert-InteractiveSession   # loud gate before any Trigger reaches the desktop
}

Describe 'error-handling' {

    # 11.04 step 4. The formatError renderer (src/io/error.cpp std::formatter<Error>)
    # must emit <file>:<line> + the native code (hr=0x… / err=N) + FORMAT_MESSAGE text
    # when available, and NEVER garbage on an undocumented code (systemMessage empty →
    # loc+hex with no trailing text, which fails the quality bar rather than printing
    # junk).
    #
    # DELIBERATE DESIGN NOTE — there is no external lever that forces winspace's
    # ERROR-level formatError path in a VM: the forced-variant stub and a pre-registered
    # hotkey conflict BOTH degrade via WARN *by design* (that is the whole point of
    # degrade-don't-crash), and a genuine COM HrError (e.g. RPC_E_WRONG_THREAD) is not
    # reproducible on demand. So this seam (a) drives the real loud failure-handling
    # path with the forced stub and proves the live diagnostic stream is well-formed and
    # never garbage, and (b) asserts the quality predicate that grades that renderer is a
    # genuine discriminator — it accepts a well-formed formatError and rejects both an
    # unstructured line and a loc+hex line lacking system text. (The same positive/garbage
    # discrimination is host-unit-tested with NO VM in scripts/tests/LogParser.Tests.ps1;
    # this is its live counterpart, mirroring how the workspace-switch guid-stability seam proves
    # the invariant a fully-faithful Trigger can't reach.)
    It 'formaterror-quality: the diagnostic renderer emits loc + hex + system text and never garbage' -Tag 'formaterror-quality' {
        $winspace = $null
        try {
            # Drive the real loud path: a forced stub variant fails the COM bridge and
            # logs its diagnostic (null bridge → switches no-op; the process stays up).
            $winspace = Start-Winspace -Env @{ WINSPACE_FORCE_VD_VARIANT = '23h2-kb' } `
                                       -ReadyPattern 'NOT YET IMPLEMENTED'
            $log = Read-WinspaceLog -Text (Get-WinspaceLogText)

            # (a1) the loud diagnostic fired and the live stream parsed cleanly — every
            # emitted line matched the [LEVEL] grammar, i.e. nothing came out as garbage.
            $log.ForcedVariantNotImplemented | Should -BeTrue -Because 'a forced stub must fail loudly'
            $log.Records.Count | Should -BeGreaterThan 0

            # (a2) LIVE "never garbage": every ERROR the real binary logs meets the
            # formatError quality bar (loc + hr=0x…/err=N + appended system text). The
            # degrade path may log zero ERRORs (the failure is a WARN), so this is
            # vacuously safe today and bites the instant winspace ever emits a malformed
            # ERROR line — the regression this guard exists for.
            foreach ($rec in ($log.Records | Where-Object Level -EQ 'ERROR')) {
                (Read-WinspaceLog -Text "[ERROR] $($rec.Message)").HasQualityFormatError |
                    Should -BeTrue -Because "winspace must never log a garbage ERROR line: '$($rec.Message)'"
            }

            # (b) the quality predicate is a real bar, not a rubber stamp. The lines
            # below mirror src/io/error.cpp's formatter<Error> output shapes exactly.
            $genuine = '[ERROR] switchTo failed: vd_bridge.cpp:340 (hr=0x8001010E): ' +
                       'The application called an interface that was marshalled for a different thread.'
            $garbageUnstructured = '[ERROR] something went catastrophically wrong'
            $garbageNoSystemText = '[ERROR] bar.cpp:7 (hr=0x88990011)'   # undocumented code → no ": text"

            (Read-WinspaceLog -Text $genuine).HasQualityFormatError             | Should -BeTrue  -Because 'loc + hex + system text is the quality bar'
            (Read-WinspaceLog -Text $garbageUnstructured).HasQualityFormatError | Should -BeFalse -Because 'an unstructured line is not a formatError'
            (Read-WinspaceLog -Text $garbageNoSystemText).HasQualityFormatError | Should -BeFalse -Because 'loc+hex with no system text is rejected, never printed as garbage'
        } catch {
            Save-FailureScreenshot -Name 'formaterror-quality'
            throw
        } finally {
            Stop-Winspace -Process $winspace
        }
    }

    # 11.04 step 5. A hotkey already held by another app is skipped-and-logged (WARN)
    # while the remaining binds still register, and winspace stays alive — one failed
    # OS call must never take the process down. The conflict is genuine: a helper
    # process holds Win+1 (winspace's first seeded bind) via RegisterHotKey before
    # winspace launches, so winspace's own RegisterHotKey for it returns
    # ERROR_HOTKEY_ALREADY_REGISTERED — the real trigger, not a simulated one.
    It 'degrade-dont-crash: a pre-registered hotkey is skipped-and-logged, the rest bind, the process survives' -Tag 'degrade-dont-crash' {
        $winspace = $null
        $conflict = $null
        try {
            # ── Arrange: stage 3 desktops (current lands on the 3rd), then pre-seize
            # Win+1 from another process so winspace collides on that one bind.
            Set-DesktopCount 3
            $conflict = Register-ConflictingHotkey     # holds Win+1
            $winspace = Start-Winspace                 # adopts the 3 desktops; bind 1 will collide

            # ── Assert 1: the collision was skipped-and-logged (log Oracle — no OS-state
            # observable for "which binds registered").
            $log = Read-WinspaceLog -Text (Get-WinspaceLogText)
            $log.HotkeySkipAndLog | Should -BeTrue -Because 'the held hotkey must be skipped-and-logged, not fatal'

            # ── Assert 2: the REST bound — a non-conflicting bind still switches. Current
            # is the 3rd desktop after staging; Win+2 (bind 2, not held) must switch
            # to the 2nd. This is OS-state truth via the registry, not the log.
            $before = Get-VdState
            $secondGuid = $before.Guids[1]
            $before.CurrentGuid | Should -Not -Be $secondGuid -Because 'staging leaves us off the 2nd desktop'
            Send-Chord 'Win+2'
            Wait-Until -Because 'Win+2 (a non-conflicting bind) to switch to the 2nd desktop' -Condition {
                (Get-VdState).CurrentGuid -eq $secondGuid
            }
            (Get-VdState).CurrentGuid | Should -Be $secondGuid

            # ── Assert 3: the process survived the failed registration.
            $winspace.HasExited | Should -BeFalse -Because 'a single failed OS call must not crash winspace'
            Get-Process -Id $winspace.Id -ErrorAction SilentlyContinue | Should -Not -BeNullOrEmpty
        } catch {
            Save-FailureScreenshot -Name 'degrade-dont-crash'
            throw
        } finally {
            Stop-Winspace -Process $winspace
            if ($conflict -and -not $conflict.HasExited) {
                Stop-Process -Id $conflict.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }
}
