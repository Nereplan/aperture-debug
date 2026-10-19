[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('Codex/tools/scratch/cleanup-contract-' + [Guid]::NewGuid().ToString('N'))
$paths = @('scripts','scratch/build/ninja-Release','scratch/libmpv-tests/run-001','scratch/libmpv-tests/media','scratch/libmpv-build/src','scratch/libmpv-build/prefix','release/data','debug/cache','debug/data/backups','outside')
foreach ($path in $paths) { New-Item -ItemType Directory -Path (Join-Path $fixture $path) -Force | Out-Null }
$cleaner = Join-Path $fixture 'scripts/compact_workspace.ps1'
Copy-Item -LiteralPath (Join-Path $repo 'scripts/compact_workspace.ps1') -Destination $cleaner
$generated = @('scratch/compiler.obj','debug/cache/thumbnail.bin','release/Aperture.pdb','scratch/build/ninja-Release/unit.obj','scratch/libmpv-tests/run-001/player.exe','scratch/libmpv-tests/run-001/libmpv-2.dll','scratch/libmpv-build/prefix/native.lib')
$retained = @('debug/data/backups/library.json.bak.1','release/Aperture.exe','release/libmpv-2.dll','scratch/libmpv-tests/run-001/summary.json','scratch/libmpv-tests/run-001/frame.png','scratch/libmpv-tests/media/movie.mp4','scratch/libmpv-build/src/source.c','release/data/library.json','outside/sentinel')
foreach ($path in ($generated + $retained)) { [IO.File]::WriteAllText((Join-Path $fixture $path), 'fixture') }
$plan = Join-Path $fixture 'plan.json'
& $cleaner -ReportPath $plan | Out-Null
if (@($generated | Where-Object { -not (Test-Path -LiteralPath (Join-Path $fixture $_)) }).Count) { throw 'Dry run removed a file.' }
if ((Get-Content -Raw -LiteralPath $plan | ConvertFrom-Json).files -ne $generated.Count) { throw 'Plan did not identify the exact generated files.' }
Write-Output 'PASS dry run identifies generated outputs without changing files'
$lock = [IO.File]::Open((Join-Path $fixture 'scratch/libmpv-build/.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $rejected = $false
    try { & $cleaner -Apply | Out-Null } catch { if ($_.Exception.Message -match 'Native dependency build is active') { $rejected = $true } else { throw } }
    if (-not $rejected -or @($generated | Where-Object { -not (Test-Path -LiteralPath (Join-Path $fixture $_)) }).Count) { throw 'Active native build was not protected.' }
} finally { $lock.Dispose() }
Write-Output 'PASS active native dependency builds block cleanup before deletion'
& $cleaner -Apply -ReportPath (Join-Path $fixture 'applied.json') | Out-Null
if (@($generated | Where-Object { Test-Path -LiteralPath (Join-Path $fixture $_) }).Count) { throw 'Generated output survived cleanup.' }
if (@($retained | Where-Object { -not (Test-Path -LiteralPath (Join-Path $fixture $_)) }).Count) { throw 'Cleanup removed retained data.' }
Write-Output 'PASS apply removes binaries/caches and retains sources, media, profiles and evidence'
& $cleaner -ReportPath (Join-Path $fixture 'empty.json') | Out-Null
if ((Get-Content -Raw -LiteralPath (Join-Path $fixture 'empty.json') | ConvertFrom-Json).files -ne 0) { throw 'Second plan was not empty.' }
Write-Output 'PASS repeated cleanup is empty'
New-Item -ItemType Junction -Path (Join-Path $fixture 'scratch/build/ninja-Debug') -Target (Join-Path $fixture 'outside') | Out-Null
$rejected = $false
try { & $cleaner -Apply | Out-Null } catch { if ($_.Exception.Message -match 'Reparse point') { $rejected = $true } else { throw } }
if (-not $rejected -or -not (Test-Path -LiteralPath (Join-Path $fixture 'outside/sentinel'))) { throw 'Cleanup did not reject a junction target.' }
Write-Output 'PASS junction targets are rejected before deletion'
Write-Output "5 cleanup contract checks passed; fixture: $fixture"
