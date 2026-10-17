[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$BaselineDll,
    [Parameter(Mandatory)][string]$CandidateDll,
    [Parameter(Mandatory)][string]$MediaDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$MediaFile = 'h264_aac_720p.mp4',
    [ValidateSet('Renderer','Pacing','Transfers','HardwareThreads','FreeHeapCaches')][string]$Experiment = 'Renderer',
    [switch]$WithAudio,
    [ValidateRange(1,15)][int]$Iterations = 3,
    [ValidateRange(0,5)][int]$WarmupIterations = 1,
    [ValidateRange(1,120)][int]$SampleSeconds = 3,
    [switch]$CaptureFrames
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required.' }
foreach ($name in @('Probe','BaselineDll','CandidateDll','MediaDir')) {
    Set-Variable -Name $name -Value (Resolve-Path -LiteralPath (Get-Variable -Name $name -ValueOnly)).Path
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$variants = @(
    @{name='baseline-gpu'; dll=$BaselineDll; renderer='gpu'},
    @{name='candidate-gpu'; dll=$CandidateDll; renderer='gpu'},
    @{name='candidate-gpu-next'; dll=$CandidateDll; renderer='gpu-next'}
)
if ($Experiment -eq 'Pacing') {
    $variants = @(
        @{name='display-auto'; dll=$CandidateDll; renderer='gpu'; sync='display-vdrop'; threads='0'},
        @{name='audio-auto'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'},
        @{name='display-one-thread'; dll=$CandidateDll; renderer='gpu'; sync='display-vdrop'; threads='1'}
    )
}
if ($Experiment -eq 'Transfers') {
    $variants = @(
        @{name='default-copy'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; zero='no'; latency='yes'},
        @{name='zero-copy'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; zero='yes'; latency='yes'},
        @{name='one-inflight'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; zero='no'; depth='1'; latency='yes'},
        @{name='zero-one-inflight'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; zero='yes'; depth='1'; latency='yes'},
        @{name='normal-latency'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; zero='no'; latency='no'}
    )
}
$results = [Collections.Generic.List[object]]::new()
if ($Experiment -eq 'HardwareThreads') {
    $variants = @(1,2,4 | ForEach-Object {
        @{name="hwdec-$_"; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads=[string]$_}
    })
}
if ($Experiment -eq 'FreeHeapCaches') {
    $variants = @(
        @{name='keep-free-caches'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'},
        @{name='release-free-caches'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; heapCleanup=$true}
    )
}
for ($round=0; $round -lt ($WarmupIterations+$Iterations); $round++) {
    for ($offset=0; $offset -lt $variants.Count; $offset++) {
        $variant = $variants[($round + $offset) % $variants.Count]
        $run = Join-Path $OutputDir ("{0}-{1}-{2}" -f $round,$variant.name,[guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $run | Out-Null
        $exe = Join-Path $run 'probe.exe'
        $dll = Join-Path $run 'libmpv-2.dll'
        Copy-Item -LiteralPath $Probe -Destination $exe
        Copy-Item -LiteralPath $variant.dll -Destination $dll
        $info = [Diagnostics.ProcessStartInfo]::new($exe)
        $info.WorkingDirectory = $run; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
        foreach ($argument in @('--dll',$dll,'--media',$MediaDir,'--scenario','resources',
                '--renderer',$variant.renderer,'--resource-file',$MediaFile)) { $info.ArgumentList.Add($argument) }
        if ($variant.sync) {
            foreach ($argument in @('--video-sync',$variant.sync,'--decoder-threads',$variant.threads)) { $info.ArgumentList.Add($argument) }
        }
        if ($WithAudio) { $info.ArgumentList.Add('--with-audio') }
        if ($variant.heapCleanup) { $info.ArgumentList.Add('--heap-cleanup') }
        foreach ($pair in @(@('--zero-copy',$variant.zero),@('--swapchain-depth',$variant.depth),@('--latency-hacks',$variant.latency),@('--hwdec-threads',$variant.hwthreads))) {
            if ($pair[1]) { $info.ArgumentList.Add($pair[0]); $info.ArgumentList.Add($pair[1]) }
        }
        $info.ArgumentList.Add('--sample-seconds'); $info.ArgumentList.Add([string]$SampleSeconds)
        if ($CaptureFrames) { $info.ArgumentList.Add('--capture-window'); $info.ArgumentList.Add((Join-Path $run 'rendered-frame.png')) }
        foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) {
            $directory = Join-Path $run $name
            New-Item -ItemType Directory -Path $directory | Out-Null
            $info.Environment[$name] = $directory
        }
        $process = [Diagnostics.Process]::Start($info)
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(($SampleSeconds+60)*1000)) { $process.Kill(); throw "Resource probe timed out: $run" }
        $log = $stdout.GetAwaiter().GetResult(); $errorLog = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $run 'stdout.txt'), $log)
        [IO.File]::WriteAllText((Join-Path $run 'stderr.txt'), $errorLog)
        if ($process.ExitCode -ne 0) { throw "Resource probe failed: $run" }
        $match = [regex]::Match($log, '(?m)^RESOURCE (.+)$')
        if (!$match.Success) { throw "No resource sample: $run" }
        $sample = $match.Groups[1].Value | ConvertFrom-Json
        $sample | Add-Member variant $variant.name
        $sample | Add-Member round $round
        $sample | Add-Member warmup ($round -lt $WarmupIterations)
        $sample | Add-Member dll_sha256 (Get-FileHash -LiteralPath $dll).Hash
        if ($CaptureFrames) {
            $capturePath = Join-Path $run 'rendered-frame.png'
            if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) { throw "Missing rendered capture: $run" }
            $sample | Add-Member capture_sha256 (Get-FileHash -LiteralPath $capturePath).Hash
        }
        $results.Add($sample)
        $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDir 'results.json') -Encoding utf8
        Write-Output "$($variant.name), round ${round}: $($sample.restart_ms) ms engine readiness; $($sample.private_bytes) private bytes"
    }
}
