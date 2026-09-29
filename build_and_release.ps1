<#
.SYNOPSIS
    Build, test, package, and install Aperture while retaining each copy's saved data.
#>
[CmdletBinding()]
param([switch]$FailFast, [switch]$IncludeDebug, [string]$Repository = $env:GITHUB_REPOSITORY)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$work = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-release-' + [guid]::NewGuid().ToString('N'))
. (Join-Path $root 'scripts/runtime_update.ps1')
try {
    New-Item -ItemType Directory -Path $work | Out-Null
    $binary = Join-Path $work 'Aperture.exe'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -BuildOutputPath $binary
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed. Installed copies were not changed.' }
    $testArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $root 'tests/run_tests.ps1'),'-Tier','pr')
    if ($FailFast) { $testArgs += '-FailFast' }
    & powershell.exe @testArgs
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed. Installed copies were not changed.' }
    $package = Join-Path $work 'package'
    & (Join-Path $root 'scripts/package_release.ps1') -BinaryPath $binary -OutputDirectory $package -Repository $Repository
    [void](Read-AperturePackage $package)
    $version = (Get-Content -LiteralPath (Join-Path $root 'version.json') -Raw | ConvertFrom-Json).versionString
    $dist = Join-Path $root 'dist'
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
    $zip = Join-Path $dist "Aperture-$version-windows-x64.zip"
    Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip -Force
    Install-AperturePackage -Package $package -Destination (Join-Path $root 'release') -RestartIfRunning
    if ($IncludeDebug) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -Debug -BuildOutputPath (Join-Path $package 'Aperture.exe') -PdbOutputPath (Join-Path $package 'Aperture.pdb')
        if ($LASTEXITCODE -ne 0) { throw 'Debug build failed. The release is ready; debug was not changed.' }
        $manifestPath = Join-Path $package 'manifest.json'
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $manifest.configuration = 'Debug'
        $manifest.binarySha256 = (Get-FileHash -LiteralPath (Join-Path $package 'Aperture.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest.files = @(Get-ChildItem -LiteralPath $package -File -Recurse | Where-Object Name -ne 'manifest.json' | ForEach-Object {
            @{ path = $_.FullName.Substring($package.Length + 1).Replace('\','/'); size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    }
    Install-AperturePackage -Package $package -Destination (Join-Path $root 'debug') -RestartIfRunning
    Write-Host "Ready: $zip" -ForegroundColor Green
} finally {
    $full = [IO.Path]::GetFullPath($work)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-release-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
