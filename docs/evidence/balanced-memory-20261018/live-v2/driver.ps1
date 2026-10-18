$ErrorActionPreference='Stop'
$perf='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017'
if(Get-Process -Name Aperture -ErrorAction SilentlyContinue) { throw 'Existing Aperture process; nothing was closed.' }
$out=Join-Path $perf 'overlay-surface-live-v2'
if(Test-Path -LiteralPath $out) { throw 'Preserving prior live test.' }
New-Item -ItemType Directory -Path $out | Out-Null
$runtime=Join-Path $out 'runtime'
New-Item -ItemType Directory -Path $runtime | Out-Null
Get-ChildItem -LiteralPath (Join-Path $perf 'overlay-surface-runtime') -Force | Copy-Item -Destination $runtime -Recurse
$info=[Diagnostics.ProcessStartInfo]::new((Join-Path $runtime 'Aperture.exe'))
$info.UseShellExecute=$false; $info.CreateNoWindow=$true; $info.WorkingDirectory=$runtime
foreach($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) { $dir=Join-Path $out $name;New-Item -ItemType Directory -Path $dir | Out-Null;$info.Environment[$name]=$dir }
$info.ArgumentList.Add((Join-Path $perf 'h264-1080p30-audio.mp4'))
$process=[Diagnostics.Process]::Start($info)
$statusProbe=Join-Path $perf 'overlay-surface-probes-v2/player_status_probe.exe'
$sampler=Join-Path $perf 'overlay-surface-probes-v2/profile_player.exe'
$symbols=Join-Path $perf 'overlay-surface-symbols'
$events=[Collections.Generic.List[object]]::new()
function Status([string]$Action='', [string]$Media='') {
 $arguments=@([string]$process.Id);if($Action) { $arguments+=@($Action) };if($Media) { $arguments+=@($Media) }
 $text=& $statusProbe @arguments
 if($LASTEXITCODE -ne 0) { throw 'Owned-PID status command failed.' }
 $value=$text | ConvertFrom-Json
 $events.Add(@{utc=[DateTime]::UtcNow.ToString('o');action=$Action;status=$value})
 return $value
}
function State([string]$Name,[bool]$PulsePause=$false) {
 $si=[Diagnostics.ProcessStartInfo]::new($sampler);$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
 foreach($arg in @([string]$process.Id,'5',$symbols,'--state-only')) { $si.ArgumentList.Add($arg) }
 $sample=[Diagnostics.Process]::Start($si);$text=$sample.StandardOutput.ReadToEndAsync();$errors=$sample.StandardError.ReadToEndAsync()
 try {
  $deadline=[DateTime]::UtcNow.AddSeconds(15)
  while(-not $sample.WaitForExit(200)) { if([DateTime]::UtcNow -gt $deadline) { $sample.Kill();throw 'State sampler timeout.' };if($PulsePause) { $null=Status '--pause' } }
  $sample.WaitForExit();if($sample.ExitCode -ne 0) { throw 'State sampler failed.' }
  $raw=$text.GetAwaiter().GetResult();[IO.File]::WriteAllText((Join-Path $out ($Name+'.json')),$raw);[IO.File]::WriteAllText((Join-Path $out ($Name+'-errors.txt')),$errors.GetAwaiter().GetResult())
  $value=$raw | ConvertFrom-Json
  if(-not $value.appFieldsAvailable -or $value.snapshots.Count -lt 3 -or @($value.snapshots | Where-Object overlayPresentationFailures -NE 0).Count) { throw 'Missing app fields or an overlay presentation failure.' }
  return $value
 } finally { $sample.Dispose() }
}
try {
 Start-Sleep -Seconds 8
 $before=Status
 if($before.state -ne 2) { throw 'Initial playback not playing.' }
 $idle=State 'idle'
 if(@($idle.snapshots | Where-Object overlayBitmapAllocated).Count) { throw 'Idle overlay still owns a bitmap.' }
 $null=Status '--pause'
 $active=State 'active' $true
 if(-not @($active.snapshots | Where-Object overlayBitmapAllocated).Count -or $active.snapshots[-1].overlayFramesPresented -le $idle.snapshots[-1].overlayFramesPresented) { throw 'Overlay did not redraw/recreate after input.' }
 if((Status).state -ne 3) { throw 'Pause state not observed.' }
 $null=Status '--play'
 Start-Sleep -Seconds 5
 $resumed=State 'resumed'
 $after=Status
 if($after.state -ne 2 -or $after.positionSec -le $before.positionSec -or @($resumed.snapshots | Where-Object overlayBitmapAllocated).Count) { throw 'Playback/idle retirement failed after resume.' }
 $closed=Status '--stop'
 if($closed.state -ne 0) { throw 'Return to library failed.' }
 $null=Status '--open' (Join-Path $perf 'h264-1080p30-audio.mp4')
 Start-Sleep -Seconds 6
 if((Status).state -ne 2) { throw 'Explicit reopen failed.' }
 [ordered]@{passed=$true;pid=$process.Id;events=$events;checks=@('idle bitmap released','visible overlay recreated after input','overlay presentation successful','pause state observed','playback resumes and advances','bitmap retires after resume','return to library','reopen playback');appSha256=(Get-FileHash -LiteralPath (Join-Path $runtime 'Aperture.exe')).Hash;nativeSha256=(Get-FileHash -LiteralPath (Join-Path $runtime 'libmpv-2.dll')).Hash} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $out 'result.json') -Encoding utf8
 Write-Output 'Live overlay lifecycle: 8 checks passed.'
} finally {
 $events | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'status-events.json') -Encoding utf8
 if(-not $process.HasExited) { [void]$process.CloseMainWindow();if(-not $process.WaitForExit(5000)) { $process.Kill();[void]$process.WaitForExit(3000) } }
 $process.Dispose()
}
