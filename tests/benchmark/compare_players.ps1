<#
.SYNOPSIS
    Local portable-player resource comparison. Does not install/download/build players.
.DESCRIPTION
    Supply complete clean portable runtime directories (or executables in those directories).
    Each iteration copies the runtime to a fresh harness directory and redirects only child
    APPDATA/LOCALAPPDATA/TEMP/TMP. Existing same-name players cause a safe failure.
    Window latency is process launch to a visible unowned top-level window, NOT first frame.
    Window size is the outer rectangle; video viewport and decoder defaults can differ.
    No memory trimming, cache flushing, global environment changes, or user-process cleanup.
    JSON metadata is retained; copied runtimes/config/cache are removed after each run.
    VLC flags are defined in official 3.0.x src/libvlc-module.c, modules/gui/qt/qt.cpp,
    modules/control/oldrc.c: https://github.com/videolan/vlc/tree/3.0.x
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$AperturePath,
    [Parameter(Mandatory)][string]$VlcPath,
    [string]$MpcHcPath = '',
    [Parameter(Mandatory)][string]$MediaPath,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$ApertureStatusProbe = '',
    [ValidateSet('Aperture','VLC','MPC-HC')][string[]]$PlayerNames = @('Aperture','VLC','MPC-HC'),
    [ValidateRange(1,30)][int]$Iterations = 3,
    [ValidateRange(0,10)][int]$WarmupIterations = 1,
    [ValidateRange(320,3840)][int]$WindowWidth = 1280,
    [ValidateRange(240,2160)][int]$WindowHeight = 800,
    [ValidateRange(0,3840)][int]$VideoWidth = 0,
    [ValidateRange(0,2160)][int]$VideoHeight = 0,
    [switch]$ForceD3D11Linear,
    [string]$ApertureVariantsFile = '',
    [ValidateRange(0,120)][int]$SamplingSeconds = 0,
    [ValidateRange(0,86400)][int]$LaunchMediaStartSeconds = 0,
    [ValidateRange(0,86400)][int]$SamplingStartMediaSeconds = 0
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7 (ProcessStartInfo.ArgumentList required).' }
function Assert-NoReparseAncestors([string]$Path) {
    $current=[IO.Path]::GetFullPath($Path)
    while($current) {
        if(Test-Path -LiteralPath $current) {
            $item=Get-Item -LiteralPath $current -Force
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point path is not allowed: $current" }
        }
        $current=[IO.Path]::GetDirectoryName($current.TrimEnd('\','/'))
    }
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\','/')
if($OutputDir.Length -le [IO.Path]::GetPathRoot($OutputDir).Length) { throw 'OutputDir must be a specific harness directory, not a drive root.' }
Assert-NoReparseAncestors $OutputDir
$MediaPath = (Resolve-Path -LiteralPath $MediaPath).Path
$mediaHash = (Get-FileHash -LiteralPath $MediaPath -Algorithm SHA256).Hash
if ($ApertureStatusProbe) { $ApertureStatusProbe = (Resolve-Path -LiteralPath $ApertureStatusProbe).Path }
function Runtime([string]$Name, [string]$Path, [string[]]$Names) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    Assert-NoReparseAncestors $resolved
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        $exe = $Names | ForEach-Object { Join-Path $resolved $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (-not $exe) { throw "No $Name executable found in $resolved" }
    } else { $exe = $resolved; $resolved = Split-Path -Parent $exe }
    # Media must never be copied into or counted as the application's runtime.
    if ($MediaPath.StartsWith($resolved.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Media must be outside $Name runtime." }
    if ($OutputDir.StartsWith($resolved.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Output must be outside $Name runtime." }
    $files = @(Get-ChildItem -LiteralPath $resolved -File -Recurse)
    if (Get-ChildItem -LiteralPath $resolved -Recurse -Attributes ReparsePoint -ErrorAction SilentlyContinue) { throw "Reparse points are not allowed in runtime: $resolved" }
    [pscustomobject]@{name=$Name;source=$resolved;exeName=[IO.Path]::GetFileName($exe);exeHash=(Get-FileHash -LiteralPath $exe).Hash;version=(Get-Item -LiteralPath $exe).VersionInfo.FileVersion;runtimeBytes=[int64](($files | Measure-Object Length -Sum).Sum);runtimeFileCount=$files.Count}
}
$players = @()
if ($ApertureVariantsFile) {
    $variants = @(Get-Content -LiteralPath $ApertureVariantsFile -Raw | ConvertFrom-Json)
    if (-not $variants.Count -or @($variants.id | Select-Object -Unique).Count -ne $variants.Count) { throw 'Variant IDs must be nonempty and unique.' }
    foreach ($variant in $variants) {
        if ($variant.id -notmatch '^[a-zA-Z0-9-]{1,64}$') { throw 'Invalid variant ID.' }
        $player = Runtime 'Aperture' $variant.runtime @('Aperture.exe')
        $player | Add-Member -NotePropertyName variant -NotePropertyValue $variant.id
        $experimentalEnvironment = @{}
        if ($variant.environment) {
            $allowed = @{
                APERTURE_TEST_ROUND_WAITS = @('floor','ceil','min1')
                APERTURE_TEST_CACHE_BACKBUFFER = @('1')
                APERTURE_TEST_NO_GPU_TIMERS = @('1')
                APERTURE_TEST_REUSE_VERTICES = @('1')
            }
            $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $player.source 'libmpv-2.dll')))
            foreach ($entry in $variant.environment.PSObject.Properties) {
                if (-not $allowed.ContainsKey($entry.Name) -or $entry.Value -cnotin $allowed[$entry.Name]) { throw 'Unsupported experimental variant environment.' }
                if (-not $dllText.Contains($entry.Name)) { throw "Variant requires a marked experimental DLL: $($entry.Name)" }
                $experimentalEnvironment[$entry.Name] = [string]$entry.Value
            }
        }
        $player | Add-Member -NotePropertyName experimentalEnvironment -NotePropertyValue $experimentalEnvironment
        $players += $player
    }
} else { $players += Runtime 'Aperture' $AperturePath @('Aperture.exe') }
$players += Runtime 'VLC' $VlcPath @('vlc.exe')
if ($MpcHcPath) { $players += Runtime 'MPC-HC' $MpcHcPath @('mpc-hc64.exe','mpc-hc.exe') }
$players = @($players | Where-Object { $_.name -in $PlayerNames })
if (-not $players.Count) { throw 'No selected runtime supplied.' }
if (($LaunchMediaStartSeconds -or $SamplingStartMediaSeconds -or $SamplingSeconds) -and 'MPC-HC' -in $players.name) { throw 'Aligned extended mode currently supports Aperture and VLC only.' }
if ($SamplingStartMediaSeconds -lt $LaunchMediaStartSeconds) { throw 'Sampling media position precedes launch position.' }
if (($VideoWidth -eq 0) -ne ($VideoHeight -eq 0)) { throw 'Specify both video canvas dimensions.' }
if ($VideoWidth -and ('MPC-HC' -in $players.name -or $VideoWidth -lt 320 -or $VideoHeight -lt 180)) { throw 'Video canvas matching supports Aperture/VLC and at least 320x180.' }
# Refuse singleton interference instead of controlling or terminating user players.
foreach ($player in $players) {
    if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($player.exeName)) -ErrorAction SilentlyContinue) { throw "Close existing $($player.name) instances before comparing; nothing was closed." }
}
if (-not ('PlayerBenchmarkWindow' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'PlayerBenchmarkWindow.cs') }
if ($MpcHcPath) { Add-Type -Path (Join-Path $PSScriptRoot 'MpcStatusObserver.cs') }
function Bytes([string]$Path) { [int64]((Get-ChildItem -LiteralPath $Path -File -Recurse | Measure-Object Length -Sum).Sum) }
function Cycles([Diagnostics.Process]$OwnedProcess) {
    [UInt64]$count = 0
    if (-not [PlayerBenchmarkWindow]::QueryProcessCycles($OwnedProcess.Handle,[ref]$count)) { throw 'Owned-process cycle counter unavailable.' }
    $count
}
function Gpu([int]$OwnedPid) {
    try {
        $samples = (Get-Counter -Counter @('\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage') -ErrorAction Stop).CounterSamples
        $owned = @($samples | Where-Object { $_.InstanceName -like "pid_${OwnedPid}_*" })
        if (-not $owned.Count) { throw 'No GPU Process Memory instance for owned PID.' }
        if (@($owned | Where-Object { $_.Status -notin @(0,1) }).Count) { throw 'GPU counter returned invalid sample status.' }
        $dedicated = @($owned | Where-Object { $_.Path -like '*\Dedicated Usage' })
        $shared = @($owned | Where-Object { $_.Path -like '*\Shared Usage' })
        if (-not $dedicated.Count -or -not $shared.Count) { throw 'Incomplete GPU counters.' }
        return @{dedicatedBytes=[int64](($dedicated | Measure-Object CookedValue -Sum).Sum);sharedBytes=[int64](($shared | Measure-Object CookedValue -Sum).Sum);unavailableReason=$null}
    } catch { return @{dedicatedBytes=$null;sharedBytes=$null;unavailableReason=$_.Exception.Message} }
}
function Vlc-Time([int]$Port, [int]$OwnedPid) {
    # Verify the listener's owning PID before sending any command. Fail closed on a race.
    $listener = @(Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort $Port -State Listen -ErrorAction Stop)
    if ($listener.Count -ne 1 -or $listener[0].OwningProcess -ne $OwnedPid) { throw 'RC listener is not owned by benchmark process.' }
    $client = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync('127.0.0.1',$Port)
        if (-not $connect.Wait(1500)) { throw 'RC connect timeout.' }
        $stream = $client.GetStream(); $stream.ReadTimeout = 1500; $stream.WriteTimeout = 1500
        $command = [Text.Encoding]::ASCII.GetBytes("get_time`n")
        $stream.Write($command,0,$command.Length)
        $buffer = [byte[]]::new(4096); $reply = ''; $deadline = [DateTime]::UtcNow.AddSeconds(2)
        do {
            $count = $stream.Read($buffer,0,$buffer.Length)
            if ($count -eq 0) { break }
            $reply += [Text.Encoding]::UTF8.GetString($buffer,0,$count)
            $matches = [regex]::Matches($reply,'(?m)^\s*(?:>\s*)?(\d+)\s*\r?$')
            if ($matches.Count) { return [double]$matches[$matches.Count-1].Groups[1].Value }
        } while ([DateTime]::UtcNow -lt $deadline)
        throw 'RC returned no numeric playback time.'
    } finally { $client.Dispose() }
}
function Playback([string]$Name, [int]$OwnedPid, [int]$Port, $Environment) {
    try {
        if ($Name -eq 'VLC') { return @{positionSec=(Vlc-Time $Port $OwnedPid);state=$null;reason=$null} }
        if ($Name -eq 'MPC-HC' -and $mpcObserver) { return @{positionSec=$mpcObserver.Query([uint32]$OwnedPid);state=$null;reason=$null} }
        if ($Name -eq 'Aperture' -and $ApertureStatusProbe) {
            $psi = [Diagnostics.ProcessStartInfo]::new($ApertureStatusProbe); $psi.UseShellExecute=$false; $psi.CreateNoWindow=$true; $psi.RedirectStandardOutput=$true
            $psi.ArgumentList.Add([string]$OwnedPid)
            foreach($key in $Environment.Keys) { $psi.Environment[$key]=$Environment[$key] }
            $probe = [Diagnostics.Process]::Start($psi)
            try {
                if (-not $probe.WaitForExit(5000)) { $probe.Kill(); throw 'Status probe timed out.' }
                if ($probe.ExitCode -ne 0) { throw "Status probe failed ($($probe.ExitCode))." }
                $status = $probe.StandardOutput.ReadToEnd() | ConvertFrom-Json
                return @{positionSec=$status.positionSec;state=$status.state;reason=$null}
            } finally { $probe.Dispose() }
        }
        return @{positionSec=$null;state=$null;reason='No owned-PID playback status probe supplied/available.'}
    } catch { return @{positionSec=$null;state=$null;reason=$_.Exception.Message} }
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$runRoot = [IO.Path]::GetFullPath((Join-Path $OutputDir ('runs-' + [Guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $runRoot | Out-Null
if (-not $ApertureStatusProbe -and 'Aperture' -in $players.name) {
    # Compile only this observer, using child-scoped VS initialization.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
            $probeSource = Join-Path $PSScriptRoot 'player_status_probe.cpp'
            $probeExe = Join-Path $runRoot 'player_status_probe.exe'
            foreach($path in @($vcvars,$probeSource,$probeExe,$runRoot)) { if ($path -match '[%"&|<>^\r\n]') { throw 'Unsupported cmd metacharacter in helper build path.' } }
            $buildCommand = 'call "{0}" >nul && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 "{1}" /Fo"{2}\player_status_probe.obj" /Fe:"{3}" /link user32.lib shell32.lib advapi32.lib /SUBSYSTEM:CONSOLE' -f $vcvars,$probeSource,$runRoot,$probeExe
            & cmd.exe /d /s /c $buildCommand *> (Join-Path $runRoot 'probe-build.log')
            if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $probeExe)) { $ApertureStatusProbe=$probeExe }
            else { Write-Warning 'Status observer build failed; Aperture playback will be explicitly unverified. See probe-build.log.' }
        }
    }
}
$results = [Collections.Generic.List[object]]::new()
for ($round=0; $round -lt ($WarmupIterations+$Iterations); $round++) {
    # Rotate player order to avoid giving one player every cache/order advantage.
    for($offset=0; $offset -lt $players.Count; $offset++) {
        $player = $players[($round+$offset)%$players.Count]
        if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($player.exeName)) -ErrorAction SilentlyContinue) { throw "Existing $($player.name) process detected; refusing launch." }
        $variantName = if ($player.PSObject.Properties['variant']) { $player.variant } else { $player.name }
        $dir = Join-Path $runRoot ("$round-$variantName")
        $runtime = Join-Path $dir 'runtime'; $profile = Join-Path $dir 'profile'
        New-Item -ItemType Directory -Path $runtime -Force | Out-Null
        Get-ChildItem -LiteralPath $player.source -Force | Copy-Item -Destination $runtime -Recurse -Force
        $envOverride = @{APPDATA=(Join-Path $profile 'Roaming');LOCALAPPDATA=(Join-Path $profile 'Local');TEMP=(Join-Path $profile 'Temp');TMP=(Join-Path $profile 'Temp')}
        foreach($path in ($envOverride.Values | Select-Object -Unique)) { New-Item -ItemType Directory -Path $path -Force | Out-Null }
        if ($player.PSObject.Properties['experimentalEnvironment']) {
            foreach ($key in $player.experimentalEnvironment.Keys) { $envOverride[$key]=$player.experimentalEnvironment[$key] }
        }
        $argsList = [Collections.Generic.List[string]]::new(); $port=0; $config=$null; $mpcObserver=$null
        if ($player.name -eq 'VLC') {
            # Windows VLC uses native Shell folders, not APPDATA overrides. Official
            # src/win32/dirs.c honors an executable-adjacent portable directory.
            New-Item -ItemType Directory -Path (Join-Path $runtime 'portable') -Force | Out-Null
            $socket = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0); $socket.Start(); $port=$socket.LocalEndpoint.Port; $socket.Stop()
            $config=Join-Path $profile 'vlcrc'; [IO.File]::WriteAllText($config,'')
            foreach($arg in @('--intf=qt','--ignore-config',"--config=$config",'--no-one-instance','--no-one-instance-when-started-from-file','--no-media-library','--no-metadata-network-access','--no-qt-privacy-ask','--no-qt-updates-notif','--no-qt-recentplay','--qt-continue=0','--no-qt-video-autoresize','--extraintf=rc','--rc-quiet',"--rc-host=127.0.0.1:$port")) { $argsList.Add($arg) }
        } elseif ($player.name -eq 'MPC-HC') {
            $config=Join-Path $runtime ([IO.Path]::GetFileNameWithoutExtension($player.exeName)+'.ini')
            # Official 2.8.3 Profile.cpp selects portable mode by adjacent INI
            # existence; keys are defined in src/mpc-hc/SettingsDefines.h.
            [IO.File]::WriteAllText($config, (@('[Settings]','KeepHistory=0','RecentFilesNumber=0','HistoryInAppData=0','RememberFilePos=0','RememberDVDPos=0','RememberExternalPlaylistPos=0','RememberPlaylistItems=0','UseWin7TaskBar=0','UpdaterAutoCheck=0','EnableCrashReporter=0','AllowMultipleInstances=1','UseSeekPreview=0') -join "`r`n"), [Text.Encoding]::ASCII)
            $mpcObserver=[MpcStatusObserver]::new()
            foreach($arg in @('/new','/slave',[string]$mpcObserver.Handle,'/play')) { $argsList.Add($arg) }
        } else {
            # Aperture uses its executable-adjacent data/cache directories.
            foreach($subdir in @('data','cache')) {
                $path=Join-Path $runtime $subdir
                if(Test-Path -LiteralPath $path) { throw "Supply a clean Aperture runtime without preexisting $subdir." }
                New-Item -ItemType Directory -Path $path | Out-Null
            }
            $config=Join-Path $runtime 'data'
        }
        if ($LaunchMediaStartSeconds) {
            $argsList.Add($(if ($player.name -eq 'VLC') { "--start-time=$LaunchMediaStartSeconds" } else { "--start=$LaunchMediaStartSeconds" }))
        }
        $argsList.Add($MediaPath)
        if ($ForceD3D11Linear -and $player.name -eq 'VLC') {
            foreach ($argument in @('--vout=direct3d11','--avcodec-hw=d3d11va','--d3d11-upscale-mode=linear')) { $argsList.Insert($argsList.Count-1,$argument) }
        }
        $beforeRuntime=Bytes $runtime; $beforeProfile=Bytes $profile
        $configText = if(Test-Path -LiteralPath $config -PathType Leaf) { [IO.File]::ReadAllText($config) } else { 'Fresh executable-adjacent data/ directory.' }
        $record=[ordered]@{player=$player.name;round=$round;warmup=($round -lt $WarmupIterations);status='failed';failure=$null;runtimeSource=$player.source;runtimeBytes=$player.runtimeBytes;runtimeFileCount=$player.runtimeFileCount;executableHash=$player.exeHash;version=$player.version;runDirectory=$dir;arguments=$argsList.ToArray();childEnvironment=$envOverride;configPath=$config;initialConfig=$configText;pid=$null;processToVisibleWindowMs=$null;windowDimensions=$null;playbackVerification='playback-unverified';playbackReason=$null;samples=@();writableGrowthBytes=$null;cleanup=$null;artifactCleanup=$null}
        $record.variant = $variantName
        $record.startedUtc = [DateTime]::UtcNow.ToString('o')
        $process=$null; $window=[IntPtr]::Zero
        try {
            $psi=[Diagnostics.ProcessStartInfo]::new((Join-Path $runtime $player.exeName)); $psi.WorkingDirectory=$runtime; $psi.UseShellExecute=$false; $psi.CreateNoWindow=$true
            foreach($arg in $argsList) { $psi.ArgumentList.Add($arg) }
            foreach ($key in @('APERTURE_TEST_SKIP_D3D11_FLUSH','APERTURE_TEST_NO_GPU_TIMERS','APERTURE_TEST_CACHE_BACKBUFFER','APERTURE_TEST_LAZY_TX_TABLES','APERTURE_TEST_VO_CPU_SECTIONS','APERTURE_TEST_ROUND_WAITS','APERTURE_TEST_REUSE_VERTICES','APERTURE_TEST_VERTEX_CACHE_ENTRIES','APERTURE_TEST_SMALL_VERTEX_BUFFER','APERTURE_TEST_CACHE_VIDEO_VIEWS','MPV_HRT','MPV_HRT_MAX','MPV_HRT_RES')) { [void]$psi.Environment.Remove($key) }
            foreach($key in $envOverride.Keys) { $psi.Environment[$key]=$envOverride[$key] }
            $timer=[Diagnostics.Stopwatch]::StartNew(); $process=[Diagnostics.Process]::Start($psi); $record.pid=$process.Id
            if($mpcObserver) { $mpcObserver.SetOwnedPid([uint32]$process.Id) }
            do {
                if($process.HasExited) { throw "Player exited before visible window ($($process.ExitCode))." }
                $window=[PlayerBenchmarkWindow]::Find([uint32]$process.Id)
                if($window -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 20 }
            } while($window -eq [IntPtr]::Zero -and $timer.ElapsedMilliseconds -lt 15000)
            if($window -eq [IntPtr]::Zero) { throw 'Visible owned window timeout.' }
            $record.processToVisibleWindowMs=$timer.Elapsed.TotalMilliseconds
            if(-not [PlayerBenchmarkWindow]::Resize($window,[uint32]$process.Id,$WindowWidth,$WindowHeight)) { throw 'Owned-window resize failed.' }
            Start-Sleep -Seconds 8
            # Some players resize again when the decoder announces video size.
            # Enforce the shared outer dimensions after that initial load too.
            if(-not [PlayerBenchmarkWindow]::Resize($window,[uint32]$process.Id,$WindowWidth,$WindowHeight)) { throw 'Post-load resize failed.' }
            if ($VideoWidth -and -not [PlayerBenchmarkWindow]::MatchVideoCanvas($window,[uint32]$process.Id,$player.name,$VideoWidth,$VideoHeight)) { throw 'Video canvas matching failed.' }
            Start-Sleep -Seconds 2
            if ($SamplingStartMediaSeconds) {
                $alignmentDeadline = [DateTime]::UtcNow.AddSeconds(60)
                do {
                    $position = Playback $player.name $process.Id $port $envOverride
                    if ($position.reason) { throw "Playback alignment failed: $($position.reason)" }
                    if ($position.positionSec -ge $SamplingStartMediaSeconds) { break }
                    Start-Sleep -Milliseconds 200
                } while ([DateTime]::UtcNow -lt $alignmentDeadline)
                if ($position.positionSec -lt $SamplingStartMediaSeconds) { throw 'Playback alignment timeout.' }
                $record.alignedPlaybackPosition = $position.positionSec
            }
            $samples=[Collections.Generic.List[object]]::new()
            $sampleCount = if ($SamplingSeconds) { [int][Math]::Ceiling($SamplingSeconds / 5.0) + 1 } else { 3 }
            $sampleCount = [Math]::Max(3,$sampleCount)
            $samplingClock = [Diagnostics.Stopwatch]::StartNew()
            $record.samplingStartedUtc = [DateTime]::UtcNow.ToString('o')
            for($sample=0; $sample -lt $sampleCount; $sample++) {
                if ($SamplingSeconds) {
                    $remaining = $SamplingSeconds * $sample / ($sampleCount-1) - $samplingClock.Elapsed.TotalSeconds
                    if ($remaining -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($remaining*1000)) }
                } elseif($sample) { Start-Sleep -Seconds 2 }
                $process.Refresh(); if($process.HasExited) { throw 'Player exited during sampling.' }
                if ($SamplingSeconds) {
                    # Timestamp CPU before either control or GPU queries. Their
                    # overhead remains inside the measured wall interval.
                    $elapsed = $timer.Elapsed.TotalSeconds; $cpu = $process.TotalProcessorTime.TotalSeconds
                    $cycles = Cycles $process
                    $workingSet = $process.WorkingSet64; $private = $process.PrivateMemorySize64
                    $playback = Playback $player.name $process.Id $port $envOverride
                    $gpu = Gpu $process.Id
                    $samples.Add([pscustomobject]@{elapsedSeconds=$elapsed;workingSetBytes=$workingSet;privateBytes=$private;cpuSeconds=$cpu;cpuCycles=$cycles;dedicatedGpuBytes=$gpu.dedicatedBytes;sharedGpuBytes=$gpu.sharedBytes;gpuUnavailableReason=$gpu.unavailableReason;playback=$playback})
                    continue
                }
                $gpu=Gpu $process.Id
                $process.Refresh()
                $samples.Add([pscustomobject]@{elapsedSeconds=$timer.Elapsed.TotalSeconds;workingSetBytes=$process.WorkingSet64;privateBytes=$process.PrivateMemorySize64;cpuSeconds=$process.TotalProcessorTime.TotalSeconds;cpuCycles=(Cycles $process);dedicatedGpuBytes=$gpu.dedicatedBytes;sharedGpuBytes=$gpu.sharedBytes;gpuUnavailableReason=$gpu.unavailableReason;playback=(Playback $player.name $process.Id $port $envOverride)})
            }
            $record.samplingEndedUtc = [DateTime]::UtcNow.ToString('o')
            $record.samples=$samples.ToArray(); $record.windowDimensions=[PlayerBenchmarkWindow]::Size($window)
            $record.videoCanvas=[PlayerBenchmarkWindow]::VideoCanvas($window,[uint32]$process.Id,$player.name)
            $record.childWindows=[PlayerBenchmarkWindow]::Children($window,[uint32]$process.Id)
            if ($VideoWidth) {
                if (-not $record.videoCanvas -or $record.videoCanvas.clientWidth -ne $VideoWidth -or $record.videoCanvas.clientHeight -ne $VideoHeight) { throw 'Player changed the requested video canvas size.' }
            } elseif($record.windowDimensions[0] -ne $WindowWidth -or $record.windowDimensions[1] -ne $WindowHeight) { throw 'Player changed the requested comparison window size.' }
            $positions=@($samples | ForEach-Object { $_.playback.positionSec }); $valid=@($samples | Where-Object { $null -ne $_.playback.positionSec -and -not $_.playback.reason })
            $statesOk=($player.name -ne 'Aperture' -or @($samples | Where-Object { $_.playback.state -ne 2 }).Count -eq 0)
            $advancing = $true
            for ($i=1; $i -lt $positions.Count; $i++) { if ($positions[$i] -le $positions[$i-1]) { $advancing=$false } }
            if($valid.Count -eq $sampleCount -and $advancing -and $statesOk) { $record.playbackVerification='advancing-playback-verified' }
            else { $record.playbackReason='Missing/nonadvancing time or nonplaying state. ' + (($samples | ForEach-Object { $_.playback.reason } | Where-Object { $_ } | Select-Object -Unique) -join '; ') }
            $record.status='sampled'
        } catch { $record.failure=$_.Exception.Message }
        finally {
            if($process) {
                if(-not $process.HasExited) {
                    [PlayerBenchmarkWindow]::Close($window,[uint32]$process.Id)
                    if(-not $process.WaitForExit(5000)) { $process.Kill(); [void]$process.WaitForExit(3000); $record.cleanup='owned-process-force-terminated' }
                    else { $record.cleanup='owned-window-closed' }
                } else { $record.cleanup='already-exited' }
                $process.Dispose()
            }
            if($mpcObserver) { $mpcObserver.Dispose() }
            $record.writableGrowthBytes=@{runtimeDelta=(Bytes $runtime)-$beforeRuntime;profileDelta=(Bytes $profile)-$beforeProfile}
            # Validate exact targets before deleting only this run's generated copies.
            foreach($generated in @($runtime,$profile)) {
                $checked = [IO.Path]::GetFullPath($generated)
                if(-not $checked.StartsWith($runRoot + '\',[StringComparison]::OrdinalIgnoreCase) -or -not $checked.StartsWith($dir + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside owned run directory.' }
                Assert-NoReparseAncestors $checked
                if(Get-ChildItem -LiteralPath $checked -Recurse -Attributes ReparsePoint -ErrorAction Stop) { throw 'Refusing recursive cleanup containing a reparse point.' }
                Remove-Item -LiteralPath $checked -Recurse -Force
            }
            $record.artifactCleanup='runtime-and-profile-copies-removed; metadata-retained'
            $results.Add([pscustomobject]$record)
            $record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $dir 'result.json') -Encoding utf8
            Write-Output ("{0} round {1} ({2}): {3}; {4}" -f $variantName,$round, $(if($record.warmup){'warmup'}else{'measured'}),$record.status,$record.playbackVerification)
        }
    }
}
$report=[ordered]@{schemaVersion=1;createdUtc=[DateTime]::UtcNow.ToString('o');mediaPath=$MediaPath;mediaSha256=$mediaHash;mediaBytes=(Get-Item -LiteralPath $MediaPath).Length;iterations=$Iterations;warmupIterations=$WarmupIterations;windowTarget=@($WindowWidth,$WindowHeight);startupMetric='Process launch to first visible unowned top-level window (not first frame)';sampling='8s after visible window plus 2s after post-load resize; 3 samples separated by at least 2s (counter/status overhead included)';apertureStatusProbe=$ApertureStatusProbe;limitations=@('Warm OS caches; no cold-cache claim.','Window outer rectangles match; video viewport and default decoder may differ.','Samples cover owned root process only; inspect unexpected child processes separately.','Playback status does not prove pixels are visible or decoder hardware equivalence.','Child environment redirects ordinary profile/temp writes; Windows Known Folder APIs may bypass environment variables (VLC additionally uses portable/).','VLC RC verification adds a control-interface overhead.','MPC-HC uses a hidden /slave window to verify position; no decoder equivalence claim.');players=$players;results=$results.ToArray()}
if ($SamplingSeconds) { $report.sampling = "CPU window requested ${SamplingSeconds}s with counter samples every ~5s; actual CPU/wall deltas retained; launch media position ${LaunchMediaStartSeconds}s, sampling begins after at least ${SamplingStartMediaSeconds}s media time and ten seconds settling. VLC media times are integer-quantized. Ordered variants rotate each round." }
if ($VideoWidth) { $report.videoCanvasTarget=@($VideoWidth,$VideoHeight); $report.limitations[1]='Video canvases match; actual child window dimensions/DPI are retained. Equal filter choices do not establish identical decoder or output pixels.' }
$report.forcedVlcD3D11Linear=[bool]$ForceD3D11Linear
$report | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $OutputDir 'comparison.json') -Encoding utf8
Write-Output (Join-Path $OutputDir 'comparison.json')
if(@($results | Where-Object { $_.status -ne 'sampled' }).Count) { throw 'One or more player runs failed; see retained per-run results.' }
