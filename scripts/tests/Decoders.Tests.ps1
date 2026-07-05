<#
    Host-side unit tests for the pure registry Oracle decoders (12.02).

    These run on the HOST with NO VM and no VMware — the decoders are pure
    (bytes in, objects out), so they are exercised against captured REG_BINARY
    fixtures under ./fixtures. This mirrors how winspace unit-tests its pure core
    with zero Windows deps (issues/README.md); the VM-driving layer is verified
    separately by a live run.

    Entry point: a plain `Invoke-Pester scripts/tests` — no harness, no VM.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\guest\WinspaceTest.psm1') -Force
    $script:Fx = Join-Path $PSScriptRoot 'fixtures'
    # The GUIDs the fixtures were built from (distinct + ordered).
    $script:G1 = [guid]'11111111-1111-1111-1111-111111111111'
    $script:G2 = [guid]'22222222-2222-2222-2222-222222222222'
    $script:G3 = [guid]'33333333-3333-3333-3333-333333333333'
}

Describe 'ConvertFrom-VirtualDesktopIDs' {

    It 'decodes a known multi-desktop blob to count + ordered GUID list' {
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'VirtualDesktopIDs.multi.bin'))
        $r = ConvertFrom-VirtualDesktopIDs -Bytes $bytes
        $r.Count    | Should -Be 3
        $r.Guids[0] | Should -Be $G1
        $r.Guids[1] | Should -Be $G2
        $r.Guids[2] | Should -Be $G3
    }

    It 'decodes the single-desktop case' {
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'VirtualDesktopIDs.single.bin'))
        $r = ConvertFrom-VirtualDesktopIDs -Bytes $bytes
        $r.Count    | Should -Be 1
        $r.Guids[0] | Should -Be $G1
    }

    It 'yields count 0 for the empty blob' {
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'VirtualDesktopIDs.empty.bin'))
        (ConvertFrom-VirtualDesktopIDs -Bytes $bytes).Count | Should -Be 0
    }

    It 'yields count 0 for a null blob (cold registry value)' {
        (ConvertFrom-VirtualDesktopIDs -Bytes $null).Count | Should -Be 0
    }

    It 'ignores a trailing partial (non-multiple-of-16) tail' {
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'VirtualDesktopIDs.single.bin')) + [byte[]]@(1, 2, 3)
        (ConvertFrom-VirtualDesktopIDs -Bytes $bytes).Count | Should -Be 1
    }
}

Describe 'ConvertFrom-CurrentVirtualDesktop' {

    It 'decodes the current-desktop blob, and it equals a list entry (the adoption invariant)' {
        $curBytes  = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'CurrentVirtualDesktop.bin'))
        $listBytes = [System.IO.File]::ReadAllBytes((Join-Path $Fx 'VirtualDesktopIDs.multi.bin'))
        $current = ConvertFrom-CurrentVirtualDesktop -Bytes $curBytes
        $list    = ConvertFrom-VirtualDesktopIDs -Bytes $listBytes
        $current | Should -Be $G2
        $current | Should -BeIn $list.Guids
    }

    It 'decodes a truncated or absent blob to Guid.Empty' {
        (ConvertFrom-CurrentVirtualDesktop -Bytes ([byte[]]@(1, 2, 3))) | Should -Be ([guid]::Empty)
        (ConvertFrom-CurrentVirtualDesktop -Bytes $null)               | Should -Be ([guid]::Empty)
    }
}
