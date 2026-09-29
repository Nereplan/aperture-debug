[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempBase ('Aperture-lookup-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $source = Join-Path $PSScriptRoot 'lookup_benchmark.cpp'
    $exe = Join-Path $run 'lookup_benchmark.exe'
    $response = Join-Path $run 'compile.rsp'
    @('/nologo','/utf-8','/O2','/EHsc','/std:c++17',
      "/Fo`"$run\\`"", "/Fe:`"$exe`"", "`"$source`"") |
        Set-Content -LiteralPath $response -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
    if ($LASTEXITCODE -ne 0) { throw 'Lookup benchmark compilation failed' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Lookup benchmark failed' }
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-lookup-*') { throw 'Unsafe temporary cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
