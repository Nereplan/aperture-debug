[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$Media,
    [Parameter(Mandatory)][string]$Sampler,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$Symbols = '',
    [ValidateRange(5,120)][int]$Seconds = 30,
    [switch]$StateOnly,
    [string]$ReturnToHubWithProbe = '',
    [ValidateSet('Aperture','VLC')][string]$Player = 'Aperture',
    [switch]$CpuOnly,
    [switch]$WindowOnly,
    [switch]$ForceD3D11Linear,
    [ValidateRange(320,3840)][int]$WindowWidth = 1280,
    [ValidateRange(240,2160)][int]$WindowHeight = 800,
    [ValidateRange(0,3840)][int]$VideoWidth = 0,
    [ValidateRange(0,2160)][int]$VideoHeight = 0
)
$ErrorActionPreference = 'Stop'
if (([int][bool]$CpuOnly+[int][bool]$StateOnly+[int][bool]$WindowOnly) -gt 1) { throw 'Choose one sampler mode.' }
if (($VideoWidth -eq 0) -ne ($VideoHeight -eq 0)) { throw 'Specify both video canvas dimensions.' }
if ($Player -eq 'VLC' -and ($StateOnly -or $ReturnToHubWithProbe)) { throw 'App-field and return-to-hub checks require Aperture.' }
if (Get-Process -Name $Player -ErrorAction SilentlyContinue) { throw "Existing $Player process; nothing was closed." }
if (-not ('PlayerBenchmarkWindow' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'PlayerBenchmarkWindow.cs') }
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
$exeName = if ($Player -eq 'VLC') { 'vlc.exe' } else { 'Aperture.exe' }
$info = [Diagnostics.ProcessStartInfo]::new((Join-Path $stage $exeName))
$info.WorkingDirectory = $stage; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
if ($Player -eq 'VLC') {
    New-Item -ItemType Directory -Path (Join-Path $stage 'portable') -Force | Out-Null
    foreach ($arg in @('--intf=qt','--ignore-config','--no-one-instance','--no-one-instance-when-started-from-file',
        '--no-media-library','--no-metadata-network-access','--no-qt-privacy-ask','--no-qt-updates-notif','--no-qt-recentplay','--qt-continue=0','--no-qt-video-autoresize')) { $info.ArgumentList.Add($arg) }
}
if ($Player -eq 'VLC' -and $ForceD3D11Linear) {
    foreach ($arg in @('--vout=direct3d11','--avcodec-hw=d3d11va','--d3d11-upscale-mode=linear')) { $info.ArgumentList.Add($arg) }
}
$info.ArgumentList.Add($mediaPath)
foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) {
    $path = Join-Path $profileRoot $name
    New-Item -ItemType Directory -Path $path | Out-Null
    $info.Environment[$name] = $path
}
$process = [Diagnostics.Process]::Start($info)
try {
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        if ($process.HasExited) { throw "$Player exited before showing a window." }
        $window = [PlayerBenchmarkWindow]::Find([uint32]$process.Id)
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $windowDeadline)
    if ($window -eq [IntPtr]::Zero -or -not [PlayerBenchmarkWindow]::Resize($window,[uint32]$process.Id,$WindowWidth,$WindowHeight)) { throw 'Owned-window setup failed.' }
    Start-Sleep -Seconds 8
    if (-not [PlayerBenchmarkWindow]::Resize($window,[uint32]$process.Id,$WindowWidth,$WindowHeight)) { throw 'Post-load resize failed.' }
    if ($VideoWidth -and -not [PlayerBenchmarkWindow]::MatchVideoCanvas($window,[uint32]$process.Id,$Player,$VideoWidth,$VideoHeight)) { throw 'Video canvas matching failed.' }
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited) { throw 'Aperture exited during settling.' }
    if ($WindowOnly) {
        [ordered]@{player=$Player;pid=$process.Id;runtime=$runtimeSource;media=$mediaPath;
            executableSha256=(Get-FileHash -LiteralPath (Join-Path $stage $exeName)).Hash;
            outerDimensions=[PlayerBenchmarkWindow]::Size($window);
            children=[PlayerBenchmarkWindow]::Children($window,[uint32]$process.Id);
            method='Owned-window geometry only; no CPU sampling or ranking.'
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $profileRoot 'window-layout.json') -Encoding utf8
        Write-Output (Join-Path $profileRoot 'window-layout.json')
        return
    }
    $sampleInfo = [Diagnostics.ProcessStartInfo]::new($samplerPath)
    $sampleInfo.WorkingDirectory=$profileRoot; $sampleInfo.UseShellExecute=$false; $sampleInfo.CreateNoWindow=$true
    $sampleInfo.RedirectStandardOutput=$true; $sampleInfo.RedirectStandardError=$true
    foreach ($arg in @([string]$process.Id,[string]$Seconds,$Symbols)) { $sampleInfo.ArgumentList.Add($arg) }
    if ($StateOnly) { $sampleInfo.ArgumentList.Add('--state-only') }
    if ($CpuOnly) { $sampleInfo.ArgumentList.Add('--cpu-only') }
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
    if ($process.HasExited) { throw "$Player exited before the diagnostic completed." }
    [ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o');pid=$process.Id;runtime=$runtimeSource;
        executableSha256=(Get-FileHash -LiteralPath $info.FileName -Algorithm SHA256).Hash;
        mediaSha256=(Get-FileHash -LiteralPath $mediaPath -Algorithm SHA256).Hash;
        privateBytes=$process.PrivateMemorySize64;workingSetBytes=$process.WorkingSet64;
        player=$Player;windowDimensions=[PlayerBenchmarkWindow]::Size($window);arguments=[string[]]@($info.ArgumentList);
        stateOnly=[bool]$StateOnly;cpuOnly=[bool]$CpuOnly;returnedToHub=[bool]$ReturnToHubWithProbe;
        experimentalWaitPolicy=$env:APERTURE_TEST_ROUND_WAITS;
        videoCanvas=[PlayerBenchmarkWindow]::VideoCanvas($window,[uint32]$process.Id,$Player);
        note='Diagnostic run with a fresh portable profile; requested geometry is retained. Raw thread cycles are not a portable duration. Stack mode suspends; CPU/state modes do not. Not a ranking or output-equivalence check.'} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $profileRoot 'profile-identity.json') -Encoding utf8
    Write-Output "Profile saved: $profileRoot"
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(30000)) { $process.Kill() }
    }
    $process.Dispose()
}
