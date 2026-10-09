$ErrorActionPreference='Stop'
$root='C:\Users\Alperen\AppData\Local\Temp\aperture-validation-20261009'
foreach ($kind in @('query','save')) {
    $rows=@{before=@(); after=@()}
    foreach ($round in 0..2) {
        $order=if ($round % 2) { @('after','before') } else { @('before','after') }
        foreach ($variant in $order) {
            $output=& "$root/$kind-$variant/$kind-benchmark.exe"
            if ($LASTEXITCODE -ne 0) { throw "$kind $variant failed" }
            $output | Set-Content "$root/$kind-$variant/repeat-$round.csv"
            $rows[$variant] += @($output | ConvertFrom-Csv)
            Write-Output "Recorded $kind $variant round $round"
        }
    }
    function Median($values) { $sorted=@($values | ForEach-Object {[double]::Parse($_,[Globalization.CultureInfo]::InvariantCulture)} | Sort-Object); $sorted[[int][Math]::Floor($sorted.Count/2)] }
    $groups=if ($kind -eq 'save') {@('items','mode')} else {@('mode')}
    $summary=foreach ($group in ($rows.before | Group-Object -Property $groups)) {
        $before=@($group.Group)
        $after=@($rows.after | Where-Object { $_.mode -eq $before[0].mode -and $_.items -eq $before[0].items })
        $hashField=if ($kind -eq 'save') {'content_hash'} else {'order_hash'}
        $hashes=@(@($before)+@($after) | ForEach-Object {$_.$hashField} | Select-Object -Unique)
        if ($before.Count -ne 21 -or $after.Count -ne 21 -or $hashes.Count -ne 1) { throw "Mismatched samples/results: $kind $($group.Name)" }
        $b=Median $before.ms; $a=Median $after.ms
        [pscustomobject]@{mode=$before[0].mode; items=$before[0].items; samplesPerVersion=21; beforeMedianMs=$b; afterMedianMs=$a; reductionPercent=[Math]::Round(100*(1-$a/$b),2); beforePeakCppBytes=(Median $before.peak_live_cpp_bytes); afterPeakCppBytes=(Median $after.peak_live_cpp_bytes); resultHash=$hashes[0]}
    }
    $summary | ConvertTo-Json | Set-Content "$root/$kind-repeated-comparison.json"
    $summary | Format-Table mode,items,beforeMedianMs,afterMedianMs,reductionPercent
}
