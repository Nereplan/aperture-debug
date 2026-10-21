$ErrorActionPreference='Stop'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$out=Join-Path $work 'cadence-collector-v2-20261021'
$runs=Join-Path $work 'cadence-runs-v2-20261021'
$pm='C:\Users\Alperen\AppData\Local\Codex\tools\PresentMon\2.6.0\PresentMon-2.6.0-x64.exe'
$expected='b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
function Assert-PresentMon {
 if((Get-FileHash -LiteralPath $pm -Algorithm SHA256).Hash -ine $expected){throw 'PresentMon does not match the verified official release digest.'}
}
function Save-Json($value,[string]$path){
 $temporary=$path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
 [IO.File]::WriteAllText($temporary,($value|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
 [IO.File]::Move($temporary,$path,$true)
}
function Stop-ExactProcess($process,[int]$ownedPid,[long]$startTicks,[string]$path){
 if($process.HasExited){return}
 if($process.Id -ne $ownedPid -or $process.StartTime.ToUniversalTime().Ticks -ne $startTicks -or $process.MainModule.FileName -ine $path){throw 'Owned process identity changed; refusing fallback termination.'}
 $process.Kill()
 if(-not $process.WaitForExit(5000)){throw 'Owned process did not exit after fallback termination.'}
}
Assert-PresentMon
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Collector did not receive the approved temporary elevation.'}
if(Test-Path -LiteralPath $out){throw 'Collector requires a fresh output directory.'}
New-Item -ItemType Directory -Path $out|Out-Null
$session='ApertureCadenceV2-'+[guid]::NewGuid().ToString('N')
$nonce=[guid]::NewGuid().ToString('N')
$markerPath=Join-Path $out 'coordinator-finished.json'
$arguments=@('--process_name','Aperture.exe','--process_name','vlc.exe','--session_name',$session,'--v2_metrics','--output_stdout','--timed','1800','--terminate_after_timed','--qpc_time','--write_display_metadata','--no_console_stats','--no_track_gpu','--no_track_input')
$identity=[ordered]@{protocol='ApertureCadenceCollectorV2';startedUtc=[DateTime]::UtcNow.ToString('o');collectorPid=$PID;collectorProcessStartUtc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o');presentMonPath=$pm;presentMonSha256=$expected;arguments=$arguments;sessionName=$session;controlNonce=$nonce;coordinatorMarkerPath=$markerPath;expectedRunDirectory=$runs;maximumSeconds=1800;cleanupGraceSeconds=20;status='starting';captureQualified=$false;scope='Temporary elevated collector for Aperture.exe/vlc.exe; analysis retains only exact coordinator-owned PID/lifetime/windows. No resource CPU ranking. No global session stop or security changes.';previousCapture=@{collectorDirectory=(Join-Path $work 'cadence-collector-20261021');runDirectory=(Join-Path $work 'cadence-runs-20261021');missingRequiredColumns=@('DisplayedTime');sampledRuns=14;hevcComplete=$false;disposition='Original default-schema trace and partial coordinator retained as diagnostic only. No joining with v2.'}}
$process=$null;$csvStream=$null;$errorStream=$null;$stdoutCopy=$null;$stderrCopy=$null
$stopIssued=$false;$forced=$false;$timedOut=$false;$validMarker=$null;$streamError=$null
try{
 $csvStream=[IO.FileStream]::new((Join-Path $out 'presents.csv'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite,1,[IO.FileOptions]::Asynchronous)
 $errorStream=[IO.FileStream]::new((Join-Path $out 'stderr.txt'),[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite,1,[IO.FileOptions]::Asynchronous)
 $start=[Diagnostics.ProcessStartInfo]::new($pm)
 $start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($argument in $arguments){$start.ArgumentList.Add($argument)}
 $process=[Diagnostics.Process]::Start($start)
 $processStartTicks=$process.StartTime.ToUniversalTime().Ticks
 $stdoutCopy=$process.StandardOutput.BaseStream.CopyToAsync($csvStream,4096)
 $stderrCopy=$process.StandardError.BaseStream.CopyToAsync($errorStream,4096)
 $identity.presentMonPid=$process.Id;$identity.presentMonProcessStartUtc=$process.StartTime.ToUniversalTime().ToString('o');$identity.status='running'
 Save-Json $identity (Join-Path $out 'started.json')
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while(-not $process.WaitForExit(200)){
  if($stdoutCopy.IsFaulted -or $stderrCopy.IsFaulted){throw 'A redirected byte stream failed.'}
  if(Test-Path -LiteralPath $markerPath){
   try{$candidate=Get-Content -LiteralPath $markerPath -Raw|ConvertFrom-Json}catch{$candidate=$null}
   if($candidate -and $candidate.protocol -ceq 'ApertureCadenceCollectorV2' -and $candidate.controlNonce -ceq $nonce -and $candidate.sessionName -ceq $session -and $candidate.presentMonPid -eq $process.Id -and $candidate.collectorPid -eq $PID -and $candidate.runDirectory -ceq $runs -and $candidate.allOwnedPlayersClosed -eq $true -and $candidate.completionStatus -cin @('completed','aborted')){
    $validMarker=$candidate;break
   }
  }
  if($timer.Elapsed.TotalSeconds -ge 1800){$timedOut=$true;break}
 }
 if(-not $validMarker){$timedOut=$true}
 $identity.status=if($validMarker){'coordinator-'+$validMarker.completionStatus}else{'unqualified-timeout-or-early-exit'}
}catch{
 $identity.status='collector-failed';$identity.failure=$_.Exception.Message
}finally{
 if($process){
  if(-not $process.HasExited){
   $helper=$null
   try{
    Assert-PresentMon
    # Session and executable come only from this collector, never from the marker.
    $stopStart=[Diagnostics.ProcessStartInfo]::new($pm)
    $stopStart.UseShellExecute=$false;$stopStart.CreateNoWindow=$true;$stopStart.RedirectStandardOutput=$true;$stopStart.RedirectStandardError=$true
    foreach($argument in @('--session_name',$session,'--terminate_existing_session')){$stopStart.ArgumentList.Add($argument)}
    $helper=[Diagnostics.Process]::Start($stopStart);$helperStartTicks=$helper.StartTime.ToUniversalTime().Ticks;$identity.stopPid=$helper.Id
    $helperOutput=$helper.StandardOutput.ReadToEndAsync();$helperError=$helper.StandardError.ReadToEndAsync();$stopIssued=$true
    if(-not $helper.WaitForExit(10000)){$forced=$true;Stop-ExactProcess $helper $identity.stopPid $helperStartTicks $pm}
    $identity.stopExitCode=$helper.ExitCode
    [IO.File]::WriteAllText((Join-Path $out 'stop-stdout.txt'),$helperOutput.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $out 'stop-stderr.txt'),$helperError.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
   }catch{$identity.stopFailure=$_.Exception.Message;$forced=$true}
   finally{if($helper){$helper.Dispose()}}
   if(-not $process.WaitForExit(20000)){
    $forced=$true
    try{Stop-ExactProcess $process $identity.presentMonPid $processStartTicks $pm}catch{$identity.terminationFailure=$_.Exception.Message}
   }
  }
  $identity.processExited=$process.HasExited
  if($process.HasExited){$identity.exitCode=$process.ExitCode}
  # Drain both pipes before closing files, including all stderr from failed runs.
  foreach($copy in @($stdoutCopy,$stderrCopy)){
   if(-not $copy){continue}
   try{if(-not $copy.Wait(10000)){throw 'Redirected byte stream did not drain.'};$copy.GetAwaiter().GetResult()}catch{$streamError=$_.Exception.Message}
  }
  $process.Dispose()
 }
 foreach($stream in @($csvStream,$errorStream)){if($stream){try{$stream.Flush($true)}catch{$streamError=$_.Exception.Message}finally{$stream.Dispose()}}}
 $identity.endedUtc=[DateTime]::UtcNow.ToString('o');$identity.stopIssued=$stopIssued;$identity.forcedCleanup=$forced;$identity.timeoutFallback=$timedOut;$identity.streamError=$streamError
 if($validMarker){$identity.coordinatorCompletion=$validMarker}
 $csv=Join-Path $out 'presents.csv';$stderr=Join-Path $out 'stderr.txt'
 $identity.csvExists=Test-Path -LiteralPath $csv
 $lossLogs=@($csv,$stderr,(Join-Path $out 'stop-stdout.txt'),(Join-Path $out 'stop-stderr.txt'))|Where-Object {Test-Path -LiteralPath $_}
 $identity.lossLogSources=@($lossLogs)
 $identity.lossWarning=if($lossLogs.Count){[bool](Select-String -LiteralPath $lossLogs -Pattern 'ETW (buffers|events) were lost|overflowed present events detected' -Quiet)}else{$true}
 $identity.schemaQualified=$false
 if($identity.csvExists){
  $header=Get-Content -LiteralPath $csv -TotalCount 1
  $identity.csvHeader=$header
  $columns=if($header){$header.Split(',')}else{@()}
  $requiredColumns=@('Application','ProcessID','SwapChainAddress','DisplayedTime','CPUStartQPC','VidPnSourceId','LayerIndex','PresentId')
  $identity.schemaQualified=(@($requiredColumns|Where-Object {$_ -cnotin $columns}).Count -eq 0)
  $identity.csvBytes=(Get-Item -LiteralPath $csv).Length
 }
 $identity.captureQualified=($identity.status -eq 'coordinator-completed' -and $identity.processExited -and $identity.exitCode -eq 0 -and $stopIssued -and $identity.stopExitCode -eq 0 -and -not $forced -and -not $timedOut -and -not $streamError -and -not $identity.lossWarning -and $identity.schemaQualified -and $identity.csvBytes -gt 0)
 Save-Json $identity (Join-Path $out 'finished.json')
}
if(-not $identity.captureQualified){exit 1}
