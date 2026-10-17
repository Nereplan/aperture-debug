[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$Media,
    [Parameter(Mandatory)][string]$Sampler,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Symbols = '',
    [ValidateRange(5,120)][int]$Seconds = 30,
    [switch]$StateOnly,
    [string]$ReturnToHubWithProbe = ''
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name Aperture -ErrorAction SilentlyContinue) { throw 'Existing Aperture process; nothing was closed.' }
$runtimeSource = (Resolve-Path -LiteralPath $Runtime).Path
$mediaPath = (Resolve-Path -LiteralPath $Media).Path
$samplerPath = (Resolve-Path -LiteralPath $Sampler).Path
if ($ReturnToHubWithProbe) { $ReturnToHubWithProbe = (Resolve-Path -LiteralPath $ReturnToHubWithProbe).Path }
$profileRoot = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $profileRoot) { throw 'OutputDir must be new.' }
if ($profileRoot.StartsWith($runtimeSource.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Output cannot be inside the source runtime.' }
New-Item -ItemType Directory -Path $profileRoot | Out-Null
$stage = Join-Path $profileRoot 'runtime'
New-Item -ItemType Directory -Path $stage | Out-Null
Get-ChildItem -LiteralPath $runtimeSource -Force | Where-Object Name -NotIn @('data','cache','diagnostics') | Copy-Item -Destination $stage -Recurse
foreach ($name in @('data','cache')) { New-Item -ItemType Directory -Path (Join-Path $stage $name) | Out-Null }
$info = [Diagnostics.ProcessStartInfo]::new((Join-Path $stage 'Aperture.exe'))
$info.WorkingDirectory = $stage; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
$info.ArgumentList.Add($mediaPath)
foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) {
    $path = Join-Path $profileRoot $name
    New-Item -ItemType Directory -Path $path | Out-Null
    $info.Environment[$name] = $path
}
$process = [Diagnostics.Process]::Start($info)
try {
    Start-Sleep -Seconds 10
    $process.Refresh()
    if ($process.HasExited) { throw 'Aperture exited during settling.' }
    $sampleInfo = [Diagnostics.ProcessStartInfo]::new($samplerPath)
    $sampleInfo.WorkingDirectory=$profileRoot; $sampleInfo.UseShellExecute=$false; $sampleInfo.CreateNoWindow=$true
    $sampleInfo.RedirectStandardOutput=$true; $sampleInfo.RedirectStandardError=$true
    foreach ($arg in @([string]$process.Id,[string]$Seconds,$Symbols)) { $sampleInfo.ArgumentList.Add($arg) }
    if ($StateOnly) { $sampleInfo.ArgumentList.Add('--state-only') }
    $samplerProcess = [Diagnostics.Process]::Start($sampleInfo)
    try {
        $stdout = $samplerProcess.StandardOutput.ReadToEndAsync(); $stderr=$samplerProcess.StandardError.ReadToEndAsync()
        if (-not $samplerProcess.WaitForExit(($Seconds+60)*1000)) { $samplerProcess.Kill(); throw 'Diagnostic sampler timed out.' }
        [IO.File]::WriteAllText((Join-Path $profileRoot 'thread-profile.json'),$stdout.GetAwaiter().GetResult())
        [IO.File]::WriteAllText((Join-Path $profileRoot 'sampler-errors.txt'),$stderr.GetAwaiter().GetResult())
        if ($samplerProcess.ExitCode) { throw "Sampler failed ($($samplerProcess.ExitCode))." }
    } finally { $samplerProcess.Dispose() }
    if ($ReturnToHubWithProbe) {
        $stopInfo = [Diagnostics.ProcessStartInfo]::new($ReturnToHubWithProbe)
        $stopInfo.UseShellExecute=$false; $stopInfo.CreateNoWindow=$true; $stopInfo.RedirectStandardOutput=$true
        $stopInfo.ArgumentList.Add([string]$process.Id); $stopInfo.ArgumentList.Add('--stop')
        foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) { $stopInfo.Environment[$name] = $info.Environment[$name] }
        $stopProcess = [Diagnostics.Process]::Start($stopInfo)
        try {
            if (-not $stopProcess.WaitForExit(10000)) { $stopProcess.Kill(); throw 'Owned-player stop timed out.' }
            $statusText = $stopProcess.StandardOutput.ReadToEnd()
            [IO.File]::WriteAllText((Join-Path $profileRoot 'hub-status.json'),$statusText)
            if ($stopProcess.ExitCode -ne 0 -or ($statusText | ConvertFrom-Json).state -ne 0) { throw 'Playback did not close into the library.' }
        } finally { $stopProcess.Dispose() }
        Start-Sleep -Seconds 2
        $sampleInfo.ArgumentList.Clear()
        foreach ($arg in @([string]$process.Id,'5',$Symbols,'--state-only')) { $sampleInfo.ArgumentList.Add($arg) }
        $hubProbe = [Diagnostics.Process]::Start($sampleInfo)
        try {
            $stdout = $hubProbe.StandardOutput.ReadToEndAsync(); $stderr = $hubProbe.StandardError.ReadToEndAsync()
            if (-not $hubProbe.WaitForExit(30000)) { $hubProbe.Kill(); throw 'Hub state sampler timed out.' }
            $stateText = $stdout.GetAwaiter().GetResult()
            [IO.File]::WriteAllText((Join-Path $profileRoot 'hub-state.json'),$stateText)
            [IO.File]::WriteAllText((Join-Path $profileRoot 'hub-sampler-errors.txt'),$stderr.GetAwaiter().GetResult())
            $hubState = $stateText | ConvertFrom-Json
            if ($hubProbe.ExitCode -ne 0 -or -not $hubState.appFieldsAvailable -or -not $hubState.snapshots.Count -or
                @($hubState.snapshots | Where-Object { -not $_.hubPresented }).Count) { throw 'Library did not present after closing direct playback.' }
        } finally { $hubProbe.Dispose() }
    }
    $process.Refresh()
    [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o');pid=$process.Id;runtime=$runtimeSource;
        executableSha256=(Get-FileHash -LiteralPath $info.FileName -Algorithm SHA256).Hash;
        mediaSha256=(Get-FileHash -LiteralPath $mediaPath -Algorithm SHA256).Hash;
        privateBytes=$process.PrivateMemorySize64;workingSetBytes=$process.WorkingSet64;
        stateOnly=[bool]$StateOnly;returnedToHub=[bool]$ReturnToHubWithProbe;
        note='Diagnostic run; stack mode suspends threads, state-only mode reads PDB fields. Neither is a ranking run.'} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $profileRoot 'profile-identity.json') -Encoding utf8
    Write-Output "Profile saved: $profileRoot"
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(30000)) { $process.Kill() }
    }
    $process.Dispose()
}
