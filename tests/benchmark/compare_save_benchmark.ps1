[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BeforeCsv,
    [Parameter(Mandatory)][string]$AfterCsv,
    [Parameter(Mandatory)][string]$OutputJson
)
$ErrorActionPreference = 'Stop'
$before = @(Import-Csv -LiteralPath $BeforeCsv)
$after = @(Import-Csv -LiteralPath $AfterCsv)
function Median($values) {
    $sorted = @($values | ForEach-Object { [double]$_ } | Sort-Object)
    if (-not $sorted.Count) { throw 'No benchmark samples' }
    if ($sorted.Count % 2) { return $sorted[[int][Math]::Floor($sorted.Count / 2)] }
    return ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2
}
if ($before.Count -ne 42 -or $after.Count -ne 42) { throw 'Expected seven samples for each of six workloads' }
$summary = foreach ($count in @(500,25000)) {
    foreach ($mode in @(0,1,2)) {
        $b = @($before | Where-Object { [int]$_.items -eq $count -and [int]$_.mode -eq $mode })
        $a = @($after | Where-Object { [int]$_.items -eq $count -and [int]$_.mode -eq $mode })
        if ($b.Count -ne 7 -or $a.Count -ne 7) { throw "Missing samples for $count/$mode" }
        $signatures = @(@($b) + @($a) | ForEach-Object { "$($_.json_bytes):$($_.content_hash)" } | Select-Object -Unique)
        if ($signatures.Count -ne 1) { throw "Saved bytes differ for $count/$mode" }
        $beforeMs = Median $b.ms
        $afterMs = Median $a.ms
        $beforePeak = Median $b.peak_live_cpp_bytes
        $afterPeak = Median $a.peak_live_cpp_bytes
        [pscustomobject][ordered]@{
            items = $count
            mode = @('first','unchanged','progress')[$mode]
            samplesPerVersion = 7
            beforeMedianMs = $beforeMs
            afterMedianMs = $afterMs
            elapsedReductionPercent = [Math]::Round(100 * (1 - $afterMs / $beforeMs), 2)
            beforePeakCppBytes = $beforePeak
            afterPeakCppBytes = $afterPeak
            peakReductionPercent = [Math]::Round(100 * (1 - $afterPeak / $beforePeak), 2)
            jsonBytes = [long]$b[0].json_bytes
            contentHash = $b[0].content_hash
        }
    }
}
$summary | ConvertTo-Json | Set-Content -LiteralPath $OutputJson -Encoding UTF8
$summary | Format-Table items,mode,beforeMedianMs,afterMedianMs,elapsedReductionPercent,beforePeakCppBytes,afterPeakCppBytes,peakReductionPercent -AutoSize
