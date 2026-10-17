[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$BaselineDll,
    [Parameter(Mandatory)][string]$CandidateDll,
    [Parameter(Mandatory)][string]$MediaDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$MediaFile = 'h264_aac_720p.mp4',
    [ValidateSet('Renderer','Pacing')][string]$Experiment = 'Renderer',
    [switch]$WithAudio,
    [ValidateRange(1,15)][int]$Iterations = 3
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
$results = [Collections.Generic.List[object]]::new()
for ($round=0; $round -le $Iterations; $round++) {
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
        foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) {
            $directory = Join-Path $run $name
            New-Item -ItemType Directory -Path $directory | Out-Null
            $info.Environment[$name] = $directory
        }
        $process = [Diagnostics.Process]::Start($info)
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(60000)) { $process.Kill(); throw "Resource probe timed out: $run" }
        $log = $stdout.GetAwaiter().GetResult(); $errorLog = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $run 'stdout.txt'), $log)
        [IO.File]::WriteAllText((Join-Path $run 'stderr.txt'), $errorLog)
        if ($process.ExitCode -ne 0) { throw "Resource probe failed: $run" }
        $match = [regex]::Match($log, '(?m)^RESOURCE (.+)$')
        if (!$match.Success) { throw "No resource sample: $run" }
        $sample = $match.Groups[1].Value | ConvertFrom-Json
        $sample | Add-Member variant $variant.name
        $sample | Add-Member round $round
        $sample | Add-Member warmup ($round -eq 0)
        $sample | Add-Member dll_sha256 (Get-FileHash -LiteralPath $dll).Hash
        $results.Add($sample)
        $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDir 'results.json') -Encoding utf8
        Write-Output "$($variant.name), round ${round}: $($sample.restart_ms) ms engine readiness; $($sample.private_bytes) private bytes"
    }
}
