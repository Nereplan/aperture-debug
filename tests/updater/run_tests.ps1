$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

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
if (-not $vsPath -or -not (Test-Path $vsPath)) { throw "Visual Studio 2022 installation not found" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

$versionJsonPath = Join-Path $rootDir "version.json"
if (-not (Test-Path -LiteralPath $versionJsonPath -PathType Leaf)) {
    throw "authoritative version.json not found at $versionJsonPath"
}

$verRaw = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($null -eq $verRaw.major -or $null -eq $verRaw.minor -or $null -eq $verRaw.patch -or $null -eq $verRaw.build) {
    throw "version.json missing required numeric four-part version fields"
}

[int]$vMajor = [int]$verRaw.major
[int]$vMinor = [int]$verRaw.minor
[int]$vPatch = [int]$verRaw.patch
[int]$vBuild = [int]$verRaw.build
if ($vMajor -lt 0 -or $vMinor -lt 0 -or $vPatch -lt 0 -or $vBuild -lt 0) {
    throw "version.json numeric version fields must be non-negative integers"
}

$vString = if ($verRaw.versionString) { $verRaw.versionString } else { "$vMajor.$vMinor.$vPatch.$vBuild" }
$vDisplay = if ($verRaw.displayVersion) { $verRaw.displayVersion } else { "$vMajor.$vMinor.$vPatch" }
$vChannel = if ($verRaw.channel) { $verRaw.channel } else { "stable" }

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_updater_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

$genDir = Join-Path $tempDir "generated"
New-Item -ItemType Directory -Path $genDir -Force | Out-Null

$genVersionHeader = Join-Path $genDir "Version.h"
$versionHeaderContent = @"
#pragma once
#include <cstdint>
#include <string_view>

#define APERTURE_VERSION_MAJOR $vMajor
#define APERTURE_VERSION_MINOR $vMinor
#define APERTURE_VERSION_PATCH $vPatch
#define APERTURE_VERSION_BUILD $vBuild
#define APERTURE_VERSION_STRING "$vString"
#define APERTURE_DISPLAY_VERSION "$vDisplay"
#define APERTURE_BUILD_CHANNEL "$vChannel"

namespace Aperture::Version {
    inline constexpr uint32_t Major = APERTURE_VERSION_MAJOR;
    inline constexpr uint32_t Minor = APERTURE_VERSION_MINOR;
    inline constexpr uint32_t Patch = APERTURE_VERSION_PATCH;
    inline constexpr uint32_t Build = APERTURE_VERSION_BUILD;
    inline constexpr std::string_view String = APERTURE_VERSION_STRING;
    inline constexpr std::string_view DisplayVersion = APERTURE_DISPLAY_VERSION;
    inline constexpr std::string_view BuildChannel = APERTURE_BUILD_CHANNEL;
}
"@
[IO.File]::WriteAllText($genVersionHeader, $versionHeaderContent, [System.Text.Encoding]::UTF8)

$exe = Join-Path $tempDir "Aperture_Updater_Tests.exe"
$srcFiles = @(
    "`"$rootDir\src\ChannelPolicy.cpp`"",
    "`"$rootDir\src\Updater.cpp`"",
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\HttpTransport.cpp`"",
    "`"$scriptDir\main.cpp`""
) -join " "

$libs = "shell32.lib shlwapi.lib ole32.lib advapi32.lib bcrypt.lib crypt32.lib wintrust.lib winhttp.lib user32.lib"
$compile = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$genDir`" /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" $srcFiles $libs /Fo`"$tempDir\\`" /Fe:`"$exe`" /link /SUBSYSTEM:CONSOLE"

try {
    cmd.exe /c $compile
    if ($LASTEXITCODE -ne 0) { throw "Updater test compilation failed with code $LASTEXITCODE" }

    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Updater test execution failed with code $LASTEXITCODE" }

    Write-Host "`nAll Aperture updater deterministic tests completed successfully." -ForegroundColor Green
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
