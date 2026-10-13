[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaselineSourceDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$baseline = (Resolve-Path -LiteralPath $BaselineSourceDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$build = Join-Path $tempBase ('Aperture-maintenance-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
. (Join-Path $root 'tests/support/native_build.ps1')
try {
    foreach ($mode in @('before','after')) {
        $source = if ($mode -eq 'before') { $baseline } else { Join-Path $root 'src' }
        $directory = Join-Path $build $mode
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $sources = @((Join-Path $PSScriptRoot 'maintenance_resource_benchmark.cpp'))
        foreach ($file in @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery',
                'GlobalSearch','TextInputModel','FolderWatcher','MediaFoundation','MediaProbeIsolation')) {
            $sources += Join-Path $source ($file + '.cpp')
        }
        Build-ApertureTestHarness -Sources $sources -Output (Join-Path $directory 'benchmark.exe') -SourceDirectory $source `
            -Libraries @('mfreadwrite.lib','oleaut32.lib','propsys.lib')
        'mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,items,changed,result_hash' |
            Set-Content -LiteralPath (Join-Path $output "$mode.csv") -Encoding UTF8
    }
    # Compile both versions first, then alternate with no concurrent test/build.
    foreach ($sample in 0..7) {
        $order = if ($sample % 2) { @('after','before') } else { @('before','after') }
        foreach ($mode in $order) {
            $rows = & (Join-Path $build "$mode/benchmark.exe") $sample (Join-Path $build "$mode/cache-root")
            if ($LASTEXITCODE -ne 0) { throw "Maintenance benchmark $mode sample $sample failed." }
            $rows | Select-Object -Skip 1 | Add-Content -LiteralPath (Join-Path $output "$mode.csv") -Encoding UTF8
            Write-Host "Recorded $mode sample $sample"
        }
    }
    $before = @(Import-Csv -LiteralPath (Join-Path $output 'before.csv'))
    $after = @(Import-Csv -LiteralPath (Join-Path $output 'after.csv'))
    if ($before.Count -ne 48 -or $after.Count -ne 48) { throw 'Incomplete maintenance measurements.' }
    foreach ($a in $before) {
        $b = @($after | Where-Object { $_.mode -eq $a.mode -and $_.sample -eq $a.sample })
        if ($b.Count -ne 1 -or $a.items -ne $b[0].items -or $a.changed -ne $b[0].changed -or $a.result_hash -ne $b[0].result_hash) {
            throw "Changed results for $($a.mode) sample $($a.sample)"
        }
    }
    function Median($rows, $field) {
        $values = @($rows | ForEach-Object { [double]$_.$field } | Sort-Object)
        return $values[[int]($values.Count / 2)]
    }
    $comparisons = foreach ($mode in @($before.mode | Select-Object -Unique)) {
        $a = @($before | Where-Object { $_.mode -eq $mode -and $_.sample -ne '0' })
        $b = @($after | Where-Object { $_.mode -eq $mode -and $_.sample -ne '0' })
        $beforeMs = Median $a 'ms'; $afterMs = Median $b 'ms'
        [pscustomobject]@{ mode=$mode; samplesPerVersion=7; beforeMedianMs=$beforeMs; afterMedianMs=$afterMs;
            timeReductionPercent=(100 * ($beforeMs - $afterMs) / $beforeMs);
            beforePeakCppBytes=(Median $a 'peak_live_cpp_bytes'); afterPeakCppBytes=(Median $b 'peak_live_cpp_bytes');
            beforeAllocations=(Median $a 'allocations'); afterAllocations=(Median $b 'allocations');
            items=$a[0].items; changed=$a[0].changed; resultHash=$a[0].result_hash }
    }
    $comparisons | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'comparison.json') -Encoding UTF8
    $comparisons | Format-Table mode,beforeMedianMs,afterMedianMs,beforeAllocations,afterAllocations -AutoSize
} finally {
    $resolved = [IO.Path]::GetFullPath($build)
    if (-not $resolved.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-maintenance-[a-f0-9]{32}$') { throw 'Unsafe maintenance build cleanup.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
