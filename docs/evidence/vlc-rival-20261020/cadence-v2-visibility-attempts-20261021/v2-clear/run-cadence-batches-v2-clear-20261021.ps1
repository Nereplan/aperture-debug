$ErrorActionPreference='Stop'
$repo='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$collector=Join-Path $work 'cadence-collector-v2-clear-20261021'
$out=Join-Path $work 'cadence-runs-v2-clear-20261021'

$expectedPm='C:\Users\Alperen\AppData\Local\Codex\tools\PresentMon\2.6.0\PresentMon-2.6.0-x64.exe'
$expectedPmHash='b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
function Save-CadenceJson($value,[string]$path){
 $temporary=$path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
 [IO.File]::WriteAllText($temporary,($value|ConvertTo-Json -Depth 20),[Text.UTF8Encoding]::new($false))
 [IO.File]::Move($temporary,$path,$true)
}
function Assert-CadenceCollectorLive {
 $abortPath=Join-Path $collector 'abort-request.json'
 if(Test-Path -LiteralPath $abortPath){
  $abort=Get-Content -LiteralPath $abortPath -Raw|ConvertFrom-Json
  if($abort.controlNonce -ceq $collectorIdentity.controlNonce -and $abort.sessionName -ceq $collectorIdentity.sessionName){throw ('Coordinator abort requested: '+$abort.reason)}
 }
 if(Test-Path -LiteralPath (Join-Path $collector 'finished.json')){throw 'Collector has already finished.'}
 $presentMon=Get-Process -Id $collectorIdentity.presentMonPid -ErrorAction Stop
 try{
  if($presentMon.StartTime.ToUniversalTime().Ticks -ne ([DateTime]$collectorIdentity.presentMonProcessStartUtc).ToUniversalTime().Ticks){throw 'Collector PID was reused.'}
 }finally{$presentMon.Dispose()}
}
function Assert-CadenceCsvSchema {
 Assert-CadenceCollectorLive
 $csv=Join-Path $collector 'presents.csv'
 $stream=[IO.FileStream]::new($csv,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite,1)
 $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true,1024)
 try{$header=$reader.ReadLine()}finally{$reader.Dispose()}
 $columns=if($header){$header.Split(',')}else{@()}
 foreach($column in 'Application','ProcessID','SwapChainAddress','DisplayedTime','CPUStartQPC','VidPnSourceId','LayerIndex','PresentId'){
  if($column -cnotin $columns){throw ('Live CSV lacks required v2 column '+$column)}
 }
 return @{header=$header;checkedQpc=[Diagnostics.Stopwatch]::GetTimestamp();requiredColumns=@('Application','ProcessID','SwapChainAddress','DisplayedTime','CPUStartQPC','VidPnSourceId','LayerIndex','PresentId')}
}
function Close-CadenceOwnedPlayer($owned){
 $process=$owned.process
 if($process.HasExited){return 'owned-process-exited'}
 if($process.Id -ne $owned.pid -or $process.StartTime.ToUniversalTime().Ticks -ne $owned.startTicks -or $process.MainModule.FileName -ine $owned.exe){throw 'Owned player identity changed; refusing cleanup.'}
 if($owned.window -ne [IntPtr]::Zero){[PlayerBenchmarkWindow]::Close($owned.window,[uint32]$owned.pid)}
 if($process.WaitForExit(5000)){return 'owned-window-closed'}
 $process.Kill()
 if(-not $process.WaitForExit(5000)){throw 'Exact owned player did not exit.'}
 return 'owned-pid-killed'
}

if(Test-Path -LiteralPath $out){throw 'Cadence runs require a fresh output directory.'}
$collectorIdentity=Get-Content -LiteralPath (Join-Path $collector 'started.json') -Raw|ConvertFrom-Json
if($collectorIdentity.protocol -cne 'ApertureCadenceCollectorV2' -or $collectorIdentity.controlNonce -cnotmatch '^[a-f0-9]{32}$' -or $collectorIdentity.sessionName -cnotmatch '^ApertureCadenceV2-[a-f0-9]{32}$' -or $collectorIdentity.expectedRunDirectory -cne $out -or $collectorIdentity.coordinatorMarkerPath -cne (Join-Path $collector 'coordinator-finished.json') -or $collectorIdentity.presentMonPath -ine $expectedPm -or $collectorIdentity.presentMonSha256 -ine $expectedPmHash){throw 'Collector identity/nonce/path is invalid.'}
if((Get-FileHash -LiteralPath $expectedPm -Algorithm SHA256).Hash -ine $expectedPmHash){throw 'Verified PresentMon executable changed.'}
foreach($required in '--v2_metrics','--output_stdout','--qpc_time','--write_display_metadata','--no_track_gpu','--no_track_input'){
 if($required -cnotin $collectorIdentity.arguments){throw ('Collector lacks required argument '+$required)}
}
Assert-CadenceCollectorLive
$activeOwned=$null;$identity=$null;$coordinatorStatus='aborted';$coordinatorFailure=$null
try {
if(Get-Process -Name Aperture,vlc,cl,link -ErrorAction SilentlyContinue){throw 'Existing player/compiler detected.'}
. (Join-Path $repo 'tests\benchmark\player_comparison_identity.ps1')
Add-Type -Path (Join-Path $repo 'tests\benchmark\PlayerBenchmarkWindow.cs')
Add-Type -Path (Join-Path $repo 'tests\benchmark\PlayerVideoReadiness.cs')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tests\benchmark\compare_players.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Existing comparison harness did not parse.'}
foreach($name in 'VisibleCanvas','Vlc-ReplyValues','Vlc-Numbers','Vlc-Time','Playback','Vlc-ColorState','Color-Status','Assert-ColorPaused','Wait-ColorPaused','Resume-ColorVlc'){
 $definition=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$false)
 if(-not $definition){throw ('Missing existing helper '+$name)}
 . ([scriptblock]::Create($definition.Extent.Text))
}
$prior=Get-Content -LiteralPath (Join-Path $work 'h264-hub-final-clean-20261020\comparison.json') -Raw|ConvertFrom-Json
$ApertureStatusProbe=Join-Path (Split-Path -Parent $prior.results[0].runDirectory) 'player_status_probe.exe'
if((Get-FileHash -LiteralPath $ApertureStatusProbe).Hash -ne $prior.apertureStatusProbeSha256){throw 'Owned-PID status probe changed.'}
New-Item -ItemType Directory -Path $out|Out-Null
$sourceA=Join-Path $work 'candidate-runtime-qualified';$sourceV=Join-Path $work 'vlc-3.0.24\vlc-3.0.24'
$manifestA=Get-PlayerRuntimeManifest $sourceA;$manifestV=Get-PlayerRuntimeManifest $sourceV
$harnessCommit=(& git -C $repo rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0 -or $harnessCommit -cnotmatch '^[a-f0-9]{40}$'){throw 'Cannot verify current harness commit.'}
$planPath=Join-Path $work 'cadence-analysis-plan-v2-clear-20261021.json'
$analysisPlan=Get-Content -LiteralPath $planPath -Raw|ConvertFrom-Json
$identity=[ordered]@{analysisPlanPath=$planPath;analysisPlanSha256=(Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash;analysisPlan=$analysisPlan;previousCaptureDisposition='Original default-schema capture lacks DisplayedTime; 14 runs retained as diagnostic only, HEVC incomplete, original pause rule failed VLC. No joining with v2.';startedUtc=[DateTime]::UtcNow.ToString('o');collector=$collectorIdentity;environment=(Get-PlayerBenchmarkEnvironment);displayControllerDiagnostics=@(Get-CimInstance Win32_VideoController|Select-Object Name,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate);sourceCommit='23fd026c2b114f8391465bfb5f7d28a79da02025';sourceProductionDirty=$false;harnessCommit=$harnessCommit;harnessSourceSha256=(Get-FileHash -LiteralPath (Join-Path $repo 'tests\benchmark\compare_players.ps1')).Hash;runtimeManifests=@{Aperture=$manifestA;VLC=$manifestV};protocol='Sequential fresh full runtimes/profiles; one warmup and three measured rounds/player/workload, rotating order; 3s hub then owned open, matched1280x720/DPI guarded visible canvas, defaults. 40s timed playback window after10s load/resize; predeclared interior5..35s is analyzed. Each fresh PID then gets a confirmed3s pause and5s resumed period to identify the video swapchain, without reusing addresses between processes. Retain undisplayed rows and all chains; reject ambiguous video-chain identification and any final ETW loss warnings. PresentMon v2 metrics track display metadata but not GPU/input timing; required live CSV columns are asserted before every timed window. The separate predeclared v2 plan identifies exactly one chain with playing/resumed rates80..120% nominal and paused rate at most50% nominal. No resource CPU ranking, unique content-frame, audio or physical-panel claim.';runs=[Collections.Generic.List[object]]::new()}
$identity|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
function Save-Run($record,[string]$dir){$record|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $dir 'run.json') -Encoding utf8}
foreach($workload in 'h264','hevc'){
 $MediaPath=Join-Path 'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017' $(if($workload -eq 'h264'){'h264-1080p30-audio.mp4'}else{'hevc-2160p60-audio.mp4'})
 for($round=0;$round -lt 4;$round++){
  for($offset=0;$offset -lt 2;$offset++){
   $name=@('Aperture','VLC')[($round+$offset)%2]
   if(Get-Process -Name Aperture,vlc -ErrorAction SilentlyContinue){throw 'Unexpected player already running; refusing the next launch.'}
   Assert-CadenceCollectorLive
   $dir=Join-Path $out ($workload+'-'+$round+'-'+$name);New-Item -ItemType Directory -Path $dir|Out-Null
   $runtime=Join-Path $dir 'runtime';New-Item -ItemType Directory -Path $runtime|Out-Null
   $source=if($name -eq 'Aperture'){$sourceA}else{$sourceV};$expectedManifest=if($name -eq 'Aperture'){$manifestA}else{$manifestV}
   Get-ChildItem -LiteralPath $source -Force|Copy-Item -Destination $runtime -Recurse
   if((Get-PlayerRuntimeManifest $runtime).sha256 -cne $expectedManifest.sha256){throw 'Staged runtime does not match its original inventory.'}
   $envOverride=@{}
   foreach($key in 'TEMP','TMP','APPDATA','LOCALAPPDATA'){$p=Join-Path $dir $key;New-Item -ItemType Directory -Path $p|Out-Null;$envOverride[$key]=$p}
   $argsList=[Collections.Generic.List[string]]::new();$port=0
   if($name -eq 'VLC'){
    New-Item -ItemType Directory -Path (Join-Path $runtime 'portable')|Out-Null
    $socket=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$socket.Start();$port=$socket.LocalEndpoint.Port;$socket.Stop()
    $config=Join-Path $dir 'vlcrc';[IO.File]::WriteAllText($config,'')
    foreach($arg in @('--intf=qt','--ignore-config',"--config=$config",'--no-one-instance','--no-one-instance-when-started-from-file','--no-media-library','--no-metadata-network-access','--no-qt-privacy-ask','--no-qt-updates-notif','--no-qt-recentplay','--qt-continue=0','--no-qt-video-autoresize','--extraintf=rc','--rc-quiet',"--rc-host=127.0.0.1:$port")){$argsList.Add($arg)}
    $exe=Join-Path $runtime 'vlc.exe'
   }else{foreach($subdir in 'data','cache'){New-Item -ItemType Directory -Path (Join-Path $runtime $subdir)|Out-Null};$exe=Join-Path $runtime 'Aperture.exe'}
   $start=[Diagnostics.ProcessStartInfo]::new($exe);$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WorkingDirectory=$runtime
   foreach($arg in $argsList){$start.ArgumentList.Add($arg)};foreach($key in $envOverride.Keys){$start.Environment[$key]=$envOverride[$key]}
   $record=[ordered]@{player=$name;workload=$workload;round=$round;warmup=($round -eq 0);mediaPath=$MediaPath;mediaSha256=(Get-FileHash -LiteralPath $MediaPath).Hash;executableSha256=(Get-FileHash -LiteralPath $exe).Hash;runtimeManifestSha256=$expectedManifest.sha256;arguments=$argsList.ToArray();startedUtc=[DateTime]::UtcNow.ToString('o');status='pending';guards=[Collections.Generic.List[object]]::new();cleanup='pending';qpcFrequency=[Diagnostics.Stopwatch]::Frequency}
   $player=[Diagnostics.Process]::Start($start);$window=[IntPtr]::Zero;$record.pid=$player.Id;$record.processStartUtc=$player.StartTime.ToUniversalTime().ToString('o')
   $activeOwned=@{process=$player;pid=$player.Id;startTicks=$player.StartTime.ToUniversalTime().Ticks;exe=$exe;window=$window}
   try {
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    do{if($player.HasExited){throw 'Player exited before window setup.'};$window=[PlayerBenchmarkWindow]::Find([uint32]$player.Id);if($window -ne [IntPtr]::Zero){break};Start-Sleep -Milliseconds 20}while([DateTime]::UtcNow -lt $deadline)
    if($window -eq [IntPtr]::Zero -or -not [PlayerBenchmarkWindow]::Resize($window,[uint32]$player.Id,1280,800) -or -not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Owned window setup failed.'}
    Start-Sleep -Seconds 3
    $record.open=Playback $name $player.Id $port $envOverride -OpenMedia
    if($record.open.reason){throw $record.open.reason}
    Start-Sleep -Seconds 8
    if(-not [PlayerBenchmarkWindow]::MatchVideoCanvas($window,[uint32]$player.Id,$name,1280,720) -or -not [PlayerBenchmarkWindow]::Raise($window,[uint32]$player.Id)){throw 'Matched video canvas setup failed.'}
    Start-Sleep -Seconds 2
    $record.canvas=[PlayerBenchmarkWindow]::VideoCanvas($window,[uint32]$player.Id,$name)
    $record.guards.Add((VisibleCanvas $player.Id $name))
    $record.before=Playback $name $player.Id $port $envOverride
    if($record.before.reason){throw $record.before.reason}
    $record.collectorSchema=Assert-CadenceCsvSchema
    $record.windowStartQpc=[Diagnostics.Stopwatch]::GetTimestamp();$timer=[Diagnostics.Stopwatch]::StartNew()
    for($i=1;$i -le 8;$i++){
     $remaining=5000*$i-$timer.Elapsed.TotalMilliseconds;if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Ceiling($remaining))}
     $record.guards.Add((VisibleCanvas $player.Id $name))
    }
    $record.windowEndQpc=[Diagnostics.Stopwatch]::GetTimestamp();$record.after=Playback $name $player.Id $port $envOverride
    if($record.after.reason -or $record.after.positionSec-$record.before.positionSec -lt 38){throw 'Timed video interval did not advance by at least38s.'}
    $record.pause=Color-Status $name $player.Id $port $envOverride -Pause;$record.pauseSettlement=@{}
    $settled=Wait-ColorPaused $name $player.Id $port $envOverride $record.pause $record.pause.positionSec $record.pauseSettlement
    $record.pauseStartQpc=[Diagnostics.Stopwatch]::GetTimestamp();Start-Sleep -Seconds 3
    $record.pauseConfirmation=Color-Status $name $player.Id $port $envOverride;Assert-ColorPaused $record.pauseConfirmation $settled.positionSec $settled
    $record.pauseEndQpc=[Diagnostics.Stopwatch]::GetTimestamp();$record.guards.Add((VisibleCanvas $player.Id $name))
    if($name -eq 'VLC'){$record.resume=@{};Resume-ColorVlc $player.Id $port $envOverride $record.resume|Out-Null}else{$record.resume=Playback $name $player.Id $port $envOverride -Action Play;if($record.resume.reason){throw $record.resume.reason}}
    $record.resumeStartQpc=[Diagnostics.Stopwatch]::GetTimestamp();Start-Sleep -Seconds 5
    $record.resumeEndQpc=[Diagnostics.Stopwatch]::GetTimestamp();$record.resumedStatus=Playback $name $player.Id $port $envOverride
    if($record.resumedStatus.reason -or $record.resumedStatus.positionSec-$record.pauseConfirmation.positionSec -lt 3){throw 'Resumed video interval did not advance.'}
    $record.guards.Add((VisibleCanvas $player.Id $name));$record.status='sampled'
   }catch{$record.status='failed';$record.failure=$_.Exception.Message;throw}
   finally{
    $activeOwned.window=$window;$record.cleanup=Close-CadenceOwnedPlayer $activeOwned
    $record.endedUtc=[DateTime]::UtcNow.ToString('o');$player.Dispose();$activeOwned=$null;Save-Run $record $dir;$identity.runs.Add($record);$identity|ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
   }
   Write-Output ($workload+' '+$name+' round'+$round+': '+$record.status)
  }
 }
}
$identity.finishedUtc=[DateTime]::UtcNow.ToString('o');$identity|ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
Write-Output 'All cadence run windows and per-process pause/resume probes completed; qualification awaits collector shutdown/loss checks.'
$coordinatorStatus='completed'
}catch{
 $coordinatorFailure=$_.Exception.Message
 throw
}finally{
 $cleanupFailure=$null
 if($activeOwned){
  try{Close-CadenceOwnedPlayer $activeOwned|Out-Null;$activeOwned.process.Dispose();$activeOwned=$null}catch{$cleanupFailure=$_.Exception.Message}
 }
 try{
  if($identity){
   $identity.coordinatorStatus=$coordinatorStatus;$identity.coordinatorFailure=$coordinatorFailure;$identity.cleanupFailure=$cleanupFailure;$identity.finishedUtc=[DateTime]::UtcNow.ToString('o')
   Save-CadenceJson $identity (Join-Path $out 'identity.json')
  }
 }catch{
  $coordinatorStatus='aborted';$coordinatorFailure=$_.Exception.Message
  throw
 }finally{
  # Only a nonce-bound marker after every owned player has exited may stop this collector.
  if(-not $activeOwned){
   $marker=[ordered]@{protocol='ApertureCadenceCollectorV2';controlNonce=$collectorIdentity.controlNonce;sessionName=$collectorIdentity.sessionName;presentMonPid=$collectorIdentity.presentMonPid;collectorPid=$collectorIdentity.collectorPid;runDirectory=$out;coordinatorPid=$PID;completionStatus=$coordinatorStatus;allOwnedPlayersClosed=$true;createdUtc=[DateTime]::UtcNow.ToString('o');failure=$coordinatorFailure}
   Save-CadenceJson $marker $collectorIdentity.coordinatorMarkerPath
  }else{throw ('Owned cleanup failed; collector marker withheld: '+$cleanupFailure)}
 }
}
