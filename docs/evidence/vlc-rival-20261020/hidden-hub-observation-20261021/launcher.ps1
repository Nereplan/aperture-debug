$ErrorActionPreference='Stop'
$repo='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$out=Join-Path $work 'hidden-hub-observation-20261021'
if(Test-Path -LiteralPath $out){throw 'Choose a fresh observation directory.'}
if(Get-Process -Name Aperture,vlc,cl,link -ErrorAction SilentlyContinue){throw 'Existing player/compiler; nothing launched.'}
$raw=Get-Content -LiteralPath (Join-Path $work 'h264-hub-final-clean-20261020\comparison.json') -Raw|ConvertFrom-Json
$probe=Join-Path (Split-Path -Parent $raw.results[0].runDirectory) 'player_status_probe.exe'
if((Get-FileHash -LiteralPath $probe).Hash -ne $raw.apertureStatusProbeSha256){throw 'Owned-PID probe changed.'}
$sampler=Join-Path $work 'hidden-hub-probe-20261021\profile_hidden_hub.exe'
$symbols=Join-Path $work 'candidate-qualified-build'
$source=Join-Path $work 'candidate-runtime-qualified'
New-Item -ItemType Directory -Path $out|Out-Null
$runtime=Join-Path $out 'runtime';New-Item -ItemType Directory -Path $runtime|Out-Null
Get-ChildItem -LiteralPath $source -Force|Copy-Item -Destination $runtime -Recurse
foreach($name in 'data','cache'){New-Item -ItemType Directory -Path (Join-Path $runtime $name)|Out-Null}
$exe=Join-Path $runtime 'Aperture.exe'
if((Get-FileHash -LiteralPath $exe).Hash -ne '78b59f4e3e98e3f5f7a3e3a309c522f3ce1828e3f58e22c1a85a12c008fa9a7f'){throw 'Frozen candidate changed.'}
Add-Type -Path (Join-Path $repo 'tests\benchmark\PlayerBenchmarkWindow.cs')
$start=[Diagnostics.ProcessStartInfo]::new($exe)
$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WorkingDirectory=$runtime
foreach($name in 'TEMP','TMP','APPDATA','LOCALAPPDATA'){$p=Join-Path $out $name;New-Item -ItemType Directory -Path $p|Out-Null;$start.Environment[$name]=$p}
$player=[Diagnostics.Process]::Start($start);$observer=$null;$window=[IntPtr]::Zero
$identity=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');sourceCommit='23fd026c2b114f8391465bfb5f7d28a79da02025';pid=$player.Id;scope='Owned visible HubThenOpen AV1 state/memory diagnostic; observer overhead prevents ranking or causal allocation attribution.';executableSha256=(Get-FileHash -LiteralPath $exe).Hash;samplerSha256=(Get-FileHash -LiteralPath $sampler).Hash;samplerSourceSha256=(Get-FileHash -LiteralPath (Join-Path $work 'hidden-hub-probe-20261021\profile_hidden_hub.cpp')).Hash;pdbSha256=(Get-FileHash -LiteralPath (Join-Path $symbols 'Aperture.pdb')).Hash;pdbProvenance='Stored beside the original qualified build. Current hash recorded; no independently saved historical PDB hash. DbgHelp must resolve matching module/type fields.';cleanup='pending'}
try {
 $deadline=[DateTime]::UtcNow.AddSeconds(15)
 do {$window=[PlayerBenchmarkWindow]::Find([uint32]$player.Id);if($window -ne [IntPtr]::Zero){break};if($player.HasExited){throw 'Player exited early.'};Start-Sleep -Milliseconds 25}while([DateTime]::UtcNow -lt $deadline)
 if($window -eq [IntPtr]::Zero -or -not [PlayerBenchmarkWindow]::Resize($window,[uint32]$player.Id,1280,800) -or -not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Owned window setup failed.'}
 $sampleStart=[Diagnostics.ProcessStartInfo]::new($sampler);$sampleStart.UseShellExecute=$false;$sampleStart.CreateNoWindow=$true;$sampleStart.RedirectStandardOutput=$true;$sampleStart.RedirectStandardError=$true
 foreach($arg in @([string]$player.Id,'36',$symbols,'--state-only')){$sampleStart.ArgumentList.Add($arg)}
 $identity.observerStartUtc=[DateTime]::UtcNow.ToString('o');$observer=[Diagnostics.Process]::Start($sampleStart);$stdout=$observer.StandardOutput.ReadToEndAsync();$stderr=$observer.StandardError.ReadToEndAsync()
 Start-Sleep -Seconds 3
 $identity.openQpcStart=[Diagnostics.Stopwatch]::GetTimestamp()
 $media='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017\av1-1080p30-audio.mp4'
 $open=& $probe $player.Id --open $media
 if($LASTEXITCODE -ne 0){throw 'Owned media open failed.'};$identity.openStatus=$open|ConvertFrom-Json;$identity.openQpcEnd=[Diagnostics.Stopwatch]::GetTimestamp()
 Start-Sleep -Seconds 8
 if(-not [PlayerBenchmarkWindow]::MatchVideoCanvas($window,[uint32]$player.Id,'Aperture',1280,720) -or -not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Playback canvas setup failed.'}
 $identity.canvas=[PlayerBenchmarkWindow]::VideoCanvas($window,[uint32]$player.Id,'Aperture')
 Start-Sleep -Seconds 17
 $playing=& $probe $player.Id
 if($LASTEXITCODE -ne 0){throw 'Owned playback status failed.'};$identity.playbackStatus=$playing|ConvertFrom-Json
 $identity.stopQpcStart=[Diagnostics.Stopwatch]::GetTimestamp();$stopped=& $probe $player.Id --stop
 if($LASTEXITCODE -ne 0 -or ($stopped|ConvertFrom-Json).state -ne 0){throw 'Owned return to library failed.'}
 $identity.stopStatus=$stopped|ConvertFrom-Json;$identity.stopQpcEnd=[Diagnostics.Stopwatch]::GetTimestamp()
 if(-not $observer.WaitForExit(15000)){throw 'State observer exceeded its deadline.'}
 [IO.File]::WriteAllText((Join-Path $out 'state.json'),$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 [IO.File]::WriteAllText((Join-Path $out 'observer-stderr.txt'),$stderr.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 if($observer.ExitCode -ne 0){throw ('State observer exited '+$observer.ExitCode)}
 $state=Get-Content -LiteralPath (Join-Path $out 'state.json') -Raw|ConvertFrom-Json
 if(-not $state.appFieldsAvailable -or $state.snapshots.Count -lt 30 -or @($state.snapshots|Where-Object {$null -eq $_}).Count){throw 'Missing authoritative PDB field observations.'}
 $identity.status='observed';$identity.qpcFrequency=[Diagnostics.Stopwatch]::Frequency
} catch {$identity.status='failed';$identity.failure=$_.Exception.Message;throw}
finally {
 if($observer -and -not $observer.HasExited){$observer.Kill();$observer.WaitForExit()}
 if(-not $player.HasExited){[PlayerBenchmarkWindow]::Close($window,[uint32]$player.Id);if(-not $player.WaitForExit(5000)){$player.Kill();$player.WaitForExit();$identity.cleanup='owned-pid-killed'}else{$identity.cleanup='owned-window-closed'}}else{$identity.cleanup='owned-process-exited'}
 $identity|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
 if($observer){$observer.Dispose()};$player.Dispose()
}
Write-Output 'Hidden hub observation completed; raw state and memory retained.'