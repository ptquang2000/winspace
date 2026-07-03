<#
.SYNOPSIS
    Handmade build for winspace — invokes cl.exe directly. No CMake, no vcpkg,
    no package manager.

.DESCRIPTION
    Assumes an x64 Native Tools for VS command prompt (cl.exe already on PATH).
    Builds two unity translation units into build/<config>/ with an always-clean
    full rebuild:

      * app  (src/unity_app.cpp)  — core + I/O adapters + WinMain; links the WM
        import libraries; /SUBSYSTEM:WINDOWS (windowless) -> winspace.exe
      * test (src/unity_test.cpp) — core only + Catch2; links NO WM libraries;
        /SUBSYSTEM:CONSOLE -> winspace_tests.exe

    Both TUs compile on every run so a purity violation in core (an OS call that
    fails to link into the WM-library-free test TU) is caught immediately.

.PARAMETER Config
    debug (default) or release.

.PARAMETER Test
    After building, run winspace_tests.exe.

.PARAMETER Clean
    Delete build/ and exit.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Config release
    .\build.ps1 -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'debug',
    [switch]$Test,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Handle native-tool exit codes ourselves rather than letting PS7 throw on them.
if (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = $PSScriptRoot
$buildRoot = Join-Path $root 'build'
$buildDir = Join-Path $buildRoot $Config

function Write-Result {
    param([string]$Label, [bool]$Ok, [double]$Seconds, [string]$Note = '')
    $tag = if ($Ok) { 'PASS' } else { 'FAIL' }
    $color = if ($Ok) { 'Green' } else { 'Red' }
    $line = '  {0,-6} ' -f $Label
    Write-Host $line -NoNewline
    Write-Host $tag -ForegroundColor $color -NoNewline
    $suffix = '  {0,6:0.00}s' -f $Seconds
    if ($Note) { $suffix += "  $Note" }
    Write-Host $suffix
}

# ── -Clean: nuke build/ and exit ────────────────────────────────────────────
if ($Clean) {
    if (Test-Path $buildRoot) { Remove-Item -Recurse -Force $buildRoot }
    Write-Host 'cleaned build/' -ForegroundColor Green
    exit 0
}

# ── fail fast if cl is absent ───────────────────────────────────────────────
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Host 'error: cl.exe not found on PATH — run from an x64 Native Tools for VS command prompt.' -ForegroundColor Red
    exit 1
}

# ── flags ───────────────────────────────────────────────────────────────────
$common = @(
    '/nologo', '/std:c++latest', '/EHsc', '/W4', '/WX', '/permissive-', '/utf-8',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN',
    '/Ithird_party', '/Isrc'
)
if ($Config -eq 'debug') {
    $cfgCompile = @('/Od', '/Zi', '/RTC1', '/MDd')
    $cfgLink = @('/DEBUG')
}
else {
    $cfgCompile = @('/O2', '/DNDEBUG', '/MD')
    $cfgLink = @()
}

# WM import libraries — linked by the app TU only. The test TU links none of
# these, which is what turns any stray OS call in core into a link error.
$wmLibs = @('user32.lib', 'ole32.lib', 'oleaut32.lib', 'dwmapi.lib', 'shcore.lib')

# ── always-clean rebuild of this config ─────────────────────────────────────
if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

function Build-TU {
    param(
        [string]$Label,
        [string]$Source,
        [string[]]$Libs,
        [string]$Subsystem,
        [string]$OutExe
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $clArgs = @()
    $clArgs += $common
    $clArgs += $cfgCompile
    $clArgs += "/Fo$buildDir\"          # object output directory
    $clArgs += "/Fd$buildDir\$Label.pdb" # compiler pdb
    $clArgs += $Source
    $clArgs += '/link'
    $clArgs += $cfgLink
    $clArgs += "/SUBSYSTEM:$Subsystem"
    $clArgs += "/OUT:$OutExe"
    $clArgs += $Libs

    $output = & cl.exe @clArgs 2>&1
    $ok = ($LASTEXITCODE -eq 0)
    $sw.Stop()
    if (-not $ok) {
        Write-Host ''
        $output | ForEach-Object { Write-Host $_ }
        Write-Host ''
    }
    Write-Result -Label $Label -Ok $ok -Seconds $sw.Elapsed.TotalSeconds
    return $ok
}

$totalSw = [System.Diagnostics.Stopwatch]::StartNew()
Write-Host "winspace build — config: $Config"

$appExe = Join-Path $buildDir 'winspace.exe'
$testExe = Join-Path $buildDir 'winspace_tests.exe'

$appOk = Build-TU -Label 'app'  -Source 'src\unity_app.cpp'  -Libs $wmLibs -Subsystem 'WINDOWS' -OutExe $appExe
$testOk = Build-TU -Label 'test' -Source 'src\unity_test.cpp' -Libs @()     -Subsystem 'CONSOLE' -OutExe $testExe

$runOk = $true
if ($Test -and $testOk) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $testExe
    $runOk = ($LASTEXITCODE -eq 0)
    $sw.Stop()
    Write-Result -Label 'run' -Ok $runOk -Seconds $sw.Elapsed.TotalSeconds
}

$totalSw.Stop()
Write-Host ('total {0:0.00}s' -f $totalSw.Elapsed.TotalSeconds)

if ($appOk -and $testOk -and $runOk) { exit 0 } else { exit 1 }
