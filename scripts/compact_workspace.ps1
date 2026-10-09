<#
.SYNOPSIS
    Reports or removes known regenerable workspace artifacts. Dry-run by default.
#>
[CmdletBinding()]
param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\', '/')
$prefix = $root + [IO.Path]::DirectorySeparatorChar
function Assert-LocalFile([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes workspace: $full" }
    # Never follow a junction/symlink in any ancestor of a cleanup target.
    $node = Get-Item -LiteralPath $full -Force
    while ($node.FullName -ne $root) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing reparse point: $($node.FullName)" }
        $node = Get-Item -LiteralPath (Split-Path -Parent $node.FullName) -Force
        if (-not $node) { throw "Could not validate ancestor of $full" }
    }
    return $full
}
$candidates = [Collections.Generic.List[string]]::new()
foreach ($relative in @('vc140.pdb', 'debug/Aperture.Release.exe', 'release/Aperture.Debug.exe', 'release/Aperture.pdb')) {
    $path = Join-Path $root $relative
    if (Test-Path -LiteralPath $path -PathType Leaf) { $candidates.Add($path) }
}
foreach ($relative in @('scratch', 'cache', 'debug/cache', 'release/cache')) {
    $dir = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $dir -File -Recurse -Force) {
        $isCache = $relative -ne 'scratch'
        $isBuildOutput = $file.Extension -in @('.obj', '.pdb', '.ilk', '.res') -or
            $file.Name -in @('query-benchmark.exe', 'Aperture.exe')
        if ($isCache -or $isBuildOutput) { $candidates.Add($file.FullName) }
    }
}
foreach ($file in Get-ChildItem -LiteralPath $root -Filter '*.pyc' -Recurse -Force -ErrorAction SilentlyContinue) {
    $candidates.Add($file.FullName)
}
$plan = @($candidates | Sort-Object -Unique | ForEach-Object {
    $full = Assert-LocalFile $_
    $file = Get-Item -LiteralPath $full
    [pscustomobject]@{ Path = $full; Bytes = $file.Length }
})
$bytes = ($plan | Measure-Object Bytes -Sum).Sum
Write-Host ("Known regenerable artifacts: {0} files, {1:N2} MiB" -f $plan.Count, ($bytes / 1MB))
$plan | ForEach-Object { Write-Host ("{0,12:N0}  {1}" -f $_.Bytes, $_.Path.Substring($prefix.Length)) }
if ($Apply) {
    foreach ($entry in $plan) {
        $full = Assert-LocalFile $entry.Path
        Remove-Item -LiteralPath $full -Force
    }
    Get-ChildItem -LiteralPath $root -Directory -Filter '__pycache__' -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { (Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0 } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -Recurse }
    Write-Host ("Removed {0:N2} MiB. Source, reports, profiles and backups retained." -f ($bytes / 1MB))
} else { Write-Host 'Dry run. Use -Apply to remove only these files.' }
