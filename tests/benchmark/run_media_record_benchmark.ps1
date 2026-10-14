[CmdletBinding()]
param(
    [string]$SourceDirectory = '',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$KeepBuild
)
$ErrorActionPreference = 'Stop'
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $PSScriptRoot '../../src' }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
. (Join-Path $PSScriptRoot '../support/native_build.ps1')
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$build = Join-Path $tempBase ('Aperture-record-benchmark-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
try {
    $exe = Join-Path $build 'media-record-benchmark.exe'
    Build-ApertureTestHarness -SourceDirectory $source -Output $exe -Sources @(
        (Join-Path $source 'Json.cpp'), (Join-Path $PSScriptRoot 'media_record_benchmark.cpp')
    ) -Libraries @('user32.lib','gdi32.lib','ole32.lib','advapi32.lib','shell32.lib')
    $rows = & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Media record benchmark failed.' }
    $rows | Set-Content -LiteralPath (Join-Path $output 'results.csv') -Encoding utf8
    $rows | Write-Output
    if ($KeepBuild) { Copy-Item -LiteralPath $exe -Destination (Join-Path $output 'media-record-benchmark.exe') }
} finally {
    $resolved = [IO.Path]::GetFullPath($build)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-record-benchmark-[a-f0-9]{32}$') {
        throw "Unsafe benchmark cleanup: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
