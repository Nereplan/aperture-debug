param([string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use the pinned PowerShell 7 runner.' }
$plan = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'plan.json') -Raw | ConvertFrom-Json
if (!$OutputRoot) { $OutputRoot = $plan.defaultOutputRoot }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot).TrimEnd('\','/')
if (!$OutputRoot.StartsWith($plan.workRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must stay below the named external work root.' }
if (Test-Path -LiteralPath $OutputRoot) { throw 'Choose a fresh output directory; no evidence will be overwritten.' }
. (Join-Path $plan.repository 'tests\benchmark\player_comparison_identity.ps1')
function Save-DiagnosticJson($Value, [string]$Path) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 16), [Text.UTF8Encoding]::new($false))
}
function Assert-DiagnosticInputs {
    foreach ($pinnedInput in $plan.inputHashes) {
        if (!(Test-Path -LiteralPath $pinnedInput.path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $pinnedInput.path -Algorithm SHA256).Hash -ine $pinnedInput.sha256) { throw "Pinned input changed: $($pinnedInput.path)" }
    }
    if ((Get-PlayerRuntimeManifest $plan.runtime).sha256 -ine $plan.runtimeManifestSha256) { throw 'Frozen runtime inventory changed.' }
    if ((Get-FileHash -LiteralPath (Join-Path $plan.runtime 'Aperture.exe') -Algorithm SHA256).Hash -ine $plan.executableSha256) { throw 'Frozen executable changed.' }
}
Assert-DiagnosticInputs
if (Get-Process -Name Aperture,vlc,cl,link,ninja,'PresentMon-2.6.0-x64' -ErrorAction SilentlyContinue) { throw 'A player, compiler or cadence collector is active; nothing was closed.' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'plan.json') -Destination (Join-Path $OutputRoot 'plan.json')
$index = [ordered]@{status='running';startedUtc=[DateTime]::UtcNow.ToString('o');purpose=$plan.purpose;sourceContentCommit=$plan.sourceContentCommit;calls=@()}
try {
    foreach ($call in $plan.calls) {
        Assert-DiagnosticInputs
        if (Get-Process -Name Aperture,vlc,cl,link,ninja,'PresentMon-2.6.0-x64' -ErrorAction SilentlyContinue) { throw 'A player, compiler or cadence collector appeared; nothing was closed.' }
        $out = Join-Path $OutputRoot $call.outputName
        New-Item -ItemType Directory -Path $out | Out-Null
        $entry = [ordered]@{sequence=$call.sequence;pair=$call.pair;phase=$call.phase;mode=$call.mode;excludeFromMeasuredSummary=$call.excludeFromMeasuredSummary;output=$out;status='running';startedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$null;failure=$null}
        $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-File',$plan.harness,
            '-AllowVisiblePlayback','-FailFast','-PlayerNames','Aperture',
            '-AperturePath',$plan.runtime,'-ApertureStatusProbe',$plan.statusProbe,
            '-MediaPath',$plan.media,'-OutputDir',$out,'-LaunchMode',$call.mode,
            '-Iterations','1','-WarmupIterations','0','-WindowWidth','1280','-WindowHeight','800',
            '-VideoWidth','1280','-VideoHeight','720','-HubSettleSeconds','3',
            '-SamplingStartMediaSeconds','20.5','-SamplingSeconds','30')
        $entry.arguments = $arguments
        Save-DiagnosticJson $entry (Join-Path $out 'call.json')
        try {
            & $plan.pwsh @arguments *> (Join-Path $out 'launcher.log')
            $entry.exitCode = $LASTEXITCODE
            if ($entry.exitCode -ne 0) { throw "Comparison exited $($entry.exitCode); retained evidence is at $out" }
            Assert-DiagnosticInputs
            $comparisonPath = Join-Path $out 'comparison.json'
            $report = Get-Content -LiteralPath $comparisonPath -Raw | ConvertFrom-Json
            $rows = @($report.results)
            if ($report.launchMode -cne $call.mode -or @($report.players).Count -ne 1 -or $report.players[0].name -cne 'Aperture' -or $rows.Count -ne 1) { throw 'Unexpected mode/player/process count.' }
            if ($report.players[0].exeHash -ine $plan.executableSha256 -or $report.players[0].runtimeManifestSha256 -ine $plan.runtimeManifestSha256 -or $report.apertureStatusProbeSha256 -ine $plan.statusProbeSha256 -or $report.mediaSha256 -ine $plan.mediaSha256) { throw 'Reported input identity mismatch.' }
            $row = $rows[0]
            if ($row.status -cne 'sampled' -or $row.playbackVerification -cne 'advancing-playback-verified' -or @($row.samples).Count -ne 7 -or $row.alignedPlaybackPosition -lt 20.5) { throw 'Resource/status sample qualification failed.' }
            if ($row.videoCanvas.clientWidth -ne 1280 -or $row.videoCanvas.clientHeight -ne 720) { throw 'Actual video canvas differs from 1280x720.' }
            foreach ($sample in $row.samples) {
                if ($sample.visibleCanvasBefore.status -cne 'validated' -or $sample.visibleCanvasAfter.status -cne 'validated' -or $sample.privateBytes -le 0 -or $sample.workingSetBytes -le 0) { throw 'Visibility or memory sample invalid.' }
            }
            $entry.comparisonSha256 = (Get-FileHash -LiteralPath $comparisonPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $entry.pid = $row.pid
            $entry.cleanup = $row.cleanup
            $entry.sampleCount = @($row.samples).Count
            $entry.status = 'validated'
        } catch {
            $entry.status = 'failed'
            $entry.failure = $_.Exception.Message
        } finally {
            $entry.finishedUtc = [DateTime]::UtcNow.ToString('o')
            Save-DiagnosticJson $entry (Join-Path $out 'call.json')
            $index.calls += $entry
            Save-DiagnosticJson $index (Join-Path $OutputRoot 'index.json')
        }
        if ($entry.status -ne 'validated') { throw $entry.failure }
    }
    $index.status = 'complete'
} catch {
    $index.status = 'failed'
    $index.failure = $_.Exception.Message
    throw
} finally {
    $index.finishedUtc = [DateTime]::UtcNow.ToString('o')
    Save-DiagnosticJson $index (Join-Path $OutputRoot 'index.json')
}
