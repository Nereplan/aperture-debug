<#
.SYNOPSIS
    Real process-level Aperture memory probe.

.DESCRIPTION
    Runs the supplied executable from an isolated portable staging directory,
    opens the exact media file through the normal command-line path, samples
    process working set/private bytes and Windows GPU-process counters, then
    drives Escape and card hover through native Win32 input.  The report keeps
    the phases separate so decoder, preview, and post-close retention cannot be
    confused with one another.

    This is intentionally an end-to-end probe.  It does not call application
    internals or treat a successful process launch as a memory result.
#>

[CmdletBinding()]
param(
    [string]$Executable = (Join-Path $PSScriptRoot "..\..\debug\Aperture.exe"),
    [string]$VideoPath = '',
    [int]$PlaybackWarmupSeconds = 8,
    [int]$PlaybackSamples = 3,
    [int]$SampleIntervalSeconds = 2,
    [int]$PreviewWarmupSeconds = 4,
    [double]$MediaWorkingSetBudgetMiB = 350,
    [double]$IdleWorkingSetBudgetMiB = 15,
    [double]$PreviewWorkingSetBudgetMiB = 350,
    [string]$ReportPath = "",
    [switch]$KeepArtifacts,
    [switch]$FailOnBudget,
    [string]$StageRoot = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..")).TrimEnd('\', '/')
function Assert-RootLocalPath([string]$Candidate, [string]$Root, [string]$ParamName) {
    $full = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    $rootNorm = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if ($full.Length -le $rootNorm.Length) {
        throw "$ParamName must be a strict descendant of the Aperture root ($rootNorm), got: $full"
    }
    if (-not $full.StartsWith($rootNorm + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$ParamName must stay below the Aperture root ($rootNorm), got: $full. Refusing to stage outside the root."
    }
    return $full
}
$executablePath = [IO.Path]::GetFullPath($Executable)
$videoFullPath = [IO.Path]::GetFullPath($VideoPath)
if ([string]::IsNullOrWhiteSpace($StageRoot)) {
    $StageRoot = Join-Path $repoRoot "scratch\bench-stage"
}
$StageRoot = Assert-RootLocalPath $StageRoot $repoRoot "StageRoot"
$runId = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ") + "-" + [Guid]::NewGuid().ToString("N")
$runDir = Assert-RootLocalPath (Join-Path $StageRoot ("Aperture-memory-probe-" + [Guid]::NewGuid().ToString("N"))) $repoRoot "runDir"
$stagedExe = Join-Path $runDir "Aperture.exe"
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportDir = Join-Path $repoRoot "diagnostics\memory"
    $ReportPath = Join-Path $reportDir ("memory-" + $runId + ".json")
} else {
    $ReportPath = [IO.Path]::GetFullPath($ReportPath)
}

if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Aperture executable not found: $executablePath"
}
if (-not (Test-Path -LiteralPath $videoFullPath -PathType Leaf)) {
    throw "Test media file not found: $videoFullPath"
}

New-Item -ItemType Directory -Path $runDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $runDir "data") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $runDir "cache") -Force | Out-Null
$reportParent = Split-Path -Parent $ReportPath
if ($reportParent -and -not (Test-Path -LiteralPath $reportParent)) {
    New-Item -ItemType Directory -Path $reportParent -Force | Out-Null
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class ApertureMemoryProbeNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int capacity);

    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();

    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool attach);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr SetActiveWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern IntPtr SetFocus(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

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
            ShowWindow(hWnd, 3);
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

    public static void KeyPress(byte virtualKey)
    {
        keybd_event(virtualKey, 0, 0, UIntPtr.Zero);
        Thread.Sleep(25);
        keybd_event(virtualKey, 0, 2, UIntPtr.Zero);
    }

    public static string GetTitle(IntPtr hWnd)
    {
        var text = new StringBuilder(1024);
        GetWindowText(hWnd, text, text.Capacity);
        return text.ToString();
    }
}
'@

function Copy-StagedRuntime {
    Copy-Item -LiteralPath $executablePath -Destination $stagedExe -Force
    $dllSource = Join-Path ([IO.Path]::GetDirectoryName($executablePath)) "libmpv-2.dll"
    if (-not (Test-Path -LiteralPath $dllSource -PathType Leaf)) {
        $dllSource = Join-Path $repoRoot "libmpv-2.dll"
    }
    if (-not (Test-Path -LiteralPath $dllSource -PathType Leaf)) {
        throw "libmpv-2.dll was not found beside the executable or in the repository root."
    }
    Copy-Item -LiteralPath $dllSource -Destination (Join-Path $runDir "libmpv-2.dll") -Force
    $fontSource = Join-Path ([IO.Path]::GetDirectoryName($executablePath)) "fonts"
    if (Test-Path -LiteralPath $fontSource -PathType Container) {
        Copy-Item -LiteralPath $fontSource -Destination $runDir -Recurse -Force
    }
}

function Get-GpuUsageBytes {
    $dedicated = 0.0
    $shared = 0.0
    try {
        $prefix = "pid_$($script:appProcess.Id)_"
        $d = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage' -ErrorAction Stop).CounterSamples |
            Where-Object { $_.InstanceName -like ($prefix + "*") } |
            Measure-Object -Property CookedValue -Sum
        $s = (Get-Counter '\GPU Process Memory(*)\Shared Usage' -ErrorAction Stop).CounterSamples |
            Where-Object { $_.InstanceName -like ($prefix + "*") } |
            Measure-Object -Property CookedValue -Sum
        if ($null -ne $d.Sum) { $dedicated = [double]$d.Sum }
        if ($null -ne $s.Sum) { $shared = [double]$s.Sum }
    } catch {
        # GPU counters are optional on some Windows driver configurations.
    }
    return [PSCustomObject]@{
        dedicatedGpuBytes = [int64]$dedicated
        sharedGpuBytes = [int64]$shared
    }
}

function Get-MemorySample([string]$Phase) {
    $script:appProcess.Refresh()
    if ($script:appProcess.HasExited) {
        return [PSCustomObject]@{
            phase = $Phase
            running = $false
            exitCode = $script:appProcess.ExitCode
        }
    }
    $gpu = Get-GpuUsageBytes
    return [PSCustomObject]@{
        phase = $Phase
        timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
        running = $true
        pid = $script:appProcess.Id
        workingSetBytes = [int64]$script:appProcess.WorkingSet64
        privateBytes = [int64]$script:appProcess.PrivateMemorySize64
        peakWorkingSetBytes = [int64]$script:appProcess.PeakWorkingSet64
        peakPagedBytes = [int64]$script:appProcess.PeakPagedMemorySize64
        dedicatedGpuBytes = $gpu.dedicatedGpuBytes
        sharedGpuBytes = $gpu.sharedGpuBytes
        handles = [int]$script:appProcess.HandleCount
        threads = [int]$script:appProcess.Threads.Count
        windowTitle = [string]$script:appProcess.MainWindowTitle
    }
}

function Wait-ForWindow([int]$TimeoutMs = 15000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $script:appProcess.Refresh()
        if (-not $script:appProcess.HasExited -and $script:appProcess.MainWindowHandle -ne [IntPtr]::Zero) {
            return
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($script:appProcess.HasExited) {
        throw "Aperture exited during startup with code $($script:appProcess.ExitCode)."
    }
    throw "Aperture did not create a main window within ${TimeoutMs}ms."
}

function Send-Escape {
    $script:appProcess.Refresh()
    $mainWindow = [ApertureMemoryProbeNative]::FindWindow('Aperture_Native_Direct2D_Class', $null)
    if ($mainWindow -eq [IntPtr]::Zero) { $mainWindow = $script:appProcess.MainWindowHandle }
    [void][ApertureMemoryProbeNative]::Activate($mainWindow)
    [ApertureMemoryProbeNative]::KeyPress(27)
    # Keep the transition deterministic if another desktop window reclaimed
    # foreground ownership between activation and keybd_event.
    [void][ApertureMemoryProbeNative]::PostMessage($mainWindow, 0x0100, [IntPtr]27, [IntPtr]0x00010001)
    [void][ApertureMemoryProbeNative]::PostMessage($mainWindow, 0x0101, [IntPtr]27, [IntPtr]0xC0010001)
}

function Wait-ForHub([int]$TimeoutMs = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $nextRetry = [DateTime]::UtcNow.AddMilliseconds(350)
    do {
        $script:appProcess.Refresh()
        $mainWindow = [ApertureMemoryProbeNative]::FindWindow('Aperture_Native_Direct2D_Class', $null)
        $title = if ($mainWindow -ne [IntPtr]::Zero) { [ApertureMemoryProbeNative]::GetTitle($mainWindow) } else { $script:appProcess.MainWindowTitle }
        if (-not $script:appProcess.HasExited -and $title -eq "Aperture") {
            return
        }
        if ([DateTime]::UtcNow -ge $nextRetry -and -not $script:appProcess.HasExited) {
            # The video child can own focus after a D3D11VA startup. Retry the
            # same real Escape action a few times rather than making a single
            # focus handoff decide whether the measurement is valid.
            Send-Escape
            $nextRetry = [DateTime]::UtcNow.AddMilliseconds(350)
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Aperture did not return to the hub after Escape."
}

function Move-ToCard {
    $script:appProcess.Refresh()
    $rect = New-Object ApertureMemoryProbeNative+RECT
    if (-not [ApertureMemoryProbeNative]::GetWindowRect($script:appProcess.MainWindowHandle, [ref]$rect)) {
        throw "Could not read the Aperture window bounds."
    }
    [void][ApertureMemoryProbeNative]::SetForegroundWindow($script:appProcess.MainWindowHandle)
    # The real card surface begins below the hub header in the maximized probe.
    [void][ApertureMemoryProbeNative]::SetCursorPos([int](($rect.Left + $rect.Right) / 2), $rect.Top + 220)
}

$script:appProcess = $null
$samples = [System.Collections.Generic.List[object]]::new()
$testResult = "FAIL"
$failure = $null
$budgetViolations = [System.Collections.Generic.List[string]]::new()
$started = Get-Date

try {
    Copy-StagedRuntime
    Write-Host "Aperture real memory probe" -ForegroundColor Cyan
    Write-Host ("Executable: {0}" -f $executablePath) -ForegroundColor DarkGray
    Write-Host ("Video:      {0}" -f $videoFullPath) -ForegroundColor DarkGray
    Write-Host ("Staged at:  {0}" -f $runDir) -ForegroundColor DarkGray

    $script:appProcess = Start-Process -FilePath $stagedExe -WorkingDirectory $runDir -ArgumentList @('"' + $videoFullPath + '"') -PassThru
    Wait-ForWindow

    Start-Sleep -Seconds ([Math]::Max(1, $PlaybackWarmupSeconds))
    for ($i = 0; $i -lt [Math]::Max(1, $PlaybackSamples); $i++) {
        $samples.Add((Get-MemorySample ("playing-{0}" -f ($i + 1))))
        if ($i + 1 -lt [Math]::Max(1, $PlaybackSamples)) {
            Start-Sleep -Seconds ([Math]::Max(1, $SampleIntervalSeconds))
        }
    }

    Send-Escape
    Wait-ForHub
    Start-Sleep -Seconds 2
    $samples.Add((Get-MemorySample "hub-after-escape"))

    Move-ToCard
    Start-Sleep -Seconds ([Math]::Max(1, $PreviewWarmupSeconds))
    $samples.Add((Get-MemorySample "hub-hover-preview"))

    [void][ApertureMemoryProbeNative]::SetCursorPos(0, 0)
    Start-Sleep -Seconds 2
    $samples.Add((Get-MemorySample "hub-after-preview-stop"))

    $testResult = "PASS"
} catch {
    $failure = $_.Exception.Message
    Write-Host ("Probe failed: {0}" -f $failure) -ForegroundColor Red
} finally {
    if ($null -ne $script:appProcess) {
        try {
            $script:appProcess.Refresh()
            if (-not $script:appProcess.HasExited) {
                $script:appProcess.CloseMainWindow() | Out-Null
                [void]$script:appProcess.WaitForExit(3000)
            }
        } catch {}
        try {
            $script:appProcess.Refresh()
            if (-not $script:appProcess.HasExited) {
                Stop-Process -Id $script:appProcess.Id -Force -ErrorAction SilentlyContinue
            }
        } catch {}
    }

    $playing = @($samples | Where-Object { $_.phase -like "playing-*" -and $_.running })
    $hub = @($samples | Where-Object { $_.phase -eq "hub-after-escape" -and $_.running }) | Select-Object -First 1
    $preview = @($samples | Where-Object { $_.phase -eq "hub-hover-preview" -and $_.running }) | Select-Object -First 1
    $afterPreview = @($samples | Where-Object { $_.phase -eq "hub-after-preview-stop" -and $_.running }) | Select-Object -First 1

    $maxMediaWs = if ($playing.Count) { ($playing | Measure-Object -Property workingSetBytes -Maximum).Maximum } else { 0 }
    $maxPreviewWs = if ($preview) { [int64]$preview.workingSetBytes } else { 0 }
    $idleWs = if ($hub) { [int64]$hub.workingSetBytes } else { 0 }
    if ($maxMediaWs -gt ($MediaWorkingSetBudgetMiB * 1MB)) { $budgetViolations.Add(("playing working set {0:N1} MiB > {1:N1} MiB" -f ($maxMediaWs / 1MB), $MediaWorkingSetBudgetMiB)) }
    if ($idleWs -gt ($IdleWorkingSetBudgetMiB * 1MB)) { $budgetViolations.Add(("hub-after-escape working set {0:N1} MiB > {1:N1} MiB" -f ($idleWs / 1MB), $IdleWorkingSetBudgetMiB)) }
    if ($maxPreviewWs -gt ($PreviewWorkingSetBudgetMiB * 1MB)) { $budgetViolations.Add(("hover-preview working set {0:N1} MiB > {1:N1} MiB" -f ($maxPreviewWs / 1MB), $PreviewWorkingSetBudgetMiB)) }

    $report = [ordered]@{
        test = "aperture-real-memory-probe"
        result = $testResult
        failure = $failure
        startedUtc = $started.ToUniversalTime().ToString("o")
        finishedUtc = (Get-Date).ToUniversalTime().ToString("o")
        executable = $executablePath
        stagedExecutable = $stagedExe
        video = $videoFullPath
        runDirectory = $runDir
        budgetsMiB = [ordered]@{
            mediaWorkingSet = $MediaWorkingSetBudgetMiB
            idleWorkingSet = $IdleWorkingSetBudgetMiB
            previewWorkingSet = $PreviewWorkingSetBudgetMiB
        }
        summary = [ordered]@{
            maxPlayingWorkingSetMiB = [math]::Round($maxMediaWs / 1MB, 2)
            hubAfterEscapeWorkingSetMiB = [math]::Round($idleWs / 1MB, 2)
            hoverPreviewWorkingSetMiB = [math]::Round($maxPreviewWs / 1MB, 2)
            hubAfterPreviewStopWorkingSetMiB = if ($afterPreview) { [math]::Round([int64]$afterPreview.workingSetBytes / 1MB, 2) } else { $null }
            maxPlayingPrivateMiB = if ($playing.Count) { [math]::Round((($playing | Measure-Object -Property privateBytes -Maximum).Maximum) / 1MB, 2) } else { $null }
            maxPlayingDedicatedGpuMiB = if ($playing.Count) { [math]::Round((($playing | Measure-Object -Property dedicatedGpuBytes -Maximum).Maximum) / 1MB, 2) } else { $null }
        }
        budgetViolations = @($budgetViolations)
        samples = @($samples)
    }
    [IO.File]::WriteAllText($ReportPath, ($report | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    Write-Host ("Report: {0}" -f $ReportPath) -ForegroundColor Cyan
    if ($budgetViolations.Count) {
        Write-Host "Budget observations:" -ForegroundColor Yellow
        $budgetViolations | ForEach-Object { Write-Host (" - " + $_) -ForegroundColor Yellow }
    }

    $safeRunPrefix = (Assert-RootLocalPath $StageRoot $repoRoot "StageRoot") + "\Aperture-memory-probe-"
    $resolvedRunDir = Assert-RootLocalPath $runDir $repoRoot "runDir"
    if (-not $KeepArtifacts -and $resolvedRunDir.StartsWith($safeRunPrefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedRunDir)) {
        Remove-Item -LiteralPath $resolvedRunDir -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($KeepArtifacts) {
        Write-Host ("Artifacts retained: {0}" -f $runDir) -ForegroundColor Yellow
    }
}

if ($testResult -ne "PASS") { exit 1 }
if ($FailOnBudget -and $budgetViolations.Count -gt 0) { exit 2 }
exit 0
