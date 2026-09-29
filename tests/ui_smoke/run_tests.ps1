<#
.SYNOPSIS
    Real end-to-end Aperture UI crash smoke test.

.DESCRIPTION
    Stages an isolated portable copy of Aperture, opens the supplied media as
    the product's real launch target, drives the visible window with Win32
    input, and records the process/window state after every user-facing
    action. It also opens and dismisses the native file picker and queries the
    Application event log for WER entries that reference the staged executable,
    so a failed step has both a visible symptom and operating-system evidence.

    The test deliberately uses the native UI and the supplied media file. It
    does not call application internals, inspect only source files, or accept
    a process that merely starts as proof that the UI is healthy.
#>

[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot "..\..\debug\Aperture.exe"),
    [string]$VideoPath = '',
    [string]$DataSource = "",
    [int]$StartupTimeoutMs = 15000,
    [int]$StepWaitMs = 900,
    [switch]$KeepArtifacts,
    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = [IO.Path]::GetFullPath($PSScriptRoot)
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir "..\.."))
$executablePath = [IO.Path]::GetFullPath($Executable)
if ([string]::IsNullOrWhiteSpace($VideoPath)) {
    throw 'Supply -VideoPath for this manual pointer smoke test, or run tests/workflows/run_tests.ps1 -AppOnly for self-contained playback scenarios.'
}
$videoFullPath = [IO.Path]::GetFullPath($VideoPath)

if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Aperture executable not found: $executablePath"
}
if (-not (Test-Path -LiteralPath $videoFullPath -PathType Leaf)) {
    throw "Test media file not found: $videoFullPath"
}

$runId = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ") + "-" + [Guid]::NewGuid().ToString("N")
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$runDir = Join-Path $tempRoot ("Aperture-ui-smoke-" + [Guid]::NewGuid().ToString("N"))
$stagedExe = Join-Path $runDir "Aperture.exe"
$reportDir = Join-Path $repoRoot "diagnostics\ui-smoke"
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $reportDir ("ui-smoke-" + $runId + ".json")
} else {
    $ReportPath = [IO.Path]::GetFullPath($ReportPath)
}

New-Item -ItemType Directory -Path $runDir -Force | Out-Null
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
$reportParent = Split-Path -Parent $ReportPath
if ($reportParent -and -not (Test-Path -LiteralPath $reportParent)) {
    New-Item -ItemType Directory -Path $reportParent -Force | Out-Null
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class ApertureUiSmokeNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    private const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    private const uint MOUSEEVENTF_WHEEL = 0x0800;
    private const uint KEYEVENTF_KEYUP = 0x0002;
    private const int SW_MAXIMIZE = 3;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder text, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessage(IntPtr hWnd, uint message, IntPtr wParam, string lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetActiveWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool attach);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int Dx;
        public int Dy;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct INPUTUNION
    {
        [FieldOffset(0)] public MOUSEINPUT Mouse;
        [FieldOffset(0)] public KEYBDINPUT Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint Type;
        public INPUTUNION Data;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, INPUT[] inputs, int inputSize);

    public static IntPtr FindWindowForProcess(uint targetProcessId)
    {
        IntPtr fallback = IntPtr.Zero;
        IntPtr titled = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            if (!IsWindowVisible(hWnd)) return true;
            uint processId;
            GetWindowThreadProcessId(hWnd, out processId);
            if (processId == targetProcessId)
            {
                if (fallback == IntPtr.Zero) fallback = hWnd;
                string title = GetWindowTitle(hWnd);
                if (!String.IsNullOrWhiteSpace(title))
                {
                    titled = hWnd;
                    if (title == "Aperture") return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return titled != IntPtr.Zero ? titled : fallback;
    }

    public static IntPtr FindFileDialog()
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate (IntPtr hWnd, IntPtr lParam)
        {
            if (!IsWindowVisible(hWnd)) return true;
            if (GetWindowClass(hWnd) == "#32770")
            {
                result = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static IntPtr GetForeground() { return GetForegroundWindow(); }

    public static string GetWindowTitle(IntPtr hWnd)
    {
        var text = new StringBuilder(1024);
        GetWindowText(hWnd, text, text.Capacity);
        return text.ToString();
    }

    public static string GetWindowClass(IntPtr hWnd)
    {
        var text = new StringBuilder(256);
        GetClassName(hWnd, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr FindFileNameEdit(IntPtr dialog)
    {
        IntPtr result = IntPtr.Zero;
        int lowestTop = Int32.MinValue;
        EnumChildWindows(dialog, delegate (IntPtr hWnd, IntPtr lParam)
        {
            if (GetWindowClass(hWnd) != "Edit") return true;
            RECT rect;
            if (GetWindowRect(hWnd, out rect) && rect.Top > lowestTop)
            {
                lowestTop = rect.Top;
                result = hWnd;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static IntPtr FindButtonByText(IntPtr dialog, string expectedText)
    {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(dialog, delegate (IntPtr hWnd, IntPtr lParam)
        {
            if (GetWindowClass(hWnd) == "Button" && GetWindowTitle(hWnd) == expectedText)
            {
                result = hWnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static bool SetControlText(IntPtr hWnd, string text)
    {
        if (hWnd == IntPtr.Zero) return false;
        return SendMessage(hWnd, 0x000C, IntPtr.Zero, text) != IntPtr.Zero;
    }

    public static bool GetBounds(IntPtr hWnd, out RECT rect)
    {
        return GetWindowRect(hWnd, out rect);
    }

    public static bool Activate(IntPtr hWnd)
    {
        if (hWnd == IntPtr.Zero) return false;
        IntPtr foreground = GetForegroundWindow();
        uint foregroundPid;
        uint foregroundThread = GetWindowThreadProcessId(foreground, out foregroundPid);
        uint targetPid;
        uint targetThread = GetWindowThreadProcessId(hWnd, out targetPid);
        uint currentThread = GetCurrentThreadId();
        bool attachedToForeground = false;
        bool attachedToTarget = false;
        try
        {
            if (foregroundThread != 0 && foregroundThread != currentThread)
                attachedToForeground = AttachThreadInput(currentThread, foregroundThread, true);
            if (targetThread != 0 && targetThread != currentThread && targetThread != foregroundThread)
                attachedToTarget = AttachThreadInput(currentThread, targetThread, true);
            ShowWindow(hWnd, SW_MAXIMIZE);
            BringWindowToTop(hWnd);
            SetActiveWindow(hWnd);
            SetFocus(hWnd);
            SetForegroundWindow(hWnd);
            Thread.Sleep(80);
            IntPtr active = GetForegroundWindow();
            uint activePid;
            GetWindowThreadProcessId(active, out activePid);
            return activePid == targetPid;
        }
        finally
        {
            if (attachedToTarget) AttachThreadInput(currentThread, targetThread, false);
            if (attachedToForeground) AttachThreadInput(currentThread, foregroundThread, false);
        }
    }

    public static bool Maximize(IntPtr hWnd)
    {
        if (hWnd == IntPtr.Zero) return false;
        return ShowWindow(hWnd, SW_MAXIMIZE);
    }

    public static bool MoveTo(int x, int y)
    {
        return SetCursorPos(x, y);
    }

    public static void ClickAt(int x, int y, bool rightButton)
    {
        if (!SetCursorPos(x, y)) throw new InvalidOperationException("Could not move the pointer to the target window.");
        Thread.Sleep(35);
        uint down = rightButton ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        uint up = rightButton ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendMouseInput(down, 0);
        Thread.Sleep(25);
        SendMouseInput(up, 0);
    }

    public static void WheelAt(int x, int y, int delta)
    {
        if (!SetCursorPos(x, y)) throw new InvalidOperationException("Could not move the pointer to the target window.");
        Thread.Sleep(35);
        SendMouseInput(MOUSEEVENTF_WHEEL, unchecked((uint)delta));
    }

    private static void SendMouseInput(uint flags, uint mouseData)
    {
        var input = new INPUT();
        input.Type = 0;
        input.Data.Mouse = new MOUSEINPUT {
            Dx = 0,
            Dy = 0,
            MouseData = mouseData,
            Flags = flags,
            Time = 0,
            ExtraInfo = UIntPtr.Zero
        };
        if (SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT))) != 1) {
            throw new InvalidOperationException("SendInput did not deliver the mouse event.");
        }
    }

    public static void KeyPress(byte virtualKey)
    {
        keybd_event(virtualKey, 0, 0, UIntPtr.Zero);
        Thread.Sleep(25);
        keybd_event(virtualKey, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static void KeyCombo(byte modifier, byte virtualKey)
    {
        keybd_event(modifier, 0, 0, UIntPtr.Zero);
        Thread.Sleep(20);
        keybd_event(virtualKey, 0, 0, UIntPtr.Zero);
        Thread.Sleep(25);
        keybd_event(virtualKey, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(modifier, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static void TypeUnicodeText(string text)
    {
        if (String.IsNullOrEmpty(text)) return;
        var inputs = new INPUT[text.Length * 2];
        int index = 0;
        foreach (char character in text)
        {
            inputs[index].Type = 1;
            inputs[index].Data.Keyboard = new KEYBDINPUT {
                VirtualKey = 0,
                ScanCode = character,
                Flags = 0x0004,
                Time = 0,
                ExtraInfo = UIntPtr.Zero
            };
            index++;
            inputs[index].Type = 1;
            inputs[index].Data.Keyboard = new KEYBDINPUT {
                VirtualKey = 0,
                ScanCode = character,
                Flags = 0x0004 | KEYEVENTF_KEYUP,
                Time = 0,
                ExtraInfo = UIntPtr.Zero
            };
            index++;
        }
        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$script:appProcess = $null
$script:mainHandle = [IntPtr]::Zero
$script:steps = [System.Collections.Generic.List[object]]::new()
$script:failureStep = $null
$script:testStarted = Get-Date
$script:retainRunDir = [bool]$KeepArtifacts
$expectedFileName = [IO.Path]::GetFileName($videoFullPath)

function Format-Handle {
    param([IntPtr]$Handle)
    if ($Handle -eq [IntPtr]::Zero) { return "0x0" }
    return ("0x{0:X}" -f $Handle.ToInt64())
}

function Get-WindowSnapshot {
    $handle = [IntPtr]::Zero
    $running = $false
    $exitCode = $null
    $processId = $null

    if ($null -ne $script:appProcess) {
        $processId = $script:appProcess.Id
        try {
            $script:appProcess.Refresh()
            $running = -not $script:appProcess.HasExited
            if (-not $running) { $exitCode = $script:appProcess.ExitCode }
        } catch {
            $running = $false
        }
        if ($running) {
            if ($script:mainHandle -ne [IntPtr]::Zero) {
                $handle = $script:mainHandle
            } else {
                $handle = [ApertureUiSmokeNative]::FindWindowForProcess([uint32]$processId)
            }
        }
    }

    $left = $null
    $top = $null
    $right = $null
    $bottom = $null
    $title = ""
    if ($handle -ne [IntPtr]::Zero) {
        $rect = New-Object -TypeName 'ApertureUiSmokeNative+RECT'
        if ([ApertureUiSmokeNative]::GetBounds($handle, [ref]$rect)) {
            $left = $rect.Left
            $top = $rect.Top
            $right = $rect.Right
            $bottom = $rect.Bottom
        }
        $title = [ApertureUiSmokeNative]::GetWindowTitle($handle)
    }

    [PSCustomObject]@{
        IsRunning = $running
        Pid = $processId
        ExitCode = $exitCode
        Handle = $handle
        WindowHandle = (Format-Handle $handle)
        WindowTitle = $title
        Left = $left
        Top = $top
        Right = $right
        Bottom = $bottom
    }
}

function Get-ReportSnapshot {
    param($Snapshot)
    [ordered]@{
        isRunning = [bool]$Snapshot.IsRunning
        pid = $Snapshot.Pid
        exitCode = $Snapshot.ExitCode
        windowHandle = $Snapshot.WindowHandle
        windowTitle = $Snapshot.WindowTitle
        bounds = if ($null -ne $Snapshot.Left) {
            [ordered]@{ left = $Snapshot.Left; top = $Snapshot.Top; right = $Snapshot.Right; bottom = $Snapshot.Bottom }
        } else { $null }
    }
}

function Get-WerEvidence {
    $matches = [System.Collections.Generic.List[object]]::new()
    try {
        $events = @(Get-WinEvent -FilterHashtable @{
            LogName = "Application"
            Id = @(1000, 1001)
            StartTime = $script:testStarted
        } -ErrorAction SilentlyContinue)
        foreach ($event in $events) {
            if ($event.Message -and $event.Message.IndexOf($stagedExe, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $message = $event.Message
                if ($message.Length -gt 2400) { $message = $message.Substring(0, 2400) }
                $matches.Add([ordered]@{
                    timeUtc = $event.TimeCreated.ToUniversalTime().ToString("o")
                    eventId = $event.Id
                    provider = $event.ProviderName
                    message = $message
                })
            }
        }
    } catch {
        # Event-log access is supplementary. Process exit and window loss still fail the step.
    }
    return @($matches)
}

function Get-LibrarySummary {
    $libraryPath = Join-Path $runDir "data\library.json"
    if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) {
        return [PSCustomObject]@{ Exists = $false; Count = 0; Paths = @() }
    }
    try {
        $data = Get-Content -LiteralPath $libraryPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $items = if ($data -is [System.Array]) { @($data) } else { @($data.items) }
        $items = @($items | Where-Object { $null -ne $_ })
        $paths = @($items | ForEach-Object { [string]$_.filePath })
        return [PSCustomObject]@{ Exists = $true; Count = $items.Count; Paths = $paths }
    } catch {
        return [PSCustomObject]@{ Exists = $true; Count = -1; Paths = @(); Error = $_.Exception.Message }
    }
}

function Wait-ForMainWindow {
    param([int]$TimeoutMs)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $snapshot = Get-WindowSnapshot
        if ($snapshot.IsRunning -and $snapshot.Handle -ne [IntPtr]::Zero -and -not [string]::IsNullOrWhiteSpace($snapshot.WindowTitle)) { return $snapshot }
        if ($null -ne $script:appProcess) {
            try { if ($script:appProcess.HasExited) { throw "Aperture exited with code $($script:appProcess.ExitCode) during startup." } } catch { throw }
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Aperture did not create a visible top-level window within ${TimeoutMs}ms."
}

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Condition,
        [Parameter(Mandatory = $true)][int]$TimeoutMs,
        [string]$FailureMessage = "Condition was not met"
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw $FailureMessage
}

function Get-ActiveAppWindow {
    $snapshot = Get-WindowSnapshot
    if (-not $snapshot.IsRunning -or $snapshot.Handle -eq [IntPtr]::Zero) {
        throw "Aperture is not running or has no visible window."
    }
    if (-not [ApertureUiSmokeNative]::Activate($snapshot.Handle)) {
        throw "Could not focus the Aperture window before sending native input."
    }
    Start-Sleep -Milliseconds 80
    return $snapshot
}

function Get-Point {
    param($Snapshot, [double]$XRatio, [double]$YRatio)
    if ($null -eq $Snapshot.Left -or $null -eq $Snapshot.Top) { throw "Window bounds are unavailable." }
    $width = $Snapshot.Right - $Snapshot.Left
    $height = $Snapshot.Bottom - $Snapshot.Top
    [PSCustomObject]@{
        X = [int]($Snapshot.Left + ($width * $XRatio))
        Y = [int]($Snapshot.Top + ($height * $YRatio))
    }
}

function Get-OffsetPoint {
    param($Snapshot, [double]$XRatio, [int]$YOffset)
    if ($null -eq $Snapshot.Left -or $null -eq $Snapshot.Top) { throw "Window bounds are unavailable." }
    $width = $Snapshot.Right - $Snapshot.Left
    [PSCustomObject]@{
        X = [int]($Snapshot.Left + ($width * $XRatio))
        Y = [int]($Snapshot.Top + $YOffset)
    }
}

function Get-DialogBounds {
    param([IntPtr]$Handle)
    $rect = New-Object -TypeName 'ApertureUiSmokeNative+RECT'
    if (-not [ApertureUiSmokeNative]::GetBounds($Handle, [ref]$rect)) {
        throw "Could not read the native file dialog bounds."
    }
    return $rect
}

function Wait-ForFileDialog {
    param([int]$TimeoutMs = 5000)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $foreground = [ApertureUiSmokeNative]::GetForeground()
        if ($foreground -ne [IntPtr]::Zero) {
            $mainHandle = (Get-WindowSnapshot).Handle
            $foregroundTitle = [ApertureUiSmokeNative]::GetWindowTitle($foreground)
            $foregroundClass = [ApertureUiSmokeNative]::GetWindowClass($foreground)
            if (($foregroundClass -eq "#32770" -or $foregroundTitle -eq "Open") -and $foreground -ne $mainHandle) {
                return $foreground
            }
        }
        $dialog = [ApertureUiSmokeNative]::FindFileDialog()
        if ($dialog -ne [IntPtr]::Zero) { return $dialog }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Native Open File dialog did not appear."
}

function Invoke-UiStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [int]$WaitMs = $StepWaitMs,
        [string]$RequiredTitleFragment = ""
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $actionError = $null
    try {
        & $Action
    } catch {
        $actionError = $_.Exception.Message
    }
    if ($WaitMs -gt 0) { Start-Sleep -Milliseconds $WaitMs }
    $timer.Stop()

    $snapshot = Get-WindowSnapshot
    $wer = @(Get-WerEvidence)
    $passed = ($null -eq $actionError -and $snapshot.IsRunning -and $snapshot.Handle -ne [IntPtr]::Zero)
    if ($passed -and -not [string]::IsNullOrWhiteSpace($RequiredTitleFragment)) {
        $passed = $snapshot.WindowTitle.IndexOf($RequiredTitleFragment, [StringComparison]::OrdinalIgnoreCase) -ge 0
        if (-not $passed -and $null -eq $actionError) {
            $actionError = "Window title did not contain '$RequiredTitleFragment'; observed '$($snapshot.WindowTitle)'."
        }
    }

    $record = [ordered]@{
        name = $Name
        passed = [bool]$passed
        elapsedMs = [int]$timer.ElapsedMilliseconds
        actionError = $actionError
        state = Get-ReportSnapshot $snapshot
        werEvents = $wer
    }
    $script:steps.Add([PSCustomObject]$record)

    if ($passed) {
        Write-Host ("[PASS] {0} | pid={1} | title={2}" -f $Name, $snapshot.Pid, $snapshot.WindowTitle) -ForegroundColor Green
    } else {
        $script:failureStep = $Name
        $script:retainRunDir = $true
        Write-Host ("[FAIL] {0} | pid={1} | exit={2} | title={3}" -f $Name, $snapshot.Pid, $snapshot.ExitCode, $snapshot.WindowTitle) -ForegroundColor Red
        if ($actionError) { Write-Host ("       {0}" -f $actionError) -ForegroundColor Red }
        if ($wer.Count -gt 0) { Write-Host ("       WER events: {0}" -f $wer.Count) -ForegroundColor Red }
        throw "UI smoke step failed: $Name"
    }
    return $snapshot
}

function Copy-StagedRuntime {
    Copy-Item -LiteralPath $executablePath -Destination $stagedExe -Force
    $pdbSource = [IO.Path]::ChangeExtension($executablePath, ".pdb")
    if (Test-Path -LiteralPath $pdbSource -PathType Leaf) {
        Copy-Item -LiteralPath $pdbSource -Destination (Join-Path $runDir "Aperture.pdb") -Force
    }

    $dllCandidates = @(
        (Join-Path ([IO.Path]::GetDirectoryName($executablePath)) "libmpv-2.dll"),
        (Join-Path $repoRoot "libmpv-2.dll")
    )
    $dllSource = $dllCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $dllSource) { throw "libmpv-2.dll was not found beside the executable or in the repository root." }
    Copy-Item -LiteralPath $dllSource -Destination (Join-Path $runDir "libmpv-2.dll") -Force

    $dataCandidates = if (-not [string]::IsNullOrWhiteSpace($DataSource)) {
        @([IO.Path]::GetFullPath($DataSource))
    } else {
        @() # Personal profiles are copied only when explicitly supplied.
    }
    $dataSource = $dataCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -First 1
    if ($dataSource) {
        Copy-Item -LiteralPath $dataSource -Destination $runDir -Recurse -Force
    } else {
        New-Item -ItemType Directory -Path (Join-Path $runDir "data") -Force | Out-Null
    }
}

$testResult = "FAIL"
$failureMessage = $null
$finalWer = @()
$beforeRemove = $null
$afterRemove = $null

try {
    Copy-StagedRuntime
    Write-Host "Aperture real UI smoke test" -ForegroundColor Cyan
    Write-Host ("Executable: {0}" -f $executablePath) -ForegroundColor DarkGray
    Write-Host ("Video:      {0}" -f $videoFullPath) -ForegroundColor DarkGray
    Write-Host ("Staged at:  {0}" -f $runDir) -ForegroundColor DarkGray

    Invoke-UiStep -Name "launch-and-open-video" -WaitMs 1200 -RequiredTitleFragment $expectedFileName -Action {
        $script:appProcess = Start-Process -FilePath $stagedExe -WorkingDirectory $runDir -ArgumentList @('"' + $videoFullPath + '"') -PassThru
        $window = Wait-ForMainWindow -TimeoutMs $StartupTimeoutMs
        $script:mainHandle = $window.Handle
        Wait-Until -TimeoutMs 15000 -FailureMessage "Aperture did not enter playback for the supplied media file." -Condition {
            $state = Get-WindowSnapshot
            return $state.IsRunning -and $state.WindowTitle.IndexOf($expectedFileName, [StringComparison]::OrdinalIgnoreCase) -ge 0
        }
    } | Out-Null

    Invoke-UiStep -Name "close-video" -WaitMs 1100 -Action {
        $main = Get-ActiveAppWindow
        [ApertureUiSmokeNative]::KeyPress(0x1B)
        Wait-Until -TimeoutMs 4000 -FailureMessage "Aperture did not leave playback after Escape." -Condition {
            $state = Get-WindowSnapshot
            return $state.IsRunning -and $state.WindowTitle -eq "Aperture"
        }
    } | Out-Null

    Invoke-UiStep -Name "open-and-dismiss-file-picker" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $openButton = [PSCustomObject]@{ X = $main.Right - 72; Y = $main.Top + 57 }
        [ApertureUiSmokeNative]::ClickAt($openButton.X, $openButton.Y, $false)
        $dialog = Wait-ForFileDialog
        [void][ApertureUiSmokeNative]::Activate($dialog)
        [ApertureUiSmokeNative]::KeyPress(0x1B)
        Wait-Until -TimeoutMs 4000 -FailureMessage "Aperture did not dismiss the native file picker." -Condition {
            $state = Get-WindowSnapshot
            return $state.IsRunning -and $state.WindowTitle -eq "Aperture"
        }
    } | Out-Null

    Wait-Until -TimeoutMs 5000 -FailureMessage "The real video did not create a library entry." -Condition {
        $summary = Get-LibrarySummary
        return $summary.Exists -and $summary.Count -gt 0
    }
    $beforeRemove = Get-LibrarySummary

    Invoke-UiStep -Name "hover-card" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $card = Get-OffsetPoint $main 0.50 200
        [void][ApertureUiSmokeNative]::MoveTo($card.X, $card.Y)
    } | Out-Null

    Invoke-UiStep -Name "scroll-media" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $scrollArea = Get-Point $main 0.52 0.80
        [ApertureUiSmokeNative]::WheelAt($scrollArea.X, $scrollArea.Y, -720)
    } | Out-Null

    Invoke-UiStep -Name "open-settings" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $settingsButton = [PSCustomObject]@{ X = $main.Right - 177; Y = $main.Top + 57 }
        [ApertureUiSmokeNative]::ClickAt($settingsButton.X, $settingsButton.Y, $false)
    } | Out-Null

    Invoke-UiStep -Name "scroll-settings" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $scrollArea = Get-Point $main 0.52 0.80
        [ApertureUiSmokeNative]::WheelAt($scrollArea.X, $scrollArea.Y, -720)
    } | Out-Null

    Invoke-UiStep -Name "return-to-media" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $mediaButton = [PSCustomObject]@{ X = $main.Left + 72; Y = $main.Top + 57 }
        [ApertureUiSmokeNative]::ClickAt($mediaButton.X, $mediaButton.Y, $false)
    } | Out-Null

    Invoke-UiStep -Name "right-click-card" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        $card = Get-OffsetPoint $main 0.50 200
        [ApertureUiSmokeNative]::ClickAt($card.X, $card.Y, $true)
    } | Out-Null

    Invoke-UiStep -Name "remove-from-context-menu" -WaitMs $StepWaitMs -Action {
        $main = Get-ActiveAppWindow
        # The menu is native-rendered by the app and its final row is stable
        # relative to the main window. Click the visible Remove action so this
        # step exercises the same path as a real pointer user, independent of
        # keyboard-focus behavior in the test harness.
        # The context menu opens at the pointer (the test card is near the top
        # of the maximized client area), so its final row is around 75% down
        # the window rather than at the bottom edge.
        $removeItem = Get-Point $main 0.525 0.75
        [ApertureUiSmokeNative]::ClickAt($removeItem.X, $removeItem.Y, $false)
    } | Out-Null

    Invoke-UiStep -Name "confirm-remove" -WaitMs 1300 -Action {
        $main = Get-ActiveAppWindow
        # Confirm through the visible modal button as well; the modal's primary
        # action is centered at a stable position in the maximized test window.
        $confirmButton = Get-Point $main 0.589 0.56
        [ApertureUiSmokeNative]::ClickAt($confirmButton.X, $confirmButton.Y, $false)
        Wait-Until -TimeoutMs 5000 -FailureMessage "The removed library item was not persisted." -Condition {
            $summary = Get-LibrarySummary
            return $summary.Exists -and $summary.Count -lt $beforeRemove.Count
        }
    } | Out-Null

    $afterRemove = Get-LibrarySummary
    $testResult = "PASS"
    Write-Host ("[PASS] delete persisted: library count {0} -> {1}" -f $beforeRemove.Count, $afterRemove.Count) -ForegroundColor Green
} catch {
    $failureMessage = $_.Exception.Message
    if (-not $script:failureStep) { $script:failureStep = "setup-or-unrecorded-step" }
    Write-Host ("Test failed: {0}" -f $failureMessage) -ForegroundColor Red
} finally {
    $finalWer = @(Get-WerEvidence)
    if ($null -ne $script:appProcess) {
        try {
            $script:appProcess.Refresh()
            if (-not $script:appProcess.HasExited) {
                [void][ApertureUiSmokeNative]::Activate([ApertureUiSmokeNative]::FindWindowForProcess([uint32]$script:appProcess.Id))
                [ApertureUiSmokeNative]::KeyCombo(0x12, 0x73)
                [void]$script:appProcess.WaitForExit(2500)
            }
        } catch {}
        try {
            $script:appProcess.Refresh()
            if (-not $script:appProcess.HasExited) {
                Stop-Process -Id $script:appProcess.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {}
    }

    $report = [ordered]@{
        test = "aperture-real-ui-smoke"
        result = $testResult
        failureStep = $script:failureStep
        failure = $failureMessage
        startedUtc = $script:testStarted.ToUniversalTime().ToString("o")
        finishedUtc = (Get-Date).ToUniversalTime().ToString("o")
        executable = $executablePath
        stagedExecutable = $stagedExe
        dataSource = if ([string]::IsNullOrWhiteSpace($DataSource)) { (Join-Path $repoRoot "data") } else { [IO.Path]::GetFullPath($DataSource) }
        video = $videoFullPath
        runDirectory = $runDir
        pid = if ($null -ne $script:appProcess) { $script:appProcess.Id } else { $null }
        libraryBeforeRemove = $beforeRemove
        libraryAfterRemove = $afterRemove
        steps = @($script:steps)
        werEvents = $finalWer
        artifactsRetained = [bool]$script:retainRunDir
    }
    [IO.File]::WriteAllText($ReportPath, ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    Write-Host ("Report: {0}" -f $ReportPath) -ForegroundColor Cyan

    $safeRunPrefix = ($tempRoot + "\Aperture-ui-smoke-")
    $resolvedRunDir = [IO.Path]::GetFullPath($runDir)
    if (-not $script:retainRunDir -and $resolvedRunDir.StartsWith($safeRunPrefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedRunDir)) {
        Remove-Item -LiteralPath $resolvedRunDir -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($script:retainRunDir) {
        Write-Host ("Artifacts retained: {0}" -f $runDir) -ForegroundColor Yellow
    }
}

if ($testResult -eq "PASS") { exit 0 }
exit 1
