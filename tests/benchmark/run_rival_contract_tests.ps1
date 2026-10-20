# Pure observer/evidence contracts. No media player is launched.
$ErrorActionPreference = 'Stop'
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $PSScriptRoot 'test_evaluate_rival.py')
if ($LASTEXITCODE -ne 0) { throw 'Rival evidence contracts failed.' }
& (Join-Path $PSScriptRoot 'test_player_comparison_identity.ps1')
& (Join-Path $PSScriptRoot 'test_player_video_readiness.ps1')
Write-Output 'Rival observer/evidence contracts passed without launching players.'
