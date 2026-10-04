[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BeforeCsv,
    [Parameter(Mandatory)][string]$AfterCsv,
    [Parameter(Mandatory)][string]$OutputJson,
    [switch]$ProgressOnly,
    [switch]$DuplicatesOnly,
    [switch]$ExtendedOnly
)
$ErrorActionPreference = 'Stop'
if (@(@($ProgressOnly, $DuplicatesOnly, $ExtendedOnly) | Where-Object { $_ }).Count -gt 1) { throw 'Select only one benchmark workload.' }
$before = @(Import-Csv -LiteralPath $BeforeCsv)
$after = @(Import-Csv -LiteralPath $AfterCsv)
function Median($values) {
    $sorted = @($values | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) } | Sort-Object)
    if (-not $sorted.Count) { throw 'No benchmark samples' }
    return $sorted[[int][Math]::Floor($sorted.Count / 2)]
}
$names = @('flat', 'folder', 'series', 'capped', 'manual', 'smart_playlist',
    'search', 'nearly_unlimited', 'extension', 'index_build', 'multi_token_path',
    'multi_token_title', 'search_miss')
$modes = @(0..12)
if ($ProgressOnly) {
    $modes = @(13, 14)
    $names = @('metadata_refresh', 'forced_text_rebuild')
}
if ($DuplicatesOnly) {
    $modes = @(15, 16, 17, 18)
    $names = @('duplicates_duration_windows', 'duplicates_dense_metadata', 'duplicates_file_identity', 'duplicates_unknown_resolution')
}
if ($ExtendedOnly) {
    $modes = @(19..25)
    $names = @('collapsed_folders', 'capped_folders', 'index_with_history', 'dense_unknown_resolution',
        'dense_mixed_resolution', 'mixed_threshold_duplicates', 'library_search_miss')
}
if ($before.Count -ne 7 * $modes.Count -or $after.Count -ne 7 * $modes.Count) { throw 'Expected seven samples for each workload' }
$summary = for ($index = 0; $index -lt $modes.Count; ++$index) {
    $mode = $modes[$index]
    $b = @($before | Where-Object { [int]$_.mode -eq $mode })
    $a = @($after | Where-Object { [int]$_.mode -eq $mode })
    if ($b.Count -ne 7 -or $a.Count -ne 7) { throw "Missing samples for mode $mode" }
    $signatures = @(@($b) + @($a) | ForEach-Object { "$($_.items):$($_.groups):$($_.order_hash)" } | Select-Object -Unique)
    if ($signatures.Count -ne 1) { throw "Query results differ for mode $mode" }
    $beforeMs = Median $b.ms
    $afterMs = Median $a.ms
    $beforePeak = Median $b.peak_live_cpp_bytes
    $afterPeak = Median $a.peak_live_cpp_bytes
    [pscustomobject][ordered]@{
        mode = $names[$index]
        samplesPerVersion = 7
        beforeMedianMs = $beforeMs
        afterMedianMs = $afterMs
        elapsedReductionPercent = [Math]::Round(100 * (1 - $afterMs / $beforeMs), 2)
        beforePeakCppBytes = $beforePeak
        afterPeakCppBytes = $afterPeak
        peakReductionPercent = if ($beforePeak) { [Math]::Round(100 * (1 - $afterPeak / $beforePeak), 2) } else { 0 }
        beforeAllocations = Median $b.allocations
        afterAllocations = Median $a.allocations
        items = [int]$b[0].items
        groups = [int]$b[0].groups
        orderHash = $b[0].order_hash
    }
}
$summary | ConvertTo-Json | Set-Content -LiteralPath $OutputJson -Encoding UTF8
$summary | Format-Table mode,beforeMedianMs,afterMedianMs,elapsedReductionPercent,beforePeakCppBytes,afterPeakCppBytes -AutoSize
