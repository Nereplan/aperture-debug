$ErrorActionPreference='Stop'
$repo='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$out=Join-Path $work 'cadence-preflight-20261021'
if(Test-Path -LiteralPath $out){throw 'Cadence output must be fresh.'}
if(Get-Process -Name Aperture,vlc,cl,link -ErrorAction SilentlyContinue){throw 'Player/compiler already running.'}
$raw=Get-Content -LiteralPath (Join-Path $work 'h264-hub-final-clean-20261020\comparison.json') -Raw|ConvertFrom-Json
$probe=Join-Path (Split-Path -Parent $raw.results[0].runDirectory) 'player_status_probe.exe'
if((Get-FileHash -LiteralPath $probe).Hash -ne $raw.apertureStatusProbeSha256){throw 'Existing owned-PID probe changed.'}
$source=Join-Path $work 'candidate-runtime-qualified'
$pm='C:\Users\Alperen\AppData\Local\Codex\tools\PresentMon\2.6.0\PresentMon-2.6.0-x64.exe'
if((Get-FileHash -LiteralPath $pm).Hash -ne 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'){throw 'PresentMon hash changed.'}
Add-Type -Path (Join-Path $repo 'tests\benchmark\PlayerBenchmarkWindow.cs')
New-Item -ItemType Directory -Path $out|Out-Null
$runtime=Join-Path $out 'runtime'
New-Item -ItemType Directory -Path $runtime|Out-Null
Get-ChildItem -LiteralPath $source -Force|Copy-Item -Destination $runtime -Recurse
foreach($name in 'data','cache'){New-Item -ItemType Directory -Path (Join-Path $runtime $name)|Out-Null}
$exe=Join-Path $runtime 'Aperture.exe'
if((Get-FileHash -LiteralPath $exe).Hash -ne '78b59f4e3e98e3f5f7a3e3a309c522f3ce1828e3f58e22c1a85a12c008fa9a7f'){throw 'Frozen candidate changed.'}
$media='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017\h264-1080p30-audio.mp4'
$start=[Diagnostics.ProcessStartInfo]::new($exe)
$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WorkingDirectory=$runtime
$start.ArgumentList.Add($media)
foreach($name in 'APPDATA','LOCALAPPDATA','TEMP','TMP'){
 $path=Join-Path $out $name;New-Item -ItemType Directory -Path $path|Out-Null;$start.Environment[$name]=$path
}
$player=[Diagnostics.Process]::Start($start)
$identity=[ordered]@{recordedAtUtc=[DateTime]::UtcNow.ToString('o');scope='Normal-permission ETW capture preflight for a coordinator-owned visible player; not a resource, cadence or quality ranking.';pid=$player.Id;sourceCommit='23fd026c2b114f8391465bfb5f7d28a79da02025';executableSha256=(Get-FileHash -LiteralPath $exe).Hash;presentMonSha256=(Get-FileHash -LiteralPath $pm).Hash;cleanup='pending'}
$pmExit=1;$window=[IntPtr]::Zero
try {
 $deadline=[DateTime]::UtcNow.AddSeconds(15)
 do {
  if($player.HasExited){throw 'Owned player exited before window setup.'}
  $window=[PlayerBenchmarkWindow]::Find([uint32]$player.Id)
  if($window -ne [IntPtr]::Zero){break}
  Start-Sleep -Milliseconds 25
 } while([DateTime]::UtcNow -lt $deadline)
 if($window -eq [IntPtr]::Zero -or -not [PlayerBenchmarkWindow]::Resize($window,[uint32]$player.Id,1280,800) -or -not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Owned visible window setup failed.'}
 Start-Sleep -Seconds 8
 if(-not [PlayerBenchmarkWindow]::MatchVideoCanvas($window,[uint32]$player.Id,'Aperture',1280,720)){throw 'Canvas matching failed.'}
 if(-not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Owned window raise failed.'}
 Start-Sleep -Seconds 2
 $identity.canvas=[PlayerBenchmarkWindow]::VideoCanvas($window,[uint32]$player.Id,'Aperture')
 $before=& $probe $player.Id
 if($LASTEXITCODE -ne 0){throw 'Owned player status failed.'}
 $identity.before=$before|ConvertFrom-Json
 $captureArgs=@('--process_id',[string]$player.Id,'--session_name',('ApertureCadence-'+[guid]::NewGuid().ToString('N')),'--output_file',(Join-Path $out 'presents.csv'),'--timed','3','--terminate_after_timed','--qpc_time','--write_display_metadata','--no_console_stats','--no_track_gpu','--no_track_input')
 $identity.presentMonArguments=$captureArgs;$identity.captureStartUtc=[DateTime]::UtcNow.ToString('o')
 & $pm @captureArgs *> (Join-Path $out 'presentmon.log')
 $pmExit=$LASTEXITCODE;$identity.presentMonExit=$pmExit;$identity.captureEndUtc=[DateTime]::UtcNow.ToString('o')
 $after=& $probe $player.Id
 if($LASTEXITCODE -ne 0){throw 'Post-capture owned player status failed.'}
 $identity.after=$after|ConvertFrom-Json
 $identity.csvExists=Test-Path -LiteralPath (Join-Path $out 'presents.csv')
 $identity.lossWarning=[bool](Select-String -LiteralPath (Join-Path $out 'presentmon.log') -Pattern 'ETW (buffers|events) were lost|overflowed present events detected' -Quiet)
 $identity.status=if($pmExit -eq 0 -and $identity.csvExists -and -not $identity.lossWarning){'captured-preflight-only'}else{'unavailable'}
} catch {$identity.status='failed';$identity.failure=$_.Exception.Message;throw}
finally {
 if(-not $player.HasExited){[PlayerBenchmarkWindow]::Close($window,[uint32]$player.Id);if(-not $player.WaitForExit(5000)){$player.Kill();$player.WaitForExit();$identity.cleanup='owned-pid-killed'}else{$identity.cleanup='owned-window-closed'}}else{$identity.cleanup='owned-process-exited'}
 $identity|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
 $player.Dispose()
}
Write-Output ('PresentMon preflight: '+$identity.status+'; exit='+$pmExit)
Get-Content -LiteralPath (Join-Path $out 'presentmon.log') -Tail 10
exit $pmExit