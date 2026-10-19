[CmdletBinding()]
param([string]$SourceDirectory = '', [string]$Suite = '', [string]$Test = '', [switch]$KeepArtifacts)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempRoot ('Aperture-backend-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$exitCode = 1
try {
    . (Join-Path $PSScriptRoot '../support/native_build.ps1')
    $source = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { Join-Path $repo 'src' }
    $sources = @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery','FolderWatcher','MediaFoundation','MediaProbeIsolation') |
        ForEach-Object { Join-Path $source ($_ + '.cpp') }
    $exe = Join-Path $run 'backend-tests.exe'
    Build-ApertureTestHarness -Sources (@($sources) + (Join-Path $PSScriptRoot 'main.cpp')) -SourceDirectory $source -Output $exe -Libraries @('mfreadwrite.lib','oleaut32.lib','propsys.lib')
    Set-Content -LiteralPath (Join-Path $run '.backend-test-root') -Value 'disposable'
    $linkTarget = Join-Path $run 'link-target'
    New-Item -ItemType Directory -Path $linkTarget,(Join-Path $run 'root-link'),(Join-Path $run 'class-link/cache'),(Join-Path $run 'nested-link/cache/generic') | Out-Null
    [IO.File]::WriteAllText((Join-Path $linkTarget 'keep.bin'), 'untouched')
    foreach ($link in @('root-link/cache','class-link/cache/shaders','nested-link/cache/generic/linked')) {
        New-Item -ItemType Junction -Path (Join-Path $run $link) -Target $linkTarget | Out-Null
    }
    $arguments = @()
    if ($Suite) { $arguments += '--suite', $Suite }
    if ($Test) { $arguments += '--test', $Test }
    & $exe @arguments
    $exitCode = $LASTEXITCODE
} finally {
    if ($KeepArtifacts) { Write-Host "Backend test installation: $run" }
    else {
        $resolved = [IO.Path]::GetFullPath($run)
        if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^Aperture-backend-[a-f0-9]{32}$') { throw 'Unsafe backend test cleanup.' }
        # Remove junctions themselves before recursive cleanup (also safe on Windows PowerShell 5.1).
        foreach ($link in @('root-link/cache','class-link/cache/shaders','nested-link/cache/generic/linked')) {
            $linkPath = Join-Path $resolved $link
            if (Test-Path -LiteralPath $linkPath) { [IO.Directory]::Delete($linkPath) }
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
exit $exitCode
