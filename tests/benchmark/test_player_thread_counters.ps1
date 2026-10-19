$ErrorActionPreference = 'Stop'
Add-Type -Path @(
    (Join-Path $PSScriptRoot 'PlayerThreadCounters.cs'),
    (Join-Path $PSScriptRoot 'PlayerBenchmarkWindow.cs'),
    (Join-Path $PSScriptRoot 'PlayerThreadCounters.Tests.cs')
)
$checks = @([PlayerThreadCounterTests]::Run())
foreach ($check in $checks) { Write-Output "PASS $check" }
if ($checks.Count -ne 6) { throw 'Incomplete thread-counter validation.' }
$snapshot = [PlayerThreadCounters]::Capture($PID)
$roundTrip = $snapshot | ConvertTo-Json -Depth 8 | ConvertFrom-Json
$readable = @($roundTrip.threads | Where-Object { $null -ne $_.cpuCycles -and $null -ne $_.createdFileTimeUtc })
if ($roundTrip.pid -ne $PID -or !$readable.Count -or $readable[0].createdFileTimeUtc -isnot [string]) { throw 'Raw thread identity/cycles were lost in JSON.' }
Write-Output 'PASS JSON preserves raw cycles and lossless creation identities'
Write-Output "$($checks.Count + 1) thread-counter checks passed; no player launched."
