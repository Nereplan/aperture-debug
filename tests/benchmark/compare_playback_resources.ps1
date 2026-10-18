[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$BaselineDll,
    [Parameter(Mandatory)][string]$CandidateDll,
    [Parameter(Mandatory)][string]$MediaDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [string]$MediaFile = 'h264_aac_720p.mp4',
    [ValidateSet('Renderer','LinearRenderers','QualityScaling','VertexReuse','Pacing','Transfers','HardwareThreads','FreeHeapCaches','EagerFlush','GpuTimers','Backbuffer','Presentation','TimerPolicy','RoundedTimerPolicy','LazyTables','Scaling','WaitRounding','VideoViews','DrawOverhead','DrawSubmission')][string]$Experiment = 'Renderer',
    [switch]$WithAudio,
    [ValidateRange(1,15)][int]$Iterations = 3,
    [ValidateRange(0,5)][int]$WarmupIterations = 1,
    [ValidateRange(1,120)][int]$SampleSeconds = 3,
    [switch]$CaptureFrames,
    [switch]$MemoryLayout,
    [switch]$VoCpuSections
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required.' }
foreach ($name in @('Probe','BaselineDll','CandidateDll','MediaDir')) {
    Set-Variable -Name $name -Value (Resolve-Path -LiteralPath (Get-Variable -Name $name -ValueOnly)).Path
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'OutputDir must be new; preserve prior measurements.' }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
if ($VoCpuSections -and -not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll)).Contains('APERTURE_TEST_VO_CPU_SECTIONS')) {
    throw 'VoCpuSections requires the marked diagnostic DLL.'
}
$variants = @(
    @{name='baseline-gpu'; dll=$BaselineDll; renderer='gpu'},
    @{name='candidate-gpu'; dll=$CandidateDll; renderer='gpu'},
    @{name='candidate-gpu-next'; dll=$CandidateDll; renderer='gpu-next'}
)
if ($Experiment -eq 'LinearRenderers') {
    if (-not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll)).Contains('APERTURE_TEST_ROUND_WAITS')) { throw 'LinearRenderers requires the marked experimental DLL.' }
    $variants = @('gpu','gpu-next' | ForEach-Object {
        @{name="linear-$_"; dll=$CandidateDll; renderer=$_; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'}
    })
}
if ($Experiment -eq 'QualityScaling') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    foreach ($marker in @('APERTURE_TEST_CACHE_BACKBUFFER','APERTURE_TEST_NO_GPU_TIMERS','APERTURE_TEST_REUSE_VERTICES','APERTURE_TEST_ROUND_WAITS')) { if (-not $dllText.Contains($marker)) { throw 'QualityScaling requires the marked experimental DLL.' } }
    $variants = @('default','balanced','linear' | ForEach-Object {
        @{name="quality-$_"; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling=$_; waitPolicy='min1'; cacheBackbuffer=$true; noTimers=$true; reuseVertices=$true}
    })
}
if ($Experiment -eq 'VertexReuse') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    foreach ($marker in @('APERTURE_TEST_VERTEX_CACHE_ENTRIES','APERTURE_TEST_CACHE_BACKBUFFER','APERTURE_TEST_NO_GPU_TIMERS','APERTURE_TEST_ROUND_WAITS')) { if (-not $dllText.Contains($marker)) { throw 'VertexReuse requires the marked experimental DLL.' } }
    $variants = @(
        @{name='vertex-none'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='balanced'; waitPolicy='min1'; cacheBackbuffer=$true; noTimers=$true},
        @{name='vertex-one'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='balanced'; waitPolicy='min1'; cacheBackbuffer=$true; noTimers=$true; reuseVertices=$true; vertexEntries='1'},
        @{name='vertex-four'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='balanced'; waitPolicy='min1'; cacheBackbuffer=$true; noTimers=$true; reuseVertices=$true; vertexEntries='4'}
    )
}
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
if ($Experiment -eq 'Presentation') {
    $variants = @(
        @{name='present-one'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; interval='1'},
        @{name='present-zero'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; interval='0'}
    )
}
if ($Experiment -eq 'TimerPolicy') {
    $variants = @('perwait','always','never' | ForEach-Object {
        @{name="timer-$_"; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; timerPolicy=$_}
    })
}
if ($Experiment -eq 'RoundedTimerPolicy') {
    if (-not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll)).Contains('APERTURE_TEST_ROUND_WAITS')) { throw 'RoundedTimerPolicy requires the marked experimental DLL.' }
    $variants = @('perwait','always','never' | ForEach-Object {
        @{name="rounded-timer-$_"; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; timerPolicy=$_}
    })
}
if ($Experiment -eq 'Scaling') {
    $variants = @(
        @{name='default-scaling'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='default'},
        @{name='linear-scaling'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'}
    )
}
if ($Experiment -eq 'WaitRounding') {
    if (-not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll)).Contains('APERTURE_TEST_ROUND_WAITS')) { throw 'WaitRounding requires the marked experimental DLL.' }
    $variants = @('floor','ceil','min1' | ForEach-Object {
        @{name="wait-$_"; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; waitPolicy=$_}
    })
}
if ($Experiment -eq 'VideoViews') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    foreach ($marker in @('APERTURE_TEST_CACHE_VIDEO_VIEWS','APERTURE_TEST_ROUND_WAITS')) { if (-not $dllText.Contains($marker)) { throw 'VideoViews requires the marked experimental DLL.' } }
    $variants = @(
        @{name='copy-linear'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'},
        @{name='direct-linear'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='yes'},
        @{name='cached-direct-linear'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='yes'; cacheVideoViews=$true}
    )
}
if ($Experiment -eq 'DrawOverhead') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    foreach ($marker in @('APERTURE_TEST_REUSE_VERTICES','APERTURE_TEST_NO_GPU_TIMERS','APERTURE_TEST_ROUND_WAITS')) { if (-not $dllText.Contains($marker)) { throw 'DrawOverhead requires the marked experimental DLL.' } }
    $variants = @(
        @{name='draw-baseline'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'},
        @{name='reuse-vertices'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; reuseVertices=$true},
        @{name='no-gpu-timing'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; noTimers=$true},
        @{name='reuse-no-timing'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; reuseVertices=$true; noTimers=$true}
    )
}
if ($Experiment -eq 'DrawSubmission') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    foreach ($marker in @('APERTURE_TEST_SKIP_D3D11_FLUSH','APERTURE_TEST_CACHE_BACKBUFFER','APERTURE_BACKBUFFER_CACHE','APERTURE_TEST_NO_GPU_TIMERS','APERTURE_TEST_ROUND_WAITS')) { if (-not $dllText.Contains($marker)) { throw 'DrawSubmission requires the marked experimental DLL.' } }
    $variants = @(
        @{name='submission-baseline'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'},
        @{name='present-flush'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; skipFlush=$true},
        @{name='reuse-backbuffer'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; cacheBackbuffer=$true},
        @{name='reuse-present-no-timing'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; scaling='linear'; waitPolicy='min1'; zero='no'; skipFlush=$true; cacheBackbuffer=$true; noTimers=$true}
    )
}
if ($Experiment -eq 'LazyTables') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    if (-not $dllText.Contains('APERTURE_TEST_LAZY_TX_TABLES')) { throw 'LazyTables requires the marked experimental DLL.' }
    $variants = @(
        @{name='writable-tables'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'},
        @{name='lazy-tables'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; lazyTables=$true}
    )
}
if ($Experiment -eq 'Backbuffer') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    if (-not $dllText.Contains('APERTURE_TEST_CACHE_BACKBUFFER')) { throw 'Backbuffer requires the marked experimental DLL.' }
    $variants = @(
        @{name='recreate-backbuffer'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'},
        @{name='reuse-backbuffer'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; cacheBackbuffer=$true},
        @{name='reuse-present-flush'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; cacheBackbuffer=$true; skipFlush=$true}
    )
}
if ($Experiment -eq 'GpuTimers') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    if (-not $dllText.Contains('APERTURE_TEST_NO_GPU_TIMERS')) { throw 'GpuTimers requires the marked experimental DLL.' }
    $variants = @(
        @{name='gpu-timers'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'},
        @{name='no-gpu-timers'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; noTimers=$true}
    )
}
if ($Experiment -eq 'EagerFlush') {
    $dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($CandidateDll))
    if (-not $dllText.Contains('APERTURE_TEST_SKIP_D3D11_FLUSH')) { throw 'EagerFlush requires the marked experimental DLL, not a normal runtime.' }
    $variants = @(
        @{name='eager-flush'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'},
        @{name='present-flush'; dll=$CandidateDll; renderer='gpu'; sync='audio'; threads='0'; hwthreads='1'; skipFlush=$true}
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
        if ($variant.scaling) { $info.ArgumentList.Add('--scaling'); $info.ArgumentList.Add($variant.scaling) }
        if ($MemoryLayout) { $info.ArgumentList.Add('--memory-layout') }
        if ($variant.heapCleanup) { $info.ArgumentList.Add('--heap-cleanup') }
        foreach ($pair in @(@('--zero-copy',$variant.zero),@('--swapchain-depth',$variant.depth),@('--latency-hacks',$variant.latency),@('--hwdec-threads',$variant.hwthreads),@('--sync-interval',$variant.interval))) {
            if ($pair[1]) { $info.ArgumentList.Add($pair[0]); $info.ArgumentList.Add($pair[1]) }
        }
        $info.ArgumentList.Add('--sample-seconds'); $info.ArgumentList.Add([string]$SampleSeconds)
        if ($CaptureFrames) { $info.ArgumentList.Add('--capture-window'); $info.ArgumentList.Add((Join-Path $run 'rendered-frame.png')) }
        foreach ($name in @('TEMP','TMP','APPDATA','LOCALAPPDATA')) {
            $directory = Join-Path $run $name
            New-Item -ItemType Directory -Path $directory | Out-Null
            $info.Environment[$name] = $directory
        }
        [void]$info.Environment.Remove('APERTURE_TEST_SKIP_D3D11_FLUSH')
        [void]$info.Environment.Remove('APERTURE_TEST_NO_GPU_TIMERS')
        [void]$info.Environment.Remove('APERTURE_TEST_CACHE_BACKBUFFER')
        [void]$info.Environment.Remove('APERTURE_TEST_LAZY_TX_TABLES')
        [void]$info.Environment.Remove('APERTURE_TEST_VO_CPU_SECTIONS')
        [void]$info.Environment.Remove('APERTURE_TEST_ROUND_WAITS')
        [void]$info.Environment.Remove('APERTURE_TEST_CACHE_VIDEO_VIEWS')
        [void]$info.Environment.Remove('APERTURE_TEST_REUSE_VERTICES')
        [void]$info.Environment.Remove('APERTURE_TEST_VERTEX_CACHE_ENTRIES')
        [void]$info.Environment.Remove('APERTURE_TEST_SMALL_VERTEX_BUFFER')
        foreach ($name in @('MPV_HRT','MPV_HRT_MAX','MPV_HRT_RES')) { [void]$info.Environment.Remove($name) }
        if ($variant.skipFlush) { $info.Environment['APERTURE_TEST_SKIP_D3D11_FLUSH'] = '1' }
        if ($variant.noTimers) { $info.Environment['APERTURE_TEST_NO_GPU_TIMERS'] = '1' }
        if ($variant.cacheBackbuffer) { $info.Environment['APERTURE_TEST_CACHE_BACKBUFFER'] = '1' }
        if ($variant.lazyTables) { $info.Environment['APERTURE_TEST_LAZY_TX_TABLES'] = '1' }
        if ($VoCpuSections) { $info.Environment['APERTURE_TEST_VO_CPU_SECTIONS'] = '1' }
        if ($variant.timerPolicy) { $info.Environment['MPV_HRT'] = $variant.timerPolicy }
        if ($variant.waitPolicy) { $info.Environment['APERTURE_TEST_ROUND_WAITS'] = $variant.waitPolicy }
        if ($variant.cacheVideoViews) { $info.Environment['APERTURE_TEST_CACHE_VIDEO_VIEWS'] = '1' }
        if ($variant.reuseVertices) { $info.Environment['APERTURE_TEST_REUSE_VERTICES'] = '1' }
        if ($variant.vertexEntries) { $info.Environment['APERTURE_TEST_VERTEX_CACHE_ENTRIES'] = $variant.vertexEntries }
        $startedUtc = [DateTime]::UtcNow.ToString('o')
        $process = [Diagnostics.Process]::Start($info)
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(($SampleSeconds+60)*1000)) { $process.Kill(); throw "Resource probe timed out: $run" }
        $completedUtc = [DateTime]::UtcNow.ToString('o')
        $log = $stdout.GetAwaiter().GetResult(); $errorLog = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $run 'stdout.txt'), $log)
        [IO.File]::WriteAllText((Join-Path $run 'stderr.txt'), $errorLog)
        if ($process.ExitCode -ne 0) { throw "Resource probe failed: $run" }
        $match = [regex]::Match($log, '(?m)^RESOURCE (.+)$')
        if (!$match.Success) { throw "No resource sample: $run" }
        $sample = $match.Groups[1].Value | ConvertFrom-Json
        $renderOptions = [regex]::Match($log, '(?m)^RENDER_OPTIONS (.+)$')
        if ($renderOptions.Success) { $sample | Add-Member render_options ($renderOptions.Groups[1].Value | ConvertFrom-Json) }
        if ($variant.scaling) {
            if (-not $renderOptions.Success) { throw "Scaling experiments require effective renderer options: $run" }
            if ($variant.scaling -eq 'linear') {
                foreach ($name in @('scale','dscale','cscale')) { if ($sample.render_options.$name -ne 'bilinear') { throw "Requested bilinear option did not apply: $name" } }
                foreach ($name in @('linear-downscaling','correct-downscaling','sigmoid-upscaling')) { if ($sample.render_options.$name -ne 'no') { throw "Requested scaling option did not apply: $name" } }
            }
            if ($variant.scaling -eq 'balanced') {
                $expected = @{scale='lanczos';dscale='hermite';cscale='bilinear';'linear-downscaling'='no';'correct-downscaling'='yes';'sigmoid-upscaling'='no'}
                foreach ($name in $expected.Keys) { if ($sample.render_options.$name -ne $expected[$name]) { throw "Requested balanced option did not apply: $name" } }
            }
        }
        if ($MemoryLayout) {
            $layoutMatch = [regex]::Match($log, '(?m)^HEAP_LAYOUT (.+)$')
            if (-not $layoutMatch.Success) { throw "Missing post-measurement memory diagnostic: $run" }
            $sample | Add-Member memory_layout ($layoutMatch.Groups[1].Value | ConvertFrom-Json)
        }
        if ($VoCpuSections) {
            $sections = @([regex]::Matches($errorLog, '(?m)^APERTURE_VO_CPU (.+)$') | ForEach-Object { $_.Groups[1].Value | ConvertFrom-Json })
            if ($sections.Count -lt 2 -or @($sections | Where-Object failures -NE 0).Count) { throw "Incomplete video CPU diagnostic: $run" }
            $sample | Add-Member vo_cpu_sections $sections
        }
        if ($variant.cacheVideoViews) {
            $viewStats = @([regex]::Matches($errorLog, '(?m)^APERTURE_VIDEO_VIEWS (.+)$') | ForEach-Object { $_.Groups[1].Value | ConvertFrom-Json })
            if (-not $viewStats.Count -or ($viewStats | Measure-Object hits -Sum).Sum -lt 1 -or ($viewStats | Measure-Object created_views -Sum).Sum -lt 1) { throw "Video view cache did not execute: $run" }
            $sample | Add-Member video_view_cache_stats $viewStats
        }
        if ($variant.reuseVertices) {
            $vertexStats = @([regex]::Matches($errorLog, '(?m)^APERTURE_VERTEX_CACHE (.+)$') | ForEach-Object { $_.Groups[1].Value | ConvertFrom-Json })
            if (-not $vertexStats.Count -or ($vertexStats | Measure-Object uploads -Sum).Sum -lt 1) { throw "Vertex cache was not observed: $run" }
            # A multi-pass quality scaler can change geometry every draw and
            # legitimately get zero hits. Linear screens require actual reuse.
            if (($variant.scaling -eq 'linear' -or ($Experiment -eq 'VertexReuse' -and $variant.vertexEntries -eq 4)) -and
                ($vertexStats | Measure-Object reuses -Sum).Sum -lt 1) { throw "Requested vertex reuse did not execute: $run" }
            if ($variant.vertexEntries -and @($vertexStats | Where-Object capacity -NE ([int]$variant.vertexEntries)).Count) { throw "Requested vertex cache capacity did not apply: $run" }
            $sample | Add-Member vertex_cache_stats $vertexStats
        }
        if ($Experiment -in @('DrawSubmission','VertexReuse') -and $variant.cacheBackbuffer) {
            $cacheStats = @([regex]::Matches($errorLog, '(?m)^APERTURE_BACKBUFFER_CACHE (.+)$') | ForEach-Object { $_.Groups[1].Value | ConvertFrom-Json })
            if (-not $cacheStats.Count -or ($cacheStats | Measure-Object reuses -Sum).Sum -lt 1) { throw "Backbuffer cache did not execute: $run" }
            $sample | Add-Member backbuffer_cache_stats $cacheStats
        }
        $sample | Add-Member skip_eager_flush ([bool]$variant.skipFlush)
        $sample | Add-Member gpu_timers_disabled ([bool]$variant.noTimers)
        $sample | Add-Member backbuffer_reuse ([bool]$variant.cacheBackbuffer)
        $sample | Add-Member lazy_tables ([bool]$variant.lazyTables)
        $sample | Add-Member cached_video_views ([bool]$variant.cacheVideoViews)
        $sample | Add-Member reused_vertices ([bool]$variant.reuseVertices)
        if ($variant.vertexEntries) { $sample | Add-Member vertex_cache_entries ([int]$variant.vertexEntries) }
        if ($variant.timerPolicy) { $sample | Add-Member timer_policy $variant.timerPolicy }
        if ($variant.waitPolicy) { $sample | Add-Member wait_policy $variant.waitPolicy }
        if ($variant.interval) { $sample | Add-Member sync_interval $variant.interval }
        $sample | Add-Member variant $variant.name
        $sample | Add-Member round $round
        $sample | Add-Member warmup ($round -lt $WarmupIterations)
        $sample | Add-Member dll_sha256 (Get-FileHash -LiteralPath $dll).Hash
        $sample | Add-Member probe_sha256 (Get-FileHash -LiteralPath $exe).Hash
        $sample | Add-Member started_utc $startedUtc
        $sample | Add-Member completed_utc $completedUtc
        $sample | Add-Member launch_arguments ([string[]]@($info.ArgumentList))
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
