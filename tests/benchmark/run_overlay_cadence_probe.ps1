[CmdletBinding()]
param(
    [string]$OutputDirectory = "",
    [switch]$BuildOnly,
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'scratch\overlay-cadence' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe = Join-Path $output 'overlay-cadence-probe.exe'
if (-not $SkipBuild) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found.' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $response = Join-Path $output 'compile.rsp'
    $compile = @('/nologo', '/utf-8', '/O2', '/EHsc', '/std:c++17', '/DUNICODE', '/D_UNICODE',
        "/I`"$root\src`"", "/I`"$root\tests\support`"", "/Fo`"$output\\`"", "/Fe:`"$exe`"",
        "`"$PSScriptRoot\overlay_cadence_probe.cpp`"", "`"$root\src\OverlayRenderer.cpp`"",
        "`"$root\src\Localization.cpp`"", "`"$root\src\Json.cpp`"", 'd2d1.lib', 'dwrite.lib', 'user32.lib', 'gdi32.lib', 'ole32.lib', 'shell32.lib', 'shlwapi.lib',
        "/link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:`"$root\tests\support\native_tests.manifest`"")
    Set-Content -LiteralPath $response -Value $compile -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
    if ($LASTEXITCODE -ne 0) { throw 'Overlay cadence probe compilation failed.' }
}
& $exe --test
if ($LASTEXITCODE -ne 0) { throw 'Overlay pacing tests failed.' }
if (-not $SkipBuild) {
    $native = & $exe --native-validation
    if ($LASTEXITCODE -ne 0) { throw "Overlay native validation failed (code $LASTEXITCODE)." }
    $native | Set-Content -LiteralPath (Join-Path $output 'native-validation.json') -Encoding UTF8
    $native | Write-Output
}
if (-not $BuildOnly) {
    $rows = & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Overlay cadence probe failed.' }
    $rows | Set-Content -LiteralPath (Join-Path $output 'results.csv') -Encoding UTF8
    $rows | Write-Output
}
