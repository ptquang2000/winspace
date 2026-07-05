<#
.SYNOPSIS
    Guest runner for the winspace VM seam-test harness (ADR-0005).

.DESCRIPTION
    Runs INSIDE the guest, in the one interactive logged-in session that the host
    orchestrator opens with `vmctl exec -it`. It owns the three things that only
    work in that session:

      * Trigger  — Send-Chord synthesizes a genuine modifier chord via
                   user32!SendInput (the real WM_HOTKEY path, Win key intact —
                   VMware MKS can't hold Super, which is why we inject in-guest).
      * Oracle   — Get-VdState reads Virtual Desktop state (count + current GUID)
                   from the registry: independent OS truth, never winspace's own
                   word. (The decoders live inline here for the walking skeleton;
                   12.02 factors them into pure, host-unit-tested functions.)
      * Report   — Invoke-WinspaceSeams runs the Pester seam files and emits
                   JUnit XML the host copies back.

    Cross-cutting spine concerns every later seam inherits: a -Seam tag filter,
    poll-the-Oracle-until-condition (never a fixed sleep) after each Trigger, and
    a screenshot artifact on seam failure.

    This module makes NO winspace source changes and adds no test hook to it — it
    drives the shipping Release binary exactly as a physical keypress would.
#>

Set-StrictMode -Version Latest

# ── P/Invoke: SendInput + the interactive-desktop gate ───────────────────────
# INPUT must marshal to the OS's exact sizeof(INPUT) (40 bytes on x64) or
# SendInput rejects cbSize and silently injects nothing — hence the full union
# with MOUSEINPUT (the largest arm), and cbSize via Marshal.SizeOf so it is
# architecture-correct.
if (-not ([System.Management.Automation.PSTypeName]'Winspace.Native').Type) {
    Add-Type -Namespace 'Winspace' -Name 'Native' -MemberDefinition @'
    public const int INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData;
        public uint dwFlags; public uint time; public System.IntPtr dwExtraInfo;
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public System.IntPtr dwExtraInfo;
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit)]
    public struct INPUTUNION {
        [System.Runtime.InteropServices.FieldOffset(0)] public MOUSEINPUT mi;
        [System.Runtime.InteropServices.FieldOffset(0)] public KEYBDINPUT ki;
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct INPUT { public int type; public INPUTUNION u; }

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);

    // OpenInputDesktop succeeds only from a process in WinSta0 with a live input
    // desktop — i.e. a real interactive session. It is the loud autologon gate.
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern System.IntPtr OpenInputDesktop(uint dwFlags, bool fInherit, uint dwDesiredAccess);
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern bool CloseDesktop(System.IntPtr hDesktop);

    // ── EnumWindows Oracle: top-level windows owned by a PID ─────────────────
    // The windowless seam proves winspace surfaces no taskbar / Alt-Tab entry.
    // EnumWindows walks only genuine top-level windows (a HWND_MESSAGE window is
    // message-only, not a child of the desktop, so it is never enumerated) — so
    // for a correctly windowless winspace this returns an empty array.
    public delegate bool EnumProc(System.IntPtr hWnd, System.IntPtr lParam);
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern bool EnumWindows(EnumProc lpEnumFunc, System.IntPtr lParam);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool IsWindowVisible(System.IntPtr hWnd);
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint lpdwProcessId);
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern int GetWindowLongW(System.IntPtr hWnd, int nIndex);

    // Visible, PID-owned, top-level windows that would appear in the taskbar /
    // Alt-Tab: a window counts if it is visible AND either sets WS_EX_APPWINDOW or
    // is not a WS_EX_TOOLWINDOW (the shell's own taskbar-presence rule). Returns
    // the handles; an empty array means "no user-facing window".
    public static System.IntPtr[] TaskbarWindowsForPid(uint pid) {
        const int GWL_EXSTYLE = -20;
        const int WS_EX_TOOLWINDOW = 0x00000080;
        const int WS_EX_APPWINDOW = 0x00040000;
        var found = new System.Collections.Generic.List<System.IntPtr>();
        EnumWindows((h, l) => {
            uint wpid;
            GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            if (!IsWindowVisible(h)) return true;
            int ex = GetWindowLongW(h, GWL_EXSTYLE);
            bool taskbarish = (ex & WS_EX_APPWINDOW) != 0 || (ex & WS_EX_TOOLWINDOW) == 0;
            if (taskbarish) found.Add(h);
            return true;
        }, System.IntPtr.Zero);
        return found.ToArray();
    }
'@
}

# ── deploy layout ────────────────────────────────────────────────────────────
# The host deploys everything under one root; WINSPACE_E2E_ROOT overrides it.
function Get-WinspaceRoot {
    if ($env:WINSPACE_E2E_ROOT) { return $env:WINSPACE_E2E_ROOT }
    return 'C:\winspace-e2e'
}
function Get-WinspaceExe { Join-Path (Get-WinspaceRoot) 'winspace.exe' }
function Get-WinspaceLog { Join-Path (Get-WinspaceRoot) 'run.log' }

# ── loud gate: an interactive session must exist (autologon on) ──────────────
# Without it every SendInput Trigger reaches no desktop and seams fail silently;
# fail here instead, naming the cause. See scripts/PROVISIONING.md.
function Assert-InteractiveSession {
    if (-not [Environment]::UserInteractive) {
        throw 'winspace-e2e: no interactive session ([Environment]::UserInteractive is false) — ' +
              'the VM likely booted to the lock screen. Enable autologon on the winspace-e2e snapshot ' +
              '(scripts/PROVISIONING.md).'
    }
    $desk = [Winspace.Native]::OpenInputDesktop(0x0001, $false, 0x0001)  # DF_ALLOWOTHERACCOUNTHOOK-free read
    if ($desk -eq [IntPtr]::Zero) {
        throw 'winspace-e2e: OpenInputDesktop failed — no interactive input desktop is attached. ' +
              'SendInput has nowhere to inject. Confirm autologon and that this runs via `vmctl exec -it` ' +
              '(scripts/PROVISIONING.md).'
    }
    [void][Winspace.Native]::CloseDesktop($desk)
}

# ── Trigger: synthesize a real modifier chord via SendInput ──────────────────
# Chord tokens are '+'-joined: modifiers (Win/Super, Alt, Ctrl, Shift) then one
# key (a digit, a letter, or F1-F24). Keydowns are sent in order, then keyups in
# reverse, in a single atomic SendInput batch — exactly how the OS sees a real
# press, so RegisterHotKey (winspace) and the native VD hotkeys both fire.
$script:ModVk = @{ 'WIN' = 0x5B; 'SUPER' = 0x5B; 'ALT' = 0x12; 'CTRL' = 0x11; 'CONTROL' = 0x11; 'SHIFT' = 0x10 }

function Resolve-ChordVk {
    param([string]$Token)
    $u = $Token.ToUpperInvariant()
    if ($script:ModVk.ContainsKey($u)) { return $script:ModVk[$u] }
    if ($u.Length -eq 1) {
        $c = $u[0]
        # '0'..'9' and 'A'..'Z' virtual-key codes equal their ASCII code points.
        if ($c -ge '0' -and $c -le '9') { return [int][char]$c }
        if ($c -ge 'A' -and $c -le 'Z') { return [int][char]$c }
    }
    if ($u -match '^F([1-9]|1[0-9]|2[0-4])$') { return 0x70 + [int]$Matches[1] - 1 }  # VK_F1 = 0x70
    throw "Send-Chord: unrecognized chord token '$Token'"
}

function Send-Chord {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Chord)

    $vks = $Chord.Split('+') | ForEach-Object { Resolve-ChordVk $_.Trim() }

    $inputs = [System.Collections.Generic.List[Winspace.Native+INPUT]]::new()
    # Build bottom-up with whole-struct assignments: a nested write like
    # $i.u.ki.wVk = x mutates a throwaway COPY of the value-type field and is lost,
    # so each level is assigned as a complete struct instead.
    $make = {
        param([int]$vk, [uint32]$flags)
        $ki = [Winspace.Native+KEYBDINPUT]::new()
        $ki.wVk = [uint16]$vk
        $ki.dwFlags = $flags
        $u = [Winspace.Native+INPUTUNION]::new()
        $u.ki = $ki
        $i = [Winspace.Native+INPUT]::new()
        $i.type = [Winspace.Native]::INPUT_KEYBOARD
        $i.u = $u
        $i
    }
    foreach ($vk in $vks) { $inputs.Add((& $make $vk 0)) }                                # keydowns, in order
    for ($j = $vks.Count - 1; $j -ge 0; $j--) {                                            # keyups, reversed
        $inputs.Add((& $make $vks[$j] ([Winspace.Native]::KEYEVENTF_KEYUP)))
    }

    $arr = $inputs.ToArray()
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type]([Winspace.Native+INPUT]))
    $sent = [Winspace.Native]::SendInput([uint32]$arr.Length, $arr, $size)
    if ($sent -ne $arr.Length) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Send-Chord '$Chord': SendInput injected $sent of $($arr.Length) events (GetLastError=$err)"
    }
}

# ── Oracle decoders: the harness's one pure seam (12.02) ─────────────────────
# VirtualDesktopIDs is a REG_BINARY packed array of 16-byte GUIDs (count =
# len/16); CurrentVirtualDesktop is one 16-byte GUID. Windows packs GUIDs in the
# same mixed-endian layout [guid]::new([byte[]]) expects, so the decode is direct
# and the current GUID compares equal to one of the list.
#
# These three decoders (VirtualDesktopIDs, current-GUID, Read-WinspaceLog) are the
# only logically-fallible logic in the harness, so — mirroring winspace's own
# pure-core-tested-with-zero-deps split (issues/README.md) — they are pure
# functions (bytes/text in, objects out; no registry, process, or VM access
# inside them) and are host-unit-tested against captured fixtures with no VM
# (scripts/tests). Get-VdState below composes them over the live registry.
$script:VdKey = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops'

function ConvertFrom-VirtualDesktopIDs {
    param([byte[]]$Bytes)
    $guids = @()
    if ($Bytes) {
        for ($o = 0; $o + 16 -le $Bytes.Length; $o += 16) {
            # cast the slice: $Bytes[a..b] yields object[], which binds [guid]'s
            # string ctor and throws — [byte[]] forces the 16-byte-array ctor.
            $guids += [guid]::new([byte[]]($Bytes[$o..($o + 15)]))
        }
    }
    [pscustomobject]@{ Count = $guids.Count; Guids = @($guids) }
}

# CurrentVirtualDesktop is a single packed GUID. Anything shorter than 16 bytes
# (absent/truncated value) decodes to Guid.Empty so callers get a total function
# rather than an exception on a cold registry.
function ConvertFrom-CurrentVirtualDesktop {
    param([byte[]]$Bytes)
    if ($Bytes -and $Bytes.Length -ge 16) {
        return [guid]::new([byte[]]($Bytes[0..15]))
    }
    return [guid]::Empty
}

# Parse a captured winspace stderr string into structured records plus the seam
# predicates the workspace-switch/error-handling tests consult. winspace tags each line with an
# ANSI-coloured [INFO]/[WARN]/[ERROR] (src/io/error.cpp), so the colour codes are
# stripped before the level+message split. Text in, object out — no file, process,
# or VM access — so it is unit-testable host-side against a captured run.log.
function Read-WinspaceLog {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $ansi   = [regex]'\x1b\[[0-9;]*m'                  # SGR colour escapes
    $lineRe = [regex]'^\[(INFO|WARN|ERROR)\]\s+(.*)$'

    $records = [System.Collections.Generic.List[psobject]]::new()
    foreach ($raw in ($Text -split "\r?\n")) {
        $clean = $ansi.Replace($raw, '').Trim()
        $m = $lineRe.Match($clean)
        if ($m.Success) {
            $records.Add([pscustomobject]@{ Level = $m.Groups[1].Value; Message = $m.Groups[2].Value })
        }
    }
    $msgs = @($records | ForEach-Object { $_.Message })

    # adoption line: "… adopted N desktop(s); current workspace = …" → the count.
    $adopted = $null
    foreach ($msg in $msgs) {
        $am = [regex]::Match($msg, 'adopted\s+(\d+)\s+desktop')
        if ($am.Success) { $adopted = [int]$am.Groups[1].Value; break }
    }

    # formatError quality: loc (<file>:<line>) + native code (hr=0xXXXXXXXX / err=N)
    # + appended FORMAT_MESSAGE text. An undocumented code renders loc+hex with NO
    # trailing ": <text>" (systemMessage returned empty), so it fails this bar —
    # exactly the "never print garbage" guarantee the seam asserts.
    $qualityRe = [regex]'\S+:\d+ \((?:hr=0x[0-9A-Fa-f]{8}|err=\d+)\): .+'

    [pscustomobject]@{
        PSTypeName                  = 'Winspace.LogParse'
        Records                     = $records.ToArray()
        AdoptedCount                = $adopted
        ResolvedVariantIID          = [bool]($msgs -match 'matched IID \{[0-9A-Fa-f-]+\}.*variant')
        ForcedVariantNotImplemented = [bool]($msgs -match 'NOT YET IMPLEMENTED')
        HasQualityFormatError       = [bool]@($msgs | Where-Object { $qualityRe.IsMatch($_) })
        HotkeySkipAndLog            = [bool]($msgs -match 'is already registered by another app.*skipping')
    }
}

# Strict-mode-safe registry read: the value's data, or $null if the key or the
# value is absent. A cold single-desktop machine — freshly booted, Task View never
# opened — has not persisted VirtualDesktopIDs/CurrentVirtualDesktop yet, so these
# values are legitimately missing. Under Set-StrictMode -Version Latest, dotting a
# property the object does not carry is a *terminating* error, so probe the property
# set with PSObject.Properties instead of accessing the name directly.
function Get-RegValue {
    param([string]$Path, [string]$Name)
    $item = Get-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue
    if ($item -and ($item.PSObject.Properties.Name -contains $Name)) { return $item.$Name }
    return $null
}

function Get-VdState {
    $ids = Get-RegValue -Path $script:VdKey -Name 'VirtualDesktopIDs'
    $decoded = ConvertFrom-VirtualDesktopIDs -Bytes $ids

    # CurrentVirtualDesktop lives at the top level on 24H2; some builds keep it
    # only under SessionInfo\<n>\. Try the top level, then fall back.
    $curBytes = Get-RegValue -Path $script:VdKey -Name 'CurrentVirtualDesktop'
    if (-not $curBytes) {
        $curBytes = Get-ChildItem -Path (Join-Path $script:VdKey 'SessionInfo') -ErrorAction SilentlyContinue |
            ForEach-Object { Get-RegValue -Path (Join-Path $_.PSPath 'VirtualDesktops') -Name 'CurrentVirtualDesktop' } |
            Where-Object { $_ } | Select-Object -First 1
    }
    $current = ConvertFrom-CurrentVirtualDesktop -Bytes $curBytes

    # A cold registry (no persisted GUID list) still means exactly one desktop, so
    # floor the count at 1 — otherwise Set-DesktopCount's 1->N increment math would
    # target Count 1 after the first Win+Ctrl+D (which actually yields 2) and hang.
    $count = if ($decoded.Count -gt 0) { $decoded.Count } else { 1 }
    [pscustomobject]@{
        Count       = $count
        CurrentGuid = $current
        Guids       = $decoded.Guids
    }
}

# ── Oracle: winspace's visible top-level windows (windowless seam) ────────────
# Independent OS truth for "does winspace show a taskbar / Alt-Tab entry": the
# shell's own taskbar-presence rule applied to every top-level window owned by
# the winspace PID. An empty array is the pass — winspace is /SUBSYSTEM:WINDOWS
# with only a message-only window, which EnumWindows never reports.
function Get-WinspaceWindows {
    [CmdletBinding()]
    param([Parameter(Mandatory)][int]$ProcessId)
    # Unary comma: a function returning a bare @() unwraps to $null on the way out,
    # and the caller's $windows.Count then throws under Set-StrictMode -Version Latest
    # — which is exactly the windowless PASS case (zero taskbar windows). Wrapping in
    # ,(...) returns the array itself, so .Count is 0 rather than a missing property.
    return , @([Winspace.Native]::TaskbarWindowsForPid([uint32]$ProcessId))
}

# ── per-seam precondition: drive the OS to exactly N Virtual Desktops ─────────
# Uses ONLY the native VD hotkeys — Win+Ctrl+F4 closes the current desktop,
# Win+Ctrl+D creates+switches — so each seam stages its own baseline from
# whatever the previous seam left behind, and the suite passes regardless of run
# order. Run BEFORE Start-Winspace so this is pure OS state that adoption then
# inherits. Polls the Oracle up/down after every chord rather than sleeping.
function Set-DesktopCount {
    [CmdletBinding()]
    param([Parameter(Mandatory)][int]$Count)
    if ($Count -lt 1) { throw "Set-DesktopCount: target must be >= 1 (got $Count)" }
    while ((Get-VdState).Count -gt 1) {                    # tear down to a single desktop
        $before = (Get-VdState).Count
        Send-Chord 'Win+Ctrl+F4'
        Wait-Until -Because "desktop count to fall to $($before - 1)" -Condition {
            (Get-VdState).Count -eq $before - 1
        }
    }
    while ((Get-VdState).Count -lt $Count) {               # then build up to the target
        $before = (Get-VdState).Count
        Send-Chord 'Win+Ctrl+D'
        Wait-Until -Because "desktop count to reach $($before + 1)" -Condition {
            (Get-VdState).Count -eq $before + 1
        }
    }
}

# ── poll, never sleep: wait for the Oracle to reach a condition ──────────────
# The interval below is a poll cadence, not a fixed settle time — it returns the
# instant the condition holds, and fails loud at the timeout.
function Wait-Until {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][scriptblock]$Condition,
        [int]$TimeoutSec = 8,
        [int]$PollMs = 150,
        [string]$Because = 'condition'
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds $PollMs
    } while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec)
    throw "Wait-Until: timed out after ${TimeoutSec}s waiting for $Because."
}

# ── launch / stop winspace with stderr captured ──────────────────────────────
# winspace is windowless (/SUBSYSTEM:WINDOWS), so its lg:: diagnostics only
# survive if stderr is redirected — `winspace.exe 2> run.log` (ADR-0005).
function Start-Winspace {
    [CmdletBinding()]
    param(
        [string]$Exe = (Get-WinspaceExe),
        [string]$LogPath = (Get-WinspaceLog),
        # Environment overrides the child snapshots at spawn (e.g.
        # WINSPACE_FORCE_VD_VARIANT). Set on this process only long enough for
        # Start-Process to inherit them, then restored so no other seam is affected.
        [hashtable]$Env,
        # The log line that means "ready". Default: adoption ran (24H2 bridge up).
        # A forced stub variant NEVER logs 'adopted' (its bridge is null), so those
        # launches wait on their loud diagnostic instead.
        [string]$ReadyPattern = 'adopted',
        [int]$ReadyTimeoutSec = 10
    )
    if (Test-Path $LogPath) { Remove-Item $LogPath -Force }

    $restore = @{}; $added = @()
    if ($Env) {
        foreach ($k in $Env.Keys) {
            if (Test-Path "env:$k") { $restore[$k] = (Get-Item "env:$k").Value } else { $added += $k }
            Set-Item "env:$k" $Env[$k]
        }
    }
    try {
        $proc = Start-Process -FilePath $Exe -RedirectStandardError $LogPath -PassThru
    } finally {
        # Start-Process has already snapshotted the env into the child, so undo the
        # override on this process immediately — the child keeps its copy.
        foreach ($k in $restore.Keys) { Set-Item "env:$k" $restore[$k] }
        foreach ($k in $added)        { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    }

    Wait-Until -TimeoutSec $ReadyTimeoutSec -Because "winspace ready pattern '$ReadyPattern' in $LogPath" -Condition {
        (Get-WinspaceLogText -LogPath $LogPath) -match $ReadyPattern
    }
    return $proc
}

function Stop-Winspace {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Process)
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
}

# Read run.log while winspace still holds it open for writing (share read+write
# so the concurrent reader never trips a sharing violation).
function Get-WinspaceLogText {
    param([string]$LogPath = (Get-WinspaceLog))
    if (-not (Test-Path $LogPath)) { return '' }
    $fs = [System.IO.File]::Open($LogPath, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try { [System.IO.StreamReader]::new($fs).ReadToEnd() } finally { $fs.Dispose() }
}

# ── failure artifact: screenshot of the interactive desktop ──────────────────
# Captured in-guest (self-contained, no host round-trip); the host copies the
# PNG back with the rest of the artifacts. Best-effort — never masks the seam
# failure it is documenting.
function Save-FailureScreenshot {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Name)
    try {
        Add-Type -AssemblyName System.Drawing, System.Windows.Forms
        $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.X, $b.Y, 0, 0, $b.Size)
        $path = Join-Path (Get-WinspaceRoot) ("failure-{0}.png" -f ($Name -replace '[^\w.-]', '_'))
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        Write-Warning "seam failure screenshot: $path"
    } catch {
        Write-Warning "Save-FailureScreenshot: could not capture ($_)"
    }
}

# ── degrade-don't-crash lever: hold a winspace hotkey from another process ────
# Pre-registers a global hotkey in a SEPARATE process so that when winspace later
# RegisterHotKey's the same combo it gets ERROR_HOTKEY_ALREADY_REGISTERED — the
# genuine, deterministic trigger for the skip-and-log degrade path (hotkeys.cpp).
# The default combo is Win+1 (MOD_NOREPEAT|MOD_WIN + '1'), i.e. exactly
# winspace's first seeded bind (src/io/app.cpp $mod = SUPER), so the collision is
# real. (Win+1 only registers because NoWinKeys=1 is baked into the snapshot; without
# that policy the OS owns Win+1 and BOTH the helper and winspace would fail to take it.)
# RegisterHotKey binds to
# the calling thread, so the helper simply holds it and sleeps; the registration
# lives as long as the process does. Returns the helper Process (Stop-Process to
# release). Fails loud if the helper cannot take the combo (nothing to conflict
# with = a meaningless seam).
function Register-ConflictingHotkey {
    [CmdletBinding()]
    param(
        [uint32]$Modifiers = 0x4008,   # MOD_NOREPEAT(0x4000)|MOD_WIN(0x0008)
        [uint32]$Vk = 0x31,            # '1'
        [int]$ReadyTimeoutSec = 8
    )
    $readyFile = Join-Path (Get-WinspaceRoot) 'conflict.ready'
    if (Test-Path $readyFile) { Remove-Item $readyFile -Force }

    # Interpolate the combo + ready-file path into the helper; `$-escape the tokens
    # that must survive to the child (its own $true / $e), keep the C# in a literal
    # here-string. Shipped via -EncodedCommand so no quoting crosses the boundary.
    $helper = @"
`$ErrorActionPreference = 'Stop'
Add-Type -Namespace CH -Name Keys -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern bool RegisterHotKey(System.IntPtr hWnd, int id, uint fsModifiers, uint vk);
'@
if ([CH.Keys]::RegisterHotKey([System.IntPtr]::Zero, 1, [uint32]$Modifiers, [uint32]$Vk)) {
    Set-Content -LiteralPath '$readyFile' -Value 'ok'
    while (`$true) { Start-Sleep -Seconds 3600 }
} else {
    `$e = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Set-Content -LiteralPath '$readyFile' -Value ('fail:' + `$e)
}
"@
    $enc = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($helper))
    $proc = Start-Process -FilePath 'powershell.exe' -PassThru -WindowStyle Hidden `
        -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $enc

    try {
        Wait-Until -TimeoutSec $ReadyTimeoutSec -Because 'the conflicting-hotkey helper to hold the combo' -Condition {
            Test-Path $readyFile
        }
    } catch {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw
    }
    $status = (Get-Content -LiteralPath $readyFile -Raw).Trim()
    if ($status -ne 'ok') {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "Register-ConflictingHotkey: helper could not take the combo ($status) — nothing for winspace to collide with."
    }
    return $proc
}

# ── Fresh-mode discovery: the live (non-skipped) seam tags ────────────────────
# The host -Fresh loop reverts the snapshot once per LIVE seam, so it must know the
# seam set. Discover it here (Pester discovery only, SkipRun) rather than hardcode
# it host-side, so a newly-dropped seam file is picked up with no orchestrator edit.
# The 'scaffold' tag (issues 02–10, all -Skip) is excluded — those need no VM and
# are collected in a single non-reverting pass. Emits a `TAGS:a,b,c` line the host
# greps out of the exec stdout.
function Get-WinspaceLiveSeamTags {
    [CmdletBinding()]
    param([string]$TestPath = $PSScriptRoot)
    Import-Module Pester -MinimumVersion 5.0 -ErrorAction Stop
    $cfg = New-PesterConfiguration
    $cfg.Run.Path = $TestPath
    $cfg.Run.SkipRun = $true          # discovery only — do not launch winspace
    $cfg.Run.PassThru = $true
    $cfg.Output.Verbosity = 'None'
    $r = Invoke-Pester -Configuration $cfg
    $tags = [System.Collections.Generic.List[string]]::new()
    foreach ($t in $r.Tests) {
        if ($t.Skip) { continue }
        foreach ($tag in $t.Tag) {
            if ($tag -ne 'scaffold' -and -not $tags.Contains($tag)) { $tags.Add($tag) }
        }
    }
    return @($tags)
}

# ── the Guest runner entry point ─────────────────────────────────────────────
# Gates on an interactive session (loud), then runs the seam Pester files with an
# optional -Seam tag filter and emits JUnit XML. Exits with the failed-test count
# so `vmctl exec -it` propagates a non-zero code to the host on failure.
function Invoke-WinspaceSeams {
    [CmdletBinding()]
    param(
        [string]$TestPath = $PSScriptRoot,
        [string]$ResultsPath = (Join-Path (Get-WinspaceRoot) 'results.xml'),
        [string]$Seam
    )
    Assert-InteractiveSession
    Import-Module Pester -MinimumVersion 5.0 -ErrorAction Stop

    $cfg = New-PesterConfiguration
    $cfg.Run.Path = $TestPath
    $cfg.Run.PassThru = $true
    $cfg.Output.Verbosity = 'Detailed'
    $cfg.TestResult.Enabled = $true
    $cfg.TestResult.OutputFormat = 'JUnitXml'
    $cfg.TestResult.OutputPath = $ResultsPath
    if ($Seam) { $cfg.Filter.Tag = $Seam }

    $result = Invoke-Pester -Configuration $cfg
    exit [int]$result.FailedCount
}

Export-ModuleMember -Function Assert-InteractiveSession, Send-Chord, Get-VdState,
    Get-WinspaceWindows, Set-DesktopCount,
    ConvertFrom-VirtualDesktopIDs, ConvertFrom-CurrentVirtualDesktop, Read-WinspaceLog,
    Wait-Until, Start-Winspace, Stop-Winspace, Register-ConflictingHotkey,
    Get-WinspaceLogText, Save-FailureScreenshot, Invoke-WinspaceSeams, Get-WinspaceLiveSeamTags,
    Get-WinspaceRoot, Get-WinspaceExe, Get-WinspaceLog
