[CmdletBinding()]
param([string]$Corpus = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempBase ('Aperture-thumbnails-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
try {
    . (Join-Path $PSScriptRoot '../support/native_build.ps1')
    $exe = Join-Path $run 'thumbnails.exe'
    $sources = @('MpvPlayer','Json','Settings','Localization','AudioConfig','ChannelPolicy') |
        ForEach-Object { Join-Path $root "src/$_.cpp" }
    Build-ApertureTestHarness -Sources @($sources + (Join-Path $PSScriptRoot 'main.cpp') +
        (Join-Path $PSScriptRoot 'purge_tests.cpp')) -Output $exe -Libraries @('oleaut32.lib','propsys.lib')
    & $exe --test (Join-Path $run 'fixtures')
    if ($LASTEXITCODE -ne 0) { throw 'Thumbnail behavioral tests failed' }
    if ($Corpus) {
        $source = (Resolve-Path -LiteralPath $Corpus).Path
        if (-not $OutputRoot) { throw 'Corpus benchmarking requires an explicit OutputRoot' }
        $output = [IO.Path]::GetFullPath($OutputRoot)
        if (-not $output.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase) -or
            $source.StartsWith($output + '\',[StringComparison]::OrdinalIgnoreCase) -or $source -ieq $output) {
            throw 'Output must be inside the workspace and separate from the input'
        }
        & $exe --corpus $source $output
        if ($LASTEXITCODE -ne 0) { throw 'Real thumbnail corpus checks failed' }
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-thumbnails-*') { throw 'Unsafe temporary cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
