# Recheck saved Aperture runtimes without rebuilding or touching their profiles.
# This measures only the first visible window, not video readiness or playback.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$OutputDir,
    [ValidateRange(3,100)][int]$Samples = 15
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required.' }
if (Get-Process -Name Aperture -ErrorAction SilentlyContinue) { throw 'Close existing Aperture instances before this benchmark; none were closed.' }
$definition = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$auditRoot = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\')
if (Test-Path -LiteralPath $auditRoot) { throw 'OutputDir must be a new directory.' }
$media = (Resolve-Path -LiteralPath $definition.media).Path
if ((Get-FileHash -LiteralPath $media -Algorithm SHA256).Hash -ne $definition.mediaSha256) { throw 'Media hash mismatch.' }
$inputs = @($definition.variants)
if ($inputs.Count -lt 2) { throw 'At least two saved runtimes are required.' }
foreach ($variant in $inputs) {
    if ($variant.id -notmatch '^[a-z0-9-]+$') { throw 'Invalid variant ID.' }
    $source = (Resolve-Path -LiteralPath $variant.runtime).Path
    if ($auditRoot.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $source.StartsWith($auditRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime and output directories must not overlap.' }
    if (Get-ChildItem -LiteralPath $source -Force -Recurse -Attributes ReparsePoint -ErrorAction SilentlyContinue) { throw 'Reparse points in runtime.' }
    foreach ($file in @('Aperture.exe','libmpv-2.dll')) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $source $file) -Algorithm SHA256).Hash
        if ($hash -ne $variant.hashes.$file) { throw "Saved binary hash mismatch: $($variant.id)/$file" }
    }
}
New-Item -ItemType Directory -Path $auditRoot | Out-Null
function Assert-AuditPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($auditRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Path escaped audit directory.' }
    return $full
}
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
public static class SavedStartupAudit {
    public delegate bool EnumProc(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr context);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateWaitableTimerEx(IntPtr attrs, string name, uint flags, uint access);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetWaitableTimer(IntPtr timer, ref long dueTime, int period, IntPtr routine, IntPtr arg, bool resume);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint timeout);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public sealed class Reading {
        public Process Process;
        public IntPtr Window;
        public double LastNotVisibleMs, FirstVisibleMs;
        public int Checks;
    }
    static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, context) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == pid && IsWindowVisible(window) && GetWindow(window, 4) == IntPtr.Zero) {
                found = window; return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static Reading Launch(ProcessStartInfo info) {
        var reading = new Reading();
        // Thread.Sleep(1) can wake at ~16 ms. This timer is local to the observer
        // and does not change the system-wide timer resolution.
        var waitTimer = CreateWaitableTimerEx(IntPtr.Zero, null, 2, 0x1F0003);
        if (waitTimer == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        var timer = Stopwatch.StartNew();
        try {
            reading.Process = Process.Start(info);
            while (timer.Elapsed.TotalSeconds < 15) {
                if (reading.Process.HasExited) throw new Exception("Exited before showing a window.");
                double checkStart = timer.Elapsed.TotalMilliseconds;
                reading.Window = Find((uint)reading.Process.Id);
                reading.Checks++;
                if (reading.Window != IntPtr.Zero) {
                    reading.FirstVisibleMs = timer.Elapsed.TotalMilliseconds;
                    return reading;
                }
                reading.LastNotVisibleMs = checkStart;
                long due = -10000;
                if (!SetWaitableTimer(waitTimer, ref due, 0, IntPtr.Zero, IntPtr.Zero, false)) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (WaitForSingleObject(waitTimer, 1000) != 0) throw new Exception("Observer timer failed.");
            }
            throw new Exception("Visible window timeout.");
        } catch {
            if (reading.Process != null) {
                if (!reading.Process.HasExited) reading.Process.Kill();
                reading.Process.Dispose();
            }
            throw;
        } finally { CloseHandle(waitTimer); }
    }
    public static void Close(Reading reading) {
        uint owner; GetWindowThreadProcessId(reading.Window, out owner);
        if (owner == reading.Process.Id) PostMessage(reading.Window, 0x0010, IntPtr.Zero, IntPtr.Zero);
        if (!reading.Process.WaitForExit(30000)) { reading.Process.Kill(); throw new Exception("Owned process did not close."); }
    }
}
'@
$results = [Collections.Generic.List[object]]::new()
$started = [DateTime]::UtcNow.ToString('o')
for ($round = 0; $round -le $Samples; $round++) {
    $order = if ($round % 2) { @($inputs | Sort-Object id -Descending) } else { @($inputs | Sort-Object id) }
    for ($offset = 0; $offset -lt $order.Count; $offset++) {
        $variant = $order[($round + $offset) % $order.Count]
        if (Get-Process -Name Aperture -ErrorAction SilentlyContinue) { throw 'Unexpected existing Aperture process.' }
        $archive = Assert-AuditPath (Join-Path $auditRoot ("trials/{0}-{1}" -f $round,$variant.id))
        New-Item -ItemType Directory -Path $archive -Force | Out-Null
        $stage = Assert-AuditPath (Join-Path $archive 'runtime')
        New-Item -ItemType Directory -Path $stage | Out-Null
        Get-ChildItem -LiteralPath $variant.runtime -Force |
            Where-Object Name -NotIn @('data','cache','diagnostics') |
            Copy-Item -Destination $stage -Recurse
        foreach ($name in @('data','cache')) { New-Item -ItemType Directory -Path (Join-Path $stage $name) | Out-Null }
        $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $stage 'Aperture.exe'))
        $info.WorkingDirectory = $stage; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.ArgumentList.Add($media)
        foreach ($name in @('APPDATA','LOCALAPPDATA','TEMP','TMP')) {
            $path = Assert-AuditPath (Join-Path $archive $name)
            New-Item -ItemType Directory -Path $path | Out-Null
            $info.Environment[$name] = $path
        }
        $reading = [SavedStartupAudit]::Launch($info)
        try {
            # Let the app finish its initialization before asking its own window to close.
            Start-Sleep -Milliseconds 300
            [SavedStartupAudit]::Close($reading)
            $results.Add([pscustomobject]@{
                variant=$variant.id; round=$round; warmup=($round -eq 0);
                lastNotVisibleMs=$reading.LastNotVisibleMs; firstVisibleMs=$reading.FirstVisibleMs;
                observationIntervalMs=$reading.FirstVisibleMs-$reading.LastNotVisibleMs;
                checks=$reading.Checks; exitCode=$reading.Process.ExitCode
            })
        } finally { $reading.Process.Dispose() }
        # Delete only this closed trial's generated runtime, after resolving and
        # checking its absolute path stays within the new audit directory.
        $cleanupPath = Assert-AuditPath (Resolve-Path -LiteralPath $stage).Path
        Remove-Item -LiteralPath $cleanupPath -Recurse -Force
    }
    $record = [ordered]@{
        startedUtc=$started; createdUtc=[DateTime]::UtcNow.ToString('o');
        method='Saved EXE/DLL runtimes; full runtime copied to a fresh path with fresh executable-local data/cache and child temp/profile per launch, matching the original copy policy; one warmup each; rotated/reversed order; warm OS caches. Normal launch with the same no-audio video. C# owned-window predicate matches the legacy harness; local high-resolution waitable timer requests 1 ms, with actual observation bounds recorded. No first-frame, playback-resource, or code-cause claim.';
        inputs=$definition; samplesPerVariant=$Samples; results=$results.ToArray()
    }
    $record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $auditRoot 'startup-audit.json') -Encoding utf8
    Write-Output "Completed round $round/$Samples."
}
