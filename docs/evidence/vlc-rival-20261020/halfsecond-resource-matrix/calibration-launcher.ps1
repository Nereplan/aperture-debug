$ErrorActionPreference='Stop'
$repo='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$output=Join-Path $work 'calibration-halfsecond-20261020'
if(Test-Path -LiteralPath $output){throw 'Calibration output already exists.'}
if(Get-Process -Name Aperture,vlc,cl,link -ErrorAction SilentlyContinue){throw 'Player/compiler remains active; calibration not started.'}
. (Join-Path $repo 'tests\benchmark\player_comparison_identity.ps1')
$old=Get-Content -LiteralPath (Join-Path $work 'calibration-20261020\identity.json') -Raw|ConvertFrom-Json
$exe=Join-Path $work 'calibration-20261020\calibrate_cpu_cycles.exe'
$source=Join-Path $repo 'tests\benchmark\calibrate_cpu_cycles.cpp'
$sourceHash=(Get-FileHash -LiteralPath $source).Hash
$exeHash=(Get-FileHash -LiteralPath $exe).Hash
if($sourceHash -ne $old.sourceSha256 -or $exeHash -ne $old.executableSha256){throw 'Calibration source/executable changed since recorded build.'}
New-Item -ItemType Directory -Path $output|Out-Null
$start=[Diagnostics.ProcessStartInfo]::new($exe)
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$p=[Diagnostics.Process]::Start($start)
try {
 $stdout=$p.StandardOutput.ReadToEndAsync()
 $stderr=$p.StandardError.ReadToEndAsync()
 if(-not $p.WaitForExit(15000)){throw 'Calibration did not finish within15s.'}
 $finished=[DateTime]::UtcNow.ToString('o')
 [IO.File]::WriteAllText((Join-Path $output 'cpu-calibration.json'),$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 [IO.File]::WriteAllText((Join-Path $output 'stderr.txt'),$stderr.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 [ordered]@{
  recordedAtUtc=$finished
  sourceSha256=$sourceHash.ToLowerInvariant()
  executableSha256=$exeHash.ToLowerInvariant()
  buildIdentity='../calibration-20261020/identity.json; unchanged existing native executable and source verified before this fresh run'
  environment=(Get-PlayerBenchmarkEnvironment)
  exitCode=$p.ExitCode
  scope='Fresh hardware-specific calibration after the complete half-second-aligned HEVC and AV1 batches. No task-owned player/build/test running concurrently.'
  calibrationSha256=(Get-FileHash -LiteralPath (Join-Path $output 'cpu-calibration.json')).Hash.ToLowerInvariant()
 }|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'identity.json') -Encoding utf8
 if($p.ExitCode -ne 0){throw "Calibration exited $($p.ExitCode)."}
 Write-Output $output
} finally {$p.Dispose()}
