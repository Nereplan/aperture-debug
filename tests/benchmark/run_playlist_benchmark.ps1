[CmdletBinding()]
param([string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'scratch/playlist-benchmark' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempBase ('Aperture-playlist-benchmark-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $exe = Join-Path $run 'playlist-benchmark.exe'
    $response = Join-Path $run 'compile.rsp'
    @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
      "/I`"$root/src`"", "/Fo`"$run\\`"", "/Fe:`"$exe`"",
      "`"$PSScriptRoot/playlist_append_benchmark.cpp`"", "`"$root/src/PlaylistIO.cpp`"") |
        Set-Content -LiteralPath $response -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
    if ($LASTEXITCODE -ne 0) { throw 'Playlist benchmark compilation failed' }
    $rows = & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Playlist benchmark failed' }
    $rows | Set-Content -LiteralPath (Join-Path $OutputDirectory 'results.csv') -Encoding UTF8
    $rows | Write-Output
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-playlist-benchmark-*') { throw "Unsafe build cleanup: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
