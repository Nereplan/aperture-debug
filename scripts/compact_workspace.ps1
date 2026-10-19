<#
.SYNOPSIS
Plans or removes known, reproducible workspace build products and runtime caches.
.DESCRIPTION
Run without arguments to inspect the plan. Use -Apply to remove generated build/runtime caches
and compiled test/experiment binaries. Source trees, downloads, media fixtures,
logs, captures, profiles, backups and historical evidence stay intact.
Use -ReportPath for a new JSON record of the selected paths and byte counts.
#>
[CmdletBinding()]
param([switch]$Apply, [string]$ReportPath = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$scratchRoot = [IO.Path]::GetFullPath((Join-Path $repo 'scratch'))
$workspacePrefix = $repo.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
$entries = [Collections.Generic.List[object]]::new()
$binaries = @('.dll','.exe','.obj','.pdb','.ilk','.res','.a','.lib')

function Assert-WorkspacePath([string]$path) {
    $resolved = [IO.Path]::GetFullPath($path)
    if (-not $resolved.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe cleanup path: $resolved" }
    $ancestor = $resolved
    while ($ancestor.Length -ge $repo.Length) {
        if ((Test-Path -LiteralPath $ancestor) -and ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Reparse point: $ancestor" }
        if ($ancestor -eq $repo) { break }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $resolved
}
function Add-Target([string]$path, [bool]$directory) {
    $resolved = Assert-WorkspacePath $path
    if (-not (Test-Path -LiteralPath $resolved)) { return }
    $files = if ($directory) { @(Get-ChildItem -LiteralPath $resolved -Recurse -File -Force) } else { @(Get-Item -LiteralPath $resolved -Force) }
    if ($directory -and @(Get-ChildItem -LiteralPath $resolved -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw "Reparse point below $resolved" }
    $bytes = [long]0; foreach ($file in $files) { $bytes += $file.Length }
    $entries.Add([ordered]@{path=$resolved;directory=$directory;files=$files.Count;bytes=$bytes})
}

foreach ($relative in @('build/ninja-Debug','build/ninja-Release','libmpv-build/ffmpeg-build','libmpv-build/placebo-build','libmpv-build/mpv-build','libmpv-build/build-shaderc','libmpv-build/build-spirv-cross','libmpv-build/prefix','libmpv-build/stage')) {
    Add-Target (Join-Path $scratchRoot $relative) $true
}
$native = Join-Path $scratchRoot 'libmpv-build'
if (Test-Path -LiteralPath $native) {
    foreach ($dir in Get-ChildItem -LiteralPath $native -Directory -Force) {
        if ($dir.Name -in @('src','shader-src','downloads','recipe','ffmpeg-build','placebo-build','mpv-build','build-shaderc','build-spirv-cross','prefix','stage')) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -File -Force) {
            if ($file.Extension -in $binaries) { Add-Target $file.FullName $false }
        }
    }
}
$runs = Join-Path $scratchRoot 'libmpv-tests'
if (Test-Path -LiteralPath $runs) {
    foreach ($dir in Get-ChildItem -LiteralPath $runs -Directory -Filter 'run-*') {
        foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -File -Force) {
            if ($file.Extension -in $binaries) { Add-Target $file.FullName $false }
        }
    }
}
foreach ($relative in @('backend-optimization','evaluation-fixes','hoverprobe','bench-stage')) {
    $path = Join-Path $scratchRoot $relative
    if (Test-Path -LiteralPath $path) {
        foreach ($file in Get-ChildItem -LiteralPath $path -Recurse -File -Force) {
            if ($file.Extension -in $binaries) { Add-Target $file.FullName $false }
        }
    }
}
# Retain the existing compactor's runtime-cache and root compiler-output scope.
foreach ($relative in @('vc140.pdb','debug/Aperture.Release.exe','release/Aperture.Debug.exe','release/Aperture.pdb')) {
    Add-Target (Join-Path $repo $relative) $false
}
foreach ($relative in @('cache','debug/cache','release/cache')) {
    Add-Target (Join-Path $repo $relative) $true
}
if (Test-Path -LiteralPath $scratchRoot) {
    foreach ($file in Get-ChildItem -LiteralPath $scratchRoot -File -Force) {
        if ($file.Extension -in @('.obj','.pdb','.ilk','.res') -or $file.Name -in @('query-benchmark.exe','Aperture.exe')) { Add-Target $file.FullName $false }
    }
}
foreach ($file in Get-ChildItem -LiteralPath $repo -Filter '*.pyc' -File -Recurse -Force -ErrorAction SilentlyContinue) {
    # Whole build caches and runtime caches already cover their bytecode.
    if (-not @($entries | Where-Object { $_.directory -and $file.FullName.StartsWith($_.path + '\',[StringComparison]::OrdinalIgnoreCase) }).Count) { Add-Target $file.FullName $false }
}
$bytes = [long]0; $count = 0
foreach ($entry in $entries) { $bytes += $entry.bytes; $count += $entry.files }
if ($ReportPath -and (Test-Path -LiteralPath $ReportPath)) { throw 'ReportPath must be new; retained reports are not overwritten.' }
if ($Apply) {
    $busy = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(cl|link|ninja|cmake|ffmpeg|mpv_playback_test|playback-ui-tests)\.exe$' -and $_.CommandLine -and $_.CommandLine.IndexOf($scratchRoot,[StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($busy.Count) { throw 'A process is using repository scratch outputs; finish it before cleanup.' }
    $nativeLock = $null
    try {
        if (Test-Path -LiteralPath $native) {
            try { $nativeLock = [IO.File]::Open((Join-Path $native '.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
            catch { throw 'Native dependency build is active; finish it before cleanup.' }
        }
        foreach ($entry in $entries) {
            $target = Assert-WorkspacePath $entry.path
            if ($entry.directory) { Remove-Item -LiteralPath $target -Recurse -Force }
            else { Remove-Item -LiteralPath $target -Force }
        }
    } finally {
        if ($null -ne $nativeLock) { $nativeLock.Dispose() }
    }
}
$report = [ordered]@{recordedUtc=[DateTime]::UtcNow.ToString('o');applied=[bool]$Apply;bytes=$bytes;files=$count;targets=$entries}
if ($ReportPath) { $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8 }
Write-Output ("{0}: {1} generated files, {2:N2} GiB across {3} targets." -f $(if ($Apply) {'Removed'} else {'Planned'}),$count,($bytes/1GB),$entries.Count)
