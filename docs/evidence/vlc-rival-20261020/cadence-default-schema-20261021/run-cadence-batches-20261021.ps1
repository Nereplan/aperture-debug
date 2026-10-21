$ErrorActionPreference='Stop'
$repo='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019'
$collector=Join-Path $work 'cadence-collector-20261021'
$out=Join-Path $work 'cadence-runs-20261021'
if(Test-Path -LiteralPath $out){throw 'Cadence runs require a fresh output directory.'}
$collectorIdentity=Get-Content -LiteralPath (Join-Path $collector 'started.json') -Raw|ConvertFrom-Json
if(-not (Get-Process -Id $collectorIdentity.presentMonPid -ErrorAction SilentlyContinue)){throw 'Verified collector is not live.'}
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
$identity=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');collector=$collectorIdentity;environment=(Get-PlayerBenchmarkEnvironment);displayControllerDiagnostics=@(Get-CimInstance Win32_VideoController|Select-Object Name,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate);sourceCommit='23fd026c2b114f8391465bfb5f7d28a79da02025';sourceProductionDirty=$false;harnessCommit='9766849';runtimeManifests=@{Aperture=$manifestA;VLC=$manifestV};protocol='Sequential fresh full runtimes/profiles; one warmup and three measured rounds/player/workload, rotating order; 3s hub then owned open, matched1280x720/DPI guarded visible canvas, defaults. 40s timed playback window after10s load/resize; predeclared interior5..35s is analyzed. Each fresh PID then gets a confirmed3s pause and5s resumed period to identify the video swapchain, without reusing addresses between processes. Retain undisplayed rows and all chains; reject ambiguous video-chain identification and any final ETW loss warnings. PresentMon tracks display metadata but not GPU/input timing. No resource CPU ranking, unique content-frame, audio or physical-panel claim.';runs=[Collections.Generic.List[object]]::new()}
$identity|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
function Save-Run($record,[string]$dir){$record|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $dir 'run.json') -Encoding utf8}
foreach($workload in 'h264','hevc'){
 $MediaPath=Join-Path 'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017' $(if($workload -eq 'h264'){'h264-1080p30-audio.mp4'}else{'hevc-2160p60-audio.mp4'})
 for($round=0;$round -lt 4;$round++){
  for($offset=0;$offset -lt 2;$offset++){
   $name=@('Aperture','VLC')[($round+$offset)%2]
   if(Get-Process -Name Aperture,vlc -ErrorAction SilentlyContinue){throw 'Unexpected player already running; refusing the next launch.'}
   if(-not (Get-Process -Id $collectorIdentity.presentMonPid -ErrorAction SilentlyContinue)){throw 'Collector ended before remaining runs.'}
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
    if(-not $player.HasExited){[PlayerBenchmarkWindow]::Close($window,[uint32]$player.Id);if(-not $player.WaitForExit(5000)){$player.Kill();$player.WaitForExit();$record.cleanup='owned-pid-killed'}else{$record.cleanup='owned-window-closed'}}else{$record.cleanup='owned-process-exited'}
    $record.endedUtc=[DateTime]::UtcNow.ToString('o');$player.Dispose();Save-Run $record $dir;$identity.runs.Add($record);$identity|ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
   }
   Write-Output ($workload+' '+$name+' round'+$round+': '+$record.status)
  }
 }
}
$identity.finishedUtc=[DateTime]::UtcNow.ToString('o');$identity|ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $out 'identity.json') -Encoding utf8
Write-Output 'All cadence run windows and per-process pause/resume probes completed; qualification awaits collector shutdown/loss checks.'