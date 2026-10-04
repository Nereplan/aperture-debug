<#
.SYNOPSIS
    Previews or clears a disposable Aperture performance session under Codex scratch.
.DESCRIPTION
    Only the named performance session is eligible. Links and unexpected top-level
    entries are rejected before deletion. Files are removed individually; directories
    are removed only when empty. Benchmark evidence saved in the repository is retained.
.EXAMPLE
    .\scripts\clean_performance_scratch.ps1
.EXAMPLE
    .\scripts\clean_performance_scratch.ps1 -Apply
#>
[CmdletBinding()]
param(
    [ValidatePattern('^aperture-subtitle-performance-[0-9]{8}$')]
    [string]$SessionName = 'aperture-subtitle-performance-20261005',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$scratchParent = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Codex\tools\scratch'))
$sessionRoot = [IO.Path]::GetFullPath((Join-Path $scratchParent $SessionName))
$sessionPrefix = $sessionRoot + [IO.Path]::DirectorySeparatorChar
if ((Split-Path -Parent $sessionRoot) -ne $scratchParent) {
    throw 'The session must be directly inside Codex scratch.'
}
if (-not (Test-Path -LiteralPath $sessionRoot)) {
    Write-Host "Nothing to clean: $sessionRoot"
    return
}

function Assert-SessionPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -ne $sessionRoot -and
        -not $full.StartsWith($sessionPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the selected session: $full"
    }
    # Inspect all ancestors, including the scratch parent, without following links.
    $node = Get-Item -LiteralPath $full -Force
    while ($null -ne $node) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing linked path: $($node.FullName)"
        }
        $parent = Split-Path -Parent $node.FullName
        if (-not $parent) { break }
        $node = Get-Item -LiteralPath $parent -Force
    }
}

Assert-SessionPath $sessionRoot
if (-not (Test-Path -LiteralPath $sessionRoot -PathType Container)) {
    throw 'The session path must be a directory.'
}
$knownFiles = @(
    'app-workflows.log', 'baseline.zip', 'benchmark.log', 'build-and-release.log',
    'build.cmd', 'core.rsp', 'final-build-and-release.log', 'harness.rsp',
    'local-release-test.log', 'subtitle-fuzz.log', 'subtitle-unit.log',
    'ui-benchmark.log', 'ui-tests.cpp', 'ui-tests.exe', 'ui-tests.log', 'ui-tests.obj'
)
$knownDirectories = @('baseline', 'objects', 'results', 'ui-results')
$pending = [Collections.Generic.Stack[string]]::new()
$pending.Push($sessionRoot)
$files = [Collections.Generic.List[object]]::new()
$directories = [Collections.Generic.List[string]]::new()
while ($pending.Count -gt 0) {
    $directory = $pending.Pop()
    Assert-SessionPath $directory
    $directories.Add($directory)
    foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
        Assert-SessionPath $item.FullName
        if ($directory -eq $sessionRoot) {
            $known = if ($item.PSIsContainer) {
                $item.Name -in $knownDirectories -or $item.Name -match '^compile-[0-9a-f]{32}$'
            } else { $item.Name -in $knownFiles }
            if (-not $known) { throw "Unexpected entry; inspect before cleaning: $($item.FullName)" }
        }
        if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        else {
            $files.Add([pscustomobject]@{
                Path = $item.FullName
                Bytes = $item.Length
                Modified = $item.LastWriteTimeUtc
            })
        }
    }
}

[long]$bytes = ($files | Measure-Object Bytes -Sum).Sum
Write-Host "Session: $sessionRoot"
Write-Host ("Temporary files: {0}; directories: {1}; size: {2:N2} MiB" -f
    $files.Count, $directories.Count, ($bytes / 1MB))
if (-not $Apply) {
    $files | ForEach-Object { Write-Host $_.Path.Substring($sessionPrefix.Length) }
    Write-Host 'Preview only. Use -Apply to clear this session.'
    return
}

foreach ($entry in $files) {
    Assert-SessionPath $entry.Path
    $current = Get-Item -LiteralPath $entry.Path -Force
    if ($current.PSIsContainer -or $current.Length -ne $entry.Bytes -or
        $current.LastWriteTimeUtc -ne $entry.Modified) {
        throw "File changed during cleanup; stopping: $($entry.Path)"
    }
    Remove-Item -LiteralPath $entry.Path -Force
}
foreach ($directory in ($directories | Sort-Object { $_.Length } -Descending)) {
    Assert-SessionPath $directory
    # Nonrecursive removal fails safely if new content appeared during cleanup.
    [IO.Directory]::Delete($directory, $false)
}
Write-Host ("Cleared {0} temporary files ({1:N2} MiB)." -f $files.Count, ($bytes / 1MB))
