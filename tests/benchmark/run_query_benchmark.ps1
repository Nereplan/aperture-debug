[CmdletBinding()]
param(
    [string]$SourceDirectory = "",
    [string]$OutputDirectory = "",
    [switch]$KeepBuild,
    [switch]$ProgressOnly
)
$ErrorActionPreference = "Stop"
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $PSScriptRoot "..\..\src" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot "..\..\scratch\query-benchmark" }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC x64 toolchain not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$build = Join-Path $tempBase ("Aperture-query-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build "query-benchmark.exe"
$response = Join-Path $build "compile.rsp"
try {
$argsList = @('/nologo', '/utf-8', '/O2', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
    "/I`"$source`"", "/Fo`"$build\\`"", "/Fe:`"$exe`"",
    "`"$(Join-Path $PSScriptRoot 'query_resource_benchmark.cpp')`"")
foreach ($file in @('Json', 'Settings', 'ChannelPolicy', 'Localization', 'AudioConfig', 'MediaLibrary', 'LibraryQuery', 'GlobalSearch', 'TextInputModel', 'FolderWatcher', 'MediaFoundation')) {
    $argsList += "`"$(Join-Path $source ($file + '.cpp'))`""
}
$argsList += @('mfreadwrite.lib', 'oleaut32.lib', 'propsys.lib')
Set-Content -LiteralPath $response -Value $argsList -Encoding UTF8
& cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
if ($LASTEXITCODE -ne 0) { throw "Query benchmark compilation failed." }
$rows = if ($ProgressOnly) { & $exe --progress } else { & $exe }
if ($LASTEXITCODE -ne 0) { throw "Query benchmark failed." }
$rows | Set-Content -LiteralPath (Join-Path $output 'results.csv') -Encoding UTF8
$rows | Write-Output
if ($KeepBuild) { Copy-Item -LiteralPath $exe -Destination (Join-Path $output 'query-benchmark.exe') -Force }
} finally {
    $resolved = [IO.Path]::GetFullPath($build)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-query-*') { throw "Unsafe build cleanup: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
