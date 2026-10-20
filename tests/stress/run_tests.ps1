param(
    [int]$Iterations = 0,
    [string]$Seed = "",
    [switch]$Soak,
    [string]$Suite = "",
    [string]$Test = "",
    [switch]$List,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

# Find Visual Studio 2022 installation
$vsPath = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath -or -not (Test-Path $vsPath)) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($cand in $candidates) {
        if (Test-Path $cand) { $vsPath = $cand; break }
    }
}
if (-not $vsPath -or -not (Test-Path $vsPath)) {
    throw "Visual Studio 2022 installation not found"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

# Parse version.json
$versionJsonPath = Join-Path $rootDir "version.json"
if (-not (Test-Path -LiteralPath $versionJsonPath)) {
    throw "Authoritative version.json not found at $versionJsonPath"
}
$verRaw = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
[int]$vMajor = [int]$verRaw.major
[int]$vMinor = [int]$verRaw.minor
[int]$vPatch = [int]$verRaw.patch
[int]$vBuild = [int]$verRaw.build
# Isolated external temp directory
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_stress_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

$genDir = Join-Path $tempDir "generated"
New-Item -ItemType Directory -Path $genDir -Force | Out-Null

$genVersionHeader = Join-Path $genDir "Version.h"
$versionHeaderContent = @"
#pragma once
#include <cstdint>
namespace Aperture::Version {
    inline constexpr uint32_t Major = $vMajor;
    inline constexpr uint32_t Minor = $vMinor;
    inline constexpr uint32_t Patch = $vPatch;
    inline constexpr uint32_t Build = $vBuild;
}
"@
[IO.File]::WriteAllText($genVersionHeader, $versionHeaderContent, [System.Text.Encoding]::UTF8)

$exe = Join-Path $tempDir "Aperture_Stress_Tests.exe"

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\Settings.cpp`"",
    "`"$rootDir\src\Localization.cpp`"",
    "`"$rootDir\src\MediaLibrary.cpp`"",
    "`"$rootDir\src\MediaFoundation.cpp`"",
    "`"$rootDir\src\MediaProbeIsolation.cpp`"",
    "`"$rootDir\src\FolderWatcher.cpp`"",
    "`"$rootDir\src\MpvPlayer.cpp`"",
    "`"$rootDir\src\PlaybackSession.cpp`"",
    "`"$rootDir\src\CommandDispatcher.cpp`"",
    "`"$rootDir\src\MediaTransport.cpp`"",
    "`"$rootDir\src\TextInputModel.cpp`"",
    "`"$rootDir\src\GlobalSearch.cpp`"",
    "`"$rootDir\src\AudioConfig.cpp`"",
    "`"$rootDir\src\SubtitleDiscovery.cpp`"",
    "`"$rootDir\src\Subtitles.cpp`"",
    "`"$rootDir\src\OnlineSubtitles.cpp`"",
    "`"$rootDir\src\LibraryQuery.cpp`"",
    "`"$rootDir\src\PlaylistIO.cpp`"",
    "`"$rootDir\src\TaskbarController.cpp`"",
    "`"$rootDir\src\Accessibility.cpp`"",
    "`"$rootDir\src\Diagnostics.cpp`"",
    "`"$rootDir\src\Updater.cpp`"",
    "`"$rootDir\src\ChannelPolicy.cpp`"",
    "`"$rootDir\src\HttpTransport.cpp`"",
    "`"$scriptDir\main.cpp`""
) -join " "

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib uiautomationcore.lib advapi32.lib propsys.lib dxguid.lib imm32.lib runtimeobject.lib winhttp.lib dbghelp.lib bcrypt.lib crypt32.lib wintrust.lib"
$compile = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$genDir`" /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" $srcFiles $libs /Fo`"$tempDir\\`" /Fe:`"$exe`" /link /SUBSYSTEM:CONSOLE"

try {
    Write-Host "Building Aperture Stress Tests..." -ForegroundColor Cyan
    cmd.exe /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with code $LASTEXITCODE"
    }

    $argsList = @()
    if ($Iterations -gt 0) { $argsList += "--iterations", $Iterations }
    if ($Seed) { $argsList += "--seed", $Seed }
    if ($Soak) { $argsList += "--soak" }
    if ($Suite) { $argsList += "--suite", $Suite }
    if ($Test) { $argsList += "--test", $Test }
    if ($List) { $argsList += "--list" }
    if ($Help) { $argsList += "--help" }

    & $exe @argsList
    $exitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit $exitCode
