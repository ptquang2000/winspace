<#
    Host-side unit tests for Read-WinspaceLog, the secondary (stderr) Oracle
    decoder (12.02). Pure text-in/object-out, so tested against captured run.log
    fixtures on the HOST with no VM.

    winspace is windowless, so its diagnostics only survive as captured stderr
    (winspace.exe 2> run.log). Each line is ANSI-coloured and tagged
    [INFO]/[WARN]/[ERROR] (src/io/error.cpp); the parser strips the colour and
    exposes the seam predicates the workspace-switch/error-handling tests assert on.
#>

BeforeAll {
    Import-Module (Join-Path $PSScriptRoot '..\guest\WinspaceTest.psm1') -Force
    $script:Fx = Join-Path $PSScriptRoot 'fixtures'
    $script:Good = Read-WinspaceLog -Text ([System.IO.File]::ReadAllText((Join-Path $Fx 'run.log')))
}

Describe 'Read-WinspaceLog' {

    It 'parses every [INFO]/[WARN]/[ERROR] line into a Level+Message record' {
        $Good.Records.Count | Should -Be 6
        @($Good.Records | Where-Object Level -EQ 'INFO').Count  | Should -Be 2
        @($Good.Records | Where-Object Level -EQ 'WARN').Count  | Should -Be 2
        @($Good.Records | Where-Object Level -EQ 'ERROR').Count | Should -Be 2
    }

    It 'strips the ANSI colour escapes from the message text' {
        $Good.Records[0].Message.Contains([char]27) | Should -BeFalse
        $Good.Records[0].Message | Should -BeLike 'virtual desktop bridge:*'
    }

    It 'recognises the "adopted N desktops" predicate and extracts the count' {
        $Good.AdoptedCount | Should -Be 3
    }

    It 'recognises the "resolved variant IID" predicate' {
        $Good.ResolvedVariantIID | Should -BeTrue
    }

    It 'recognises the "forced-variant not-yet-implemented" predicate' {
        $Good.ForcedVariantNotImplemented | Should -BeTrue
    }

    It 'recognises the "skip-and-log for hotkey" predicate' {
        $Good.HotkeySkipAndLog | Should -BeTrue
    }

    It 'accepts a well-formed formatError line (loc + hr=0x… / err=N + system text)' {
        $Good.HasQualityFormatError | Should -BeTrue
    }

    It 'reports a garbage/undocumented formatError line as NOT meeting the quality bar' {
        $bad = Read-WinspaceLog -Text ([System.IO.File]::ReadAllText((Join-Path $Fx 'formaterror-garbage.log')))
        # neither an unstructured line nor a loc+hex line lacking appended text qualifies
        $bad.HasQualityFormatError | Should -BeFalse
        # and the unrelated seam predicates stay negative on this capture
        $bad.AdoptedCount          | Should -BeNullOrEmpty
        $bad.ResolvedVariantIID    | Should -BeFalse
    }

    It 'is a total function on empty input' {
        $e = Read-WinspaceLog -Text ''
        $e.Records.Count         | Should -Be 0
        $e.AdoptedCount          | Should -BeNullOrEmpty
        $e.HasQualityFormatError | Should -BeFalse
    }
}
