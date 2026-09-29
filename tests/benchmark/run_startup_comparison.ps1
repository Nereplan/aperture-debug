# Compare fresh-process startup using identical isolated profiles and alternating
# order. OS/driver/file caches are not flushed: this is not a reboot-cold test.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$BeforeBinary,
    [Parameter(Mandatory=$true)][string]$AfterBinary,
    [ValidateRange(1,100)][int]$Samples = 9,
    [ValidateRange(0,100000)][int]$LibrarySize = 0
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = Join-Path $root ('scratch/startup-comparison-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$records = @(for ($i = 0; $i -lt $LibrarySize; $i++) {
    [ordered]@{ id="med_$i"; filePath=".\media\sample_$i.mp4"; title="Sample $i";
        durationSec=120; status='new'; fileSizeBytes=1048576; addedTimestamp=1788000000;
        videoWidth=1920; videoHeight=1080 }
})
$libraryJson = ConvertTo-Json -InputObject $records -Depth 4 -Compress
foreach ($variant in @('before','after')) {
    $stage = Join-Path $output $variant
    New-Item -ItemType Directory -Path (Join-Path $stage 'data') -Force | Out-Null
    $binary = if ($variant -eq 'before') { $BeforeBinary } else { $AfterBinary }
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $binary).Path -Destination (Join-Path $stage 'Aperture.exe')
    foreach ($asset in @('fonts','locales','libmpv-2.dll')) {
        Copy-Item -LiteralPath (Join-Path $root $asset) -Destination $stage -Recurse
    }
    [IO.File]::WriteAllText((Join-Path $stage 'data/library.json'), $libraryJson, $utf8)
}
$results = [Collections.Generic.List[object]]::new()
for ($iteration = 0; $iteration -le $Samples; $iteration++) {
    $order = if (($iteration % 2) -eq 0) { @('before','after') } else { @('after','before') }
    foreach ($variant in $order) {
        $stage = Join-Path $output $variant
        $samplePath = Join-Path $stage "sample-$iteration.json"
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = Join-Path $stage 'Aperture.exe'
        $start.Arguments = "--benchmark-probe=`"$samplePath`""
        $start.WorkingDirectory = $stage
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $process = [Diagnostics.Process]::Start($start)
        try {
            if (-not $process.WaitForExit(30000)) {
                $process.Kill()
                throw "Startup timed out: $variant sample $iteration"
            }
            if ($process.ExitCode -ne 0) { throw "Startup failed: $variant sample $iteration" }
        } finally { $process.Dispose() }
        $sample = Get-Content -LiteralPath $samplePath -Raw | ConvertFrom-Json
        if (-not $sample.ready -or -not $sample.libraryReady -or $sample.populatedMs -lt 0) {
            throw "Library did not render: $variant sample $iteration"
        }
        $reportFile = Get-ChildItem -LiteralPath (Join-Path $stage 'diagnostics/perf') -Filter 'startup-*.json' |
            Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        $report = Get-Content -LiteralPath $reportFile.FullName -Raw | ConvertFrom-Json
        # Timestamp milestones include the old process-entry resource capture,
        # which the legacy probe's startupMs timer excludes. Compare both.
        $gpu = @($report.milestones | Where-Object name -eq 'first-gpu-shell')
        $populated = @($report.milestones | Where-Object name -eq 'first-populated-frame')
        if ($gpu.Count -ne 1 -or $populated.Count -ne 1) { throw 'Missing startup milestones' }
        Copy-Item -LiteralPath $reportFile.FullName -Destination (Join-Path $stage "milestones-$iteration.json")
        if ($iteration -gt 0) {
            $results.Add([pscustomobject]@{ variant=$variant; sample=$iteration; librarySize=$LibrarySize;
                shownMs=$sample.shownMs; startupMs=$sample.startupMs; populatedMs=$sample.populatedMs;
                gpuSinceEntryMs=$gpu[0].msSinceStart; populatedSinceEntryMs=$populated[0].msSinceStart;
                workingSetBytes=$sample.workingSetBytes; privateBytes=$sample.privateBytes })
        }
    }
}
$results | Export-Csv -LiteralPath (Join-Path $output 'samples.csv') -NoTypeInformation
function Median($values) {
    $sorted = @($values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}
$summary = [ordered]@{ librarySize=$LibrarySize; samplesPerVariant=$Samples;
    method='Alternating fresh processes, one warmup each; OS caches retained'; metrics=[ordered]@{} }
foreach ($metric in @('shownMs','startupMs','populatedMs','gpuSinceEntryMs','populatedSinceEntryMs','workingSetBytes','privateBytes')) {
    $before = Median @($results | Where-Object variant -eq 'before' | ForEach-Object { $_.$metric })
    $after = Median @($results | Where-Object variant -eq 'after' | ForEach-Object { $_.$metric })
    $summary.metrics[$metric] = [ordered]@{ before=$before; after=$after;
        reductionPercent=[Math]::Round(100 * (1 - $after / $before), 1) }
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'summary.json')
$summary | ConvertTo-Json -Depth 5
Write-Host "Results: $output"
