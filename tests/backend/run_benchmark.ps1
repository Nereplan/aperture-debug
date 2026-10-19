[CmdletBinding()]
param([string]$SourceDirectory = '', [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { Join-Path $repo 'src' }
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempRoot ('Aperture-backend-bench-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run,$OutputDirectory -Force | Out-Null
try {
    . (Join-Path $PSScriptRoot '../support/native_build.ps1')
    $exe = Join-Path $run 'benchmark.exe'
    Build-ApertureTestHarness -Sources @((Join-Path $source 'Json.cpp'), (Join-Path $PSScriptRoot 'benchmark.cpp')) -SourceDirectory $source -Output $exe -Libraries @('ole32.lib')
    Set-Content -LiteralPath (Join-Path $run '.backend-test-root') -Value 'disposable'
    $rows = & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Backend benchmark failed.' }
    $rows | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.csv') -Encoding UTF8
    $rows | Write-Output
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-backend-bench-[a-f0-9]{32}$') { throw 'Unsafe backend benchmark cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
