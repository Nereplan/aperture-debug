[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + '\'
$fixture = Join-Path $tempBase ('Aperture-cleanup-test-' + [Guid]::NewGuid().ToString('N'))
try {
    foreach ($dir in @('scripts','src','scratch','debug/data/backups','debug/cache','release')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $dir) -Force | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $root 'scripts/compact_workspace.ps1') -Destination (Join-Path $fixture 'scripts')
    $remove = @('scratch/compiler.obj','scratch/query-benchmark.exe','debug/cache/thumbnail.bin','release/Aperture.pdb')
    $keep = @('src/main.cpp','scratch/results.csv','scratch/source-backup.cpp','debug/data/library.json','debug/data/backups/library.json.bak.1','release/Aperture.exe','release/libmpv-2.dll')
    foreach ($path in $remove + $keep) { Set-Content -LiteralPath (Join-Path $fixture $path) -Value 'sentinel' }
    $script = Join-Path $fixture 'scripts/compact_workspace.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Cleanup dry run failed' }
    foreach ($path in $remove + $keep) {
        if (-not (Test-Path -LiteralPath (Join-Path $fixture $path))) { throw "Dry run removed $path" }
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Apply *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Cleanup apply failed' }
    foreach ($path in $remove) { if (Test-Path -LiteralPath (Join-Path $fixture $path)) { throw "Artifact survived: $path" } }
    foreach ($path in $keep) {
        if ((Get-Content -LiteralPath (Join-Path $fixture $path) -Raw).Trim() -ne 'sentinel') { throw "Protected file changed: $path" }
    }
    # A second run must be harmless and must retain reports and personal data.
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Apply *> $null
    if ($LASTEXITCODE -ne 0) { throw 'Repeated cleanup failed' }
    Write-Host 'PASS cleanup dry-run, targeted removal, protected data and repeated application'
} finally {
    $resolved = [IO.Path]::GetFullPath($fixture)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-cleanup-test-*') { throw "Unsafe fixture cleanup: $resolved" }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
