<#
.SYNOPSIS
    Verifies Aperture's strict runtime-only directory layout.

.DESCRIPTION
    A valid runtime directory contains only Aperture.exe, the package-local
    libmpv runtime, required private fonts, and supported locale JSON files.
    Development files, symbols, manifests, source, tests, and writable runtime
    state are rejected.
    -IncludeDebug additionally requires Aperture.Debug.exe and Aperture.pdb;
    optional source paths verify their hashes against the build outputs.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory,

    [string]$SourceRoot = (Join-Path $PSScriptRoot ".."),

    [string]$BinarySource = "",
    [switch]$IncludeDebug,
    [string]$DebugBinarySource = "",
    [string]$DebugSymbolsSource = ""
)

$ErrorActionPreference = "Stop"
$RuntimeDirectory = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\', '/')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\', '/')

if (-not (Test-Path -LiteralPath $RuntimeDirectory -PathType Container)) {
    throw "Runtime directory does not exist: $RuntimeDirectory"
}
if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "Source root does not exist: $SourceRoot"
}

$dependencyManifest = Join-Path $SourceRoot "scripts\dependencies.json"
if (-not (Test-Path -LiteralPath $dependencyManifest -PathType Leaf)) {
    throw "Dependency manifest is missing: $dependencyManifest"
}

$manifest = Get-Content -LiteralPath $dependencyManifest -Raw -Encoding UTF8 | ConvertFrom-Json
$runtimeAssets = [System.Collections.Generic.List[string]]::new()
$runtimeAssets.Add("Aperture.exe")
if ($IncludeDebug) {
    $runtimeAssets.Add("Aperture.Debug.exe")
    $runtimeAssets.Add("Aperture.pdb")
} elseif ($DebugBinarySource -or $DebugSymbolsSource) {
    throw "Debug sources require -IncludeDebug."
}

foreach ($entry in @($manifest.vendoredFiles)) {
    $normalized = ($entry.path -replace '/', '\')
    if ($entry.category -eq "required" -and ($normalized -ieq "libmpv-2.dll" -or $normalized.StartsWith("fonts\", [StringComparison]::OrdinalIgnoreCase))) {
        $runtimeAssets.Add($normalized)
    }
}

$localeSource = Join-Path $SourceRoot "locales"
$localeFiles = @()
if (Test-Path -LiteralPath $localeSource -PathType Container) {
    $localeFiles = @(Get-ChildItem -LiteralPath $localeSource -File -Filter "*.json" | Sort-Object Name)
}
if ($localeFiles.Count -eq 0) {
    throw "No locale JSON files were found under $localeSource"
}
foreach ($locale in $localeFiles) {
    $runtimeAssets.Add(("locales\" + $locale.Name))
}

$expected = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($asset in $runtimeAssets) {
    [void]$expected.Add($asset)
}

$actual = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in @(Get-ChildItem -LiteralPath $RuntimeDirectory -Recurse -File -Force)) {
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Runtime directory contains a reparse-point file: $($file.FullName)"
    }
    $relative = $file.FullName.Substring($RuntimeDirectory.Length).TrimStart('\', '/')
    [void]$actual.Add($relative)
}

$missing = @($expected | Where-Object { -not $actual.Contains($_) } | Sort-Object)
$extra = @($actual | Where-Object { -not $expected.Contains($_) } | Sort-Object)
if ($missing.Count -gt 0 -or $extra.Count -gt 0) {
    $parts = @()
    if ($missing.Count -gt 0) { $parts += "missing: $($missing -join ', ')" }
    if ($extra.Count -gt 0) { $parts += "unexpected: $($extra -join ', ')" }
    throw "Runtime layout is not clean ($($parts -join '; '))."
}

$allowedDirectories = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
[void]$allowedDirectories.Add("fonts")
[void]$allowedDirectories.Add("locales")
foreach ($dir in @(Get-ChildItem -LiteralPath $RuntimeDirectory -Recurse -Directory -Force)) {
    if (($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Runtime directory contains a reparse-point directory: $($dir.FullName)"
    }
    $relative = $dir.FullName.Substring($RuntimeDirectory.Length).TrimStart('\', '/')
    if (-not $allowedDirectories.Contains($relative)) {
        throw "Runtime directory contains an unexpected directory: $relative"
    }
}

foreach ($asset in $runtimeAssets) {
    if ($asset -in @("Aperture.exe", "Aperture.Debug.exe", "Aperture.pdb")) { continue }
    $source = Join-Path $SourceRoot $asset
    $published = Join-Path $RuntimeDirectory $asset
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Runtime source asset is missing: $source"
    }
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $publishedHash = (Get-FileHash -LiteralPath $published -Algorithm SHA256).Hash
    if ($sourceHash -ne $publishedHash) {
        throw "Runtime asset differs from its source: $asset"
    }
}

if (-not [string]::IsNullOrWhiteSpace($BinarySource)) {
    $BinarySource = [IO.Path]::GetFullPath($BinarySource)
    if (-not (Test-Path -LiteralPath $BinarySource -PathType Leaf)) {
        throw "Binary source does not exist: $BinarySource"
    }
    $sourceHash = (Get-FileHash -LiteralPath $BinarySource -Algorithm SHA256).Hash
    $publishedHash = (Get-FileHash -LiteralPath (Join-Path $RuntimeDirectory "Aperture.exe") -Algorithm SHA256).Hash
    if ($sourceHash -ne $publishedHash) {
        throw "Published Aperture.exe differs from the built Release binary."
    }
}

foreach ($debugArtifact in @(
    @{ Name = "Aperture.Debug.exe"; Source = $DebugBinarySource },
    @{ Name = "Aperture.pdb"; Source = $DebugSymbolsSource }
)) {
    if ([string]::IsNullOrWhiteSpace($debugArtifact.Source)) { continue }
    $sourceHash = (Get-FileHash -LiteralPath $debugArtifact.Source -Algorithm SHA256).Hash
    $publishedHash = (Get-FileHash -LiteralPath (Join-Path $RuntimeDirectory $debugArtifact.Name) -Algorithm SHA256).Hash
    if ($sourceHash -ne $publishedHash) { throw "Published $($debugArtifact.Name) differs from its build source." }
}
Write-Host "Runtime layout verified: $RuntimeDirectory" -ForegroundColor Green
Write-Host ("Files: {0}; includes Debug build: {1}" -f $actual.Count, [bool]$IncludeDebug) -ForegroundColor Cyan
