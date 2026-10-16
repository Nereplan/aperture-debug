[CmdletBinding()]
param([string]$SourceDirectory = '', [string]$Suite = '', [string]$Test = '', [switch]$KeepArtifacts)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempRoot ('Aperture-loading-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$exitCode = 1
try {
    . (Join-Path $PSScriptRoot '../support/native_build.ps1')
    $source = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { Join-Path $repo 'src' }
    $sources = @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery','FolderWatcher','MediaFoundation','MediaProbeIsolation') |
        ForEach-Object { Join-Path $source ($_ + '.cpp') }
    $exe = Join-Path $run 'loading-tests.exe'
    Build-ApertureTestHarness -Sources (@($sources) + (Join-Path $PSScriptRoot 'main.cpp')) -SourceDirectory $source -Output $exe -Libraries @('mfreadwrite.lib','oleaut32.lib','propsys.lib')
    Set-Content -LiteralPath (Join-Path $run '.loading-test-root') -Value 'disposable'
    $arguments = @()
    if ($Suite) { $arguments += '--suite', $Suite }
    if ($Test) { $arguments += '--test', $Test }
    & $exe @arguments
    $exitCode = $LASTEXITCODE
} finally {
    if ($KeepArtifacts) { Write-Host "Loading test installation: $run" }
    else {
        $resolved = [IO.Path]::GetFullPath($run)
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^Aperture-loading-[a-f0-9]{32}$') { throw 'Unsafe loading test cleanup.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
exit $exitCode
