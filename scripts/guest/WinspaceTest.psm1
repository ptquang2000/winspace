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

    // ── geometry Oracle: raw rect, visible frame, and the monitor work area ──
    // The fill seams (02) assert winspace lands a window's VISIBLE frame flush on
    // the monitor work area. These read the same independent OS geometry winspace
    // itself positions against (GetWindowRect / DWMWA_EXTENDED_FRAME_BOUNDS /
    // GetMonitorInfo.rcWork) — never winspace's own report.
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct RECT { public int left; public int top; public int right; public int bottom; }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern bool GetWindowRect(System.IntPtr hWnd, out RECT lpRect);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern System.IntPtr MonitorFromWindow(System.IntPtr hWnd, uint dwFlags);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool GetMonitorInfoW(System.IntPtr hMonitor, ref MONITORINFO lpmi);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern System.IntPtr GetAncestor(System.IntPtr hWnd, uint gaFlags);
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(System.IntPtr hWnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);
    [System.Runtime.InteropServices.DllImport("dwmapi.dll", EntryPoint = "DwmGetWindowAttribute")]
    public static extern int DwmGetWindowAttributeI(System.IntPtr hWnd, int dwAttribute, out int pvAttribute, int cbAttribute);
    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetWindowPos(System.IntPtr hWnd, System.IntPtr hAfter, int x, int y, int cx, int cy, uint flags);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
    public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    public static extern int GetWindowTextW(System.IntPtr hWnd, System.Text.StringBuilder s, int max);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    public static extern int GetWindowTextLengthW(System.IntPtr hWnd);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(System.IntPtr value);

    // The current foreground window — the independent OS Oracle for the spatial-
    // focus seam (issue 05). winspace's SetForegroundWindow Effect must move THIS,
    // asserted as a before/after delta around the `focus` chord.
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    public static extern System.IntPtr GetForegroundWindow();

    private const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    private const int DWMWA_CLOAKED = 14;
    private const uint MONITOR_DEFAULTTONEAREST = 2;
    private const uint GA_ROOT = 2;

    // The full window rect (drop-shadow border included) — what winspace reads and
    // grows outward from, so a raw-rect compare proves an untouched window unmoved.
    public static RECT WindowRect(System.IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }

    // The VISIBLE frame (drop-shadow excluded) — the coordinate space winspace
    // compensates INTO, so a filled window's frame bounds equal the target rcWork.
    // Falls back to the raw window rect if DWM has no frame to report.
    public static RECT FrameBounds(System.IntPtr h) {
        RECT r;
        int cb = System.Runtime.InteropServices.Marshal.SizeOf(typeof(RECT));
        if (DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, out r, cb) == 0) return r;
        GetWindowRect(h, out r);
        return r;
    }

    // The work area (desktop minus taskbar) of the monitor the window sits on —
    // the same rcWork the pure Reducer resolves the head's fill against.
    public static RECT WorkAreaForWindow(System.IntPtr h) {
        var mi = new MONITORINFO();
        mi.cbSize = System.Runtime.InteropServices.Marshal.SizeOf(typeof(MONITORINFO));
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), ref mi);
        return mi.rcWork;
    }

    // DWM cloak state — how a UWP CoreWindow reads as hidden while WS_VISIBLE. The
    // cloaked-UWP seam asserts winspace excludes the cloaked host (no phantom tile).
    public static bool IsCloaked(System.IntPtr h) {
        int c = 0;
        DwmGetWindowAttributeI(h, DWMWA_CLOAKED, out c, sizeof(int));
        return c != 0;
    }

    public static string WindowTitle(System.IntPtr h) {
        int n = GetWindowTextLengthW(h);
        if (n <= 0) return "";
        var sb = new System.Text.StringBuilder(n + 1);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // Visible top-level windows whose title contains `sub` (case-insensitive) —
    // used to locate a UWP frame window (e.g. Calculator) by caption, since a Store
    // app's stub launcher owns no window and its real PID is unknowable up front.
    public static System.IntPtr[] FindVisibleWindowsByTitle(string sub) {
        var found = new System.Collections.Generic.List<System.IntPtr>();
        string want = sub.ToLowerInvariant();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            if (GetAncestor(h, GA_ROOT) != h) return true;             // top-level only
            if (WindowTitle(h).ToLowerInvariant().Contains(want)) found.Add(h);
            return true;
        }, System.IntPtr.Zero);
        return found.ToArray();
    }

    // Best-effort: make THIS runner process Per-Monitor-V2 DPI aware so its
    // GetWindowRect / GetMonitorInfo reads share the physical-pixel space winspace
    // (also PMv2) positions in. No-op / harmless if already set or unsupported.
    public static void MakeDpiAware() {
        try { SetProcessDpiAwarenessContext((System.IntPtr)(-4)); } catch { }
    }
'@
}

# Match winspace's physical-pixel coordinate space before any geometry read.
[Winspace.Native]::MakeDpiAware()

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

# ── hide/show the runner's OWN console (the vmctl exec -it window) ─────────────
# That console is a real WS_THICKFRAME|WS_CAPTION top-level window, i.e. exactly
# what winspace's Eligibility gate calls Tileable — so if it is visible when a
# window-tracking seam runs, winspace adopts it and it competes for the head-fill,
# stealing the fill from (or reordering) the test form the seam actually asserts on
# (fatal for the adoption seam, whose foreground form would otherwise be the head).
# The window-tracking Describe hides it for the duration and restores it after, so
# the test forms are the only Tileable windows on the desktop. SendInput and the
# interactive-desktop gate are unaffected by a hidden console window.
function Set-RunnerConsoleVisible {
    [CmdletBinding()]
    param([Parameter(Mandatory)][bool]$Visible)
    if (-not ([System.Management.Automation.PSTypeName]'Winspace.Console').Type) {
        Add-Type -Namespace 'Winspace' -Name 'Console' -MemberDefinition @'
    [System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern System.IntPtr GetConsoleWindow();
    [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int n);
'@
    }
    $h = [Winspace.Console]::GetConsoleWindow()
    if ($h -ne [IntPtr]::Zero) {
        [void][Winspace.Console]::ShowWindow($h, ($(if ($Visible) { 5 } else { 0 })))  # SW_SHOW / SW_HIDE
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

# ── geometry Oracle wrappers (02 fill seams) ─────────────────────────────────
# Thin PowerShell faces over the [Winspace.Native] statics: each returns a RECT
# value type so callers compare fields directly. All read independent OS geometry,
# never winspace's own log.
# The current foreground window (the spatial-focus Oracle). Returns the top-level
# HWND that owns the keyboard — compared against a test window's Hwnd as a delta.
function Get-ForegroundWindow { [Winspace.Native]::GetForegroundWindow() }
function Get-WindowRect  { param([Parameter(Mandatory)][IntPtr]$Hwnd) [Winspace.Native]::WindowRect($Hwnd) }
function Get-FrameBounds { param([Parameter(Mandatory)][IntPtr]$Hwnd) [Winspace.Native]::FrameBounds($Hwnd) }
function Get-WorkArea    { param([Parameter(Mandatory)][IntPtr]$Hwnd) [Winspace.Native]::WorkAreaForWindow($Hwnd) }
function Test-WindowCloaked { param([Parameter(Mandatory)][IntPtr]$Hwnd) [Winspace.Native]::IsCloaked($Hwnd) }

# Visible top-level windows whose caption contains $Substring — the cloaked-UWP
# seam's only way to reach a Store app's frame window (its stub launcher owns no
# window and its real PID is unknowable up front). Emits the handles to the
# pipeline one at a time (NO unary-comma wrap: the callers pipe this into
# Where-Object, where a `,@(...)`-wrapped result would surface the whole IntPtr[]
# as a single $_ and fail the [IntPtr] cast — the cloaked-uwp seam's crash).
function Find-WindowsByTitle {
    param([Parameter(Mandatory)][string]$Substring)
    return @([Winspace.Native]::FindVisibleWindowsByTitle($Substring))
}

# Fills assert on the VISIBLE frame, which winspace lands flush on rcWork by
# growing the window out past its invisible drop-shadow border — the compensation
# is integer-exact at 100% DPI, so a couple of pixels covers DWM rounding / the
# Win11 rounded-corner allowance. "Unchanged" (the ineligible seam) compares the
# raw rect exactly: winspace never touched it, so it is byte-identical.
function Test-RectNear {
    param([Parameter(Mandatory)]$Actual, [Parameter(Mandatory)]$Expected, [int]$Tolerance = 4)
    return ([math]::Abs($Actual.left - $Expected.left)     -le $Tolerance) -and
           ([math]::Abs($Actual.top - $Expected.top)       -le $Tolerance) -and
           ([math]::Abs($Actual.right - $Expected.right)   -le $Tolerance) -and
           ([math]::Abs($Actual.bottom - $Expected.bottom) -le $Tolerance)
}
function Test-RectEqual {
    param([Parameter(Mandatory)]$A, [Parameter(Mandatory)]$B)
    return $A.left -eq $B.left -and $A.top -eq $B.top -and $A.right -eq $B.right -and $A.bottom -eq $B.bottom
}
function Format-Rect { param($R) "[$($R.left),$($R.top) $($R.right),$($R.bottom)]" }

# ── test-window fixture: a real top-level window winspace tracks (or ignores) ──
# A genuine Win32 window whose eligibility winspace's own gate (isTileable) decides
# from live style bits — spawned in a SEPARATE process so its HWND is real to
# EnumWindows / the win-event hook, exactly like a user's app:
#   sizable — WS_THICKFRAME|WS_CAPTION, unowned  → Tileable (winspace fills it)
#   tool    — + WS_EX_TOOLWINDOW                 → Ineligible (left alone)
#   dialog  — FixedDialog, no WS_THICKFRAME      → Ineligible (left alone)
# The child writes its own top-level HWND to a ready file once shown, so the seam
# drives the exact window winspace sees. Returns { Process; Hwnd; ReadyFile; Title }.
function Start-TestWindow {
    [CmdletBinding()]
    param(
        [ValidateSet('sizable', 'tool', 'dialog')][string]$Style = 'sizable',
        [int]$X = 80, [int]$Y = 80, [int]$Width = 520, [int]$Height = 360,
        [string]$Title = 'winspace-test-window',
        [int]$ReadyTimeoutSec = 12
    )
    $border = switch ($Style) {
        'sizable' { 'Sizable' }
        'tool'    { 'SizableToolWindow' }   # keeps thickframe+caption but sets WS_EX_TOOLWINDOW
        'dialog'  { 'FixedDialog' }          # drops WS_THICKFRAME
    }
    $readyFile = Join-Path (Get-WinspaceRoot) ("testwindow-{0}.hwnd" -f [guid]::NewGuid().ToString('N'))
    if (Test-Path $readyFile) { Remove-Item $readyFile -Force }

    # Interpolate geometry + style + ready-file path into the child; `$-escape the
    # tokens that must survive to it. The child is PMv2 DPI aware so its Bounds land
    # in the same physical pixels the runner and winspace read. Shipped via
    # -EncodedCommand so no quoting crosses the boundary (mirrors Register-ConflictingHotkey).
    # The child hides its OWN console window (ShowWindow SW_HIDE on GetConsoleWindow)
    # BEFORE it creates the form, so the launcher's console never lingers as a second
    # eligible (WS_THICKFRAME|WS_CAPTION) top-level window that winspace would track and
    # fill alongside the test form — which perturbs head-fill ordering. This is done in
    # the child (an explicit ShowWindow), NOT via Start-Process -WindowStyle Hidden: that
    # flag sets STARTUPINFO.wShowWindow = SW_HIDE, which Windows applies to the process's
    # FIRST ShowWindow — i.e. the WinForms form itself — leaving the form invisible and
    # so ineligible. Hiding the console here lets the process start normally, so the form
    # shows normally (its ShowWindow is not the first). The child writes its HWND to a
    # temp file then MOVEs it into place, so the ready file appears atomically (no
    # partial/locked read races the parent's poll).
    $helper = @"
`$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace TW -Name Win -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetProcessDpiAwarenessContext(System.IntPtr v);
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern System.IntPtr GetConsoleWindow();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool ShowWindow(System.IntPtr h, int n);
'@
try { [void][TW.Win]::SetProcessDpiAwarenessContext([System.IntPtr](-4)) } catch { }
try { [void][TW.Win]::ShowWindow([TW.Win]::GetConsoleWindow(), 0) } catch { }   # SW_HIDE
`$f = New-Object System.Windows.Forms.Form
`$f.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::$border
`$f.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
`$f.Text = '$Title'
`$f.Bounds = New-Object System.Drawing.Rectangle($X, $Y, $Width, $Height)
`$f.Add_Shown({
    `$tmp = '$readyFile' + '.tmp'
    Set-Content -LiteralPath `$tmp -Value ([string][int64]`$f.Handle)
    Move-Item -LiteralPath `$tmp -Destination '$readyFile' -Force
})
[System.Windows.Forms.Application]::Run(`$f)
"@
    $enc = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($helper))
    $proc = Start-Process -FilePath 'powershell.exe' -PassThru `
        -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $enc

    # Poll for the ready file AND a readable, non-empty HWND in one condition, so a
    # transient lock or an empty just-created file simply re-polls rather than throwing.
    $hwndText = $null
    try {
        Wait-Until -TimeoutSec $ReadyTimeoutSec -Because "test window '$Title' to show and report its HWND" -Condition {
            if (-not (Test-Path $readyFile)) { return $false }
            try { $t = (Get-Content -LiteralPath $readyFile -Raw -ErrorAction Stop).Trim() } catch { return $false }
            if ($t) { $script:hwndText = $t; return $true }
            return $false
        }
    } catch {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw
    }
    $hwnd = [IntPtr][int64]$script:hwndText
    if ($hwnd -eq [IntPtr]::Zero) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        throw "Start-TestWindow: child reported a null HWND for '$Title'."
    }
    return [pscustomobject]@{ Process = $proc; Hwnd = $hwnd; ReadyFile = $readyFile; Title = $Title }
}

# Graceful close: PostMessage WM_CLOSE so the window crosses a real DESTROY/HIDE
# edge (the Vanished the reclaim seam depends on), then wait for the process to go.
function Close-TestWindow {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$Window, [int]$TimeoutSec = 8)
    if ($Window.Hwnd -ne [IntPtr]::Zero) {
        [void][Winspace.Native]::PostMessageW($Window.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
    }
    if ($Window.Process) {
        Wait-Until -TimeoutSec $TimeoutSec -Because "test window '$($Window.Title)' to exit after WM_CLOSE" -Condition {
            $Window.Process.HasExited
        }
    }
}

# finally-block cleanup: force-kill a fixture regardless of state; never throws.
function Stop-TestWindow {
    [CmdletBinding()]
    param($Window)
    if ($Window -and $Window.Process -and -not $Window.Process.HasExited) {
        Stop-Process -Id $Window.Process.Id -Force -ErrorAction SilentlyContinue
    }
}

# Move a window ourselves (SWP_NOZORDER|SWP_NOACTIVATE). winspace is a place-once
# tiler — it never re-asserts on a move (EVENT_OBJECT_LOCATIONCHANGE is dropped) —
# so the reclaim seam uses this to shove the survivor OFF rcWork, proving the
# survivor's fill on the head's close is a genuine, observable reclaim.
function Move-Window {
    [CmdletBinding()]
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [int]$X, [int]$Y, [int]$Width, [int]$Height)
    [void][Winspace.Native]::SetWindowPos($Hwnd, [IntPtr]::Zero, $X, $Y, $Width, $Height, [uint32](0x0004 -bor 0x0010))
}

# ── Fresh-mode discovery: the live (non-skipped) seam tags ────────────────────
# The host -Fresh loop reverts the snapshot once per LIVE seam, so it must know the
# seam set. Discover it here (Pester discovery only, SkipRun) rather than hardcode
# it host-side, so a newly-dropped seam file is picked up with no orchestrator edit.
# The 'scaffold' tag (issues 03–10, all -Skip) is excluded — those need no VM and
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
    Get-WinspaceLogText, Save-FailureScreenshot, Set-RunnerConsoleVisible, Invoke-WinspaceSeams, Get-WinspaceLiveSeamTags,
    Get-WinspaceRoot, Get-WinspaceExe, Get-WinspaceLog,
    Get-ForegroundWindow,
    Get-WindowRect, Get-FrameBounds, Get-WorkArea, Test-WindowCloaked, Find-WindowsByTitle,
    Test-RectNear, Test-RectEqual, Format-Rect,
    Start-TestWindow, Close-TestWindow, Stop-TestWindow, Move-Window
