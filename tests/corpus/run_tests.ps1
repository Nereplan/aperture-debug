<#
.SYNOPSIS
    Deterministic Parser and Media Corpus Test Runner for Aperture.

.DESCRIPTION
    Builds and executes the native C++17 Aperture corpus test harness against
    authoritative manifest fixtures (tests/corpus/manifest.json), verifying
    observable accept/reject behavior, malformed/truncated boundaries, Unicode,
    and extension/content disagreement for JSON, playlists, subtitles, URLs,
    command-line/IPC envelopes, settings, and media containers.
    Ensures all build artifacts and intermediates are strictly isolated to
    external temporary directories and cleaned in finally.

.PARAMETER Category
    Filter fixtures to run by category (e.g. json, playlist, subtitle, url, command_line, settings, media).

.PARAMETER Id
    Filter to a specific fixture ID.

.PARAMETER ManifestPath
    Path to the corpus manifest JSON (defaults to tests/corpus/manifest.json).

.PARAMETER List
    List all registered corpus fixtures without running tests.

.PARAMETER Validate
    Validate manifest schema, SHA-256 hashes, file sizes, category constraints,
    duplicate IDs, missing files, and orphan files without executing parsers.

.PARAMETER ValidateOnly
    Alias for -Validate.

.PARAMETER Help
    Display help information.
#>

param(
    [string]$Category = "",
    [string]$Id = "",
    [string]$ManifestPath = "",
    [switch]$List,
    [Alias("ValidateOnly")]
    [switch]$Validate,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path
    exit 0
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $scriptDir "manifest.json"
}
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Corpus manifest file not found at: $ManifestPath"
}

# If Validate mode requested and python is available, run validate_corpus.py for fast dry-run validation
$validatorPy = Join-Path $scriptDir "validate_corpus.py"
if (($Validate -or $ValidateOnly) -and (Test-Path -LiteralPath $validatorPy)) {
    $pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
    if (-not $pyCmd) { $pyCmd = Get-Command "python" -ErrorAction SilentlyContinue }
    if ($pyCmd) {
        & $pyCmd.Source $validatorPy $scriptDir
        if ($LASTEXITCODE -eq 0) {
            exit 0
        }
        throw "Corpus manifest validation failed with exit code $LASTEXITCODE"
    }
}

# Locate Visual Studio 2022 toolchain
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
    throw "Visual Studio 2022 installation not found!"
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

# Read authoritative version.json and generate Version.h
$versionJsonPath = Join-Path $rootDir "version.json"
if (Test-Path -LiteralPath $versionJsonPath -PathType Leaf) {
    $verRaw = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $vMajor = [int]$verRaw.major
    $vMinor = [int]$verRaw.minor
    $vPatch = [int]$verRaw.patch
    $vBuild = [int]$verRaw.build
    $vString = if ($verRaw.versionString) { $verRaw.versionString } else { "$vMajor.$vMinor.$vPatch.$vBuild" }
    $vDisplay = if ($verRaw.displayVersion) { $verRaw.displayVersion } else { "$vMajor.$vMinor.$vPatch" }
    $vChannel = if ($verRaw.channel) { $verRaw.channel } else { "stable" }
} else {
    $vMajor = 1; $vMinor = 0; $vPatch = 0; $vBuild = 0; $vString = "1.0.0.0"; $vDisplay = "1.0.0"; $vChannel = "stable"
}

# Create unique external isolated temporary build directory
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_corpus_tests_" + [System.Guid]::NewGuid().ToString("N"))
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

$exeOutput = Join-Path $tempDir "Aperture_Corpus_Tests.exe"

$srcFiles = @(
    (Join-Path $rootDir "src\Json.cpp"),
    (Join-Path $rootDir "src\PlaylistIO.cpp"),
    (Join-Path $rootDir "src\Subtitles.cpp"),
    (Join-Path $rootDir "src\SubtitleDiscovery.cpp"),
    (Join-Path $rootDir "src\NetworkStream.cpp"),
    (Join-Path $rootDir "src\MpvPlayer.cpp"),
    (Join-Path $rootDir "src\Settings.cpp"),
    (Join-Path $rootDir "src\Localization.cpp"),
    (Join-Path $rootDir "src\MediaLibrary.cpp"),
    (Join-Path $rootDir "src\MediaFoundation.cpp"),
    (Join-Path $rootDir "src\FolderWatcher.cpp"),
    (Join-Path $rootDir "src\LibraryQuery.cpp"),
    (Join-Path $rootDir "src\GlobalSearch.cpp"),
    (Join-Path $rootDir "src\AudioConfig.cpp"),
    (Join-Path $rootDir "src\TextInputModel.cpp"),
    (Join-Path $rootDir "src\ChannelPolicy.cpp"),
    (Join-Path $scriptDir "main.cpp")
)

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib uiautomationcore.lib advapi32.lib propsys.lib dxguid.lib imm32.lib runtimeobject.lib winhttp.lib bcrypt.lib crypt32.lib wintrust.lib"

$sourcesJoined = ($srcFiles | ForEach-Object { "`"$_`"" }) -join " "

$clCmd = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$genDir`" /I `"$rootDir\tests\support`" /Fo`"$tempDir\\`" $sourcesJoined $libs /Fe:`"$exeOutput`" /link /SUBSYSTEM:CONSOLE"
$exitCode = 1
try {
    Write-Host "Building Aperture Parser & Media Corpus Tests..." -ForegroundColor Cyan
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Corpus test compilation failed with code $LASTEXITCODE"
    }

    $argsList = @("--manifest", $ManifestPath)
    if ($Category) { $argsList += "--category", $Category }
    if ($Id) { $argsList += "--id", $Id }
    if ($List) { $argsList += "--list" }
    if ($Validate -or $ValidateOnly) { $argsList += "--validate" }

    & $exeOutput $argsList
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Corpus tests failed with exit code $exitCode"
    }
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
