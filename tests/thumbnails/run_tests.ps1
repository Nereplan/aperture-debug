[CmdletBinding()]
param([string]$Corpus = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempBase ('Aperture-thumbnails-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $exe = Join-Path $run 'thumbnails.exe'
    $response = Join-Path $run 'compile.rsp'
    @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
      "/I`"$root/src`"", "/Fo`"$run\\`"", "/Fe:`"$exe`"", "`"$PSScriptRoot/main.cpp`"") |
        Set-Content -LiteralPath $response -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
    if ($LASTEXITCODE -ne 0) { throw 'Thumbnail harness compilation failed' }
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
