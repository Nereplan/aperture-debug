param(
    [string]$LibmpvPath = ""
)

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
if (-not $vsPath -or -not (Test-Path $vsPath)) {
    throw "Visual Studio 2022 installation not found!"
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

# Parse authoritative version.json
$versionJsonPath = Join-Path $rootDir "version.json"
if (-not (Test-Path -LiteralPath $versionJsonPath)) {
    throw "Authoritative version.json not found at $versionJsonPath"
}
$verRaw = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
[int]$vMajor = [int]$verRaw.major
[int]$vMinor = [int]$verRaw.minor
[int]$vPatch = [int]$verRaw.patch
[int]$vBuild = [int]$verRaw.build
$vString = if ($verRaw.versionString) { $verRaw.versionString } else { "$vMajor.$vMinor.$vPatch.$vBuild" }
$vDisplay = if ($verRaw.displayVersion) { $verRaw.displayVersion } else { "$vMajor.$vMinor.$vPatch" }
$vCompany = if ($verRaw.companyName) { $verRaw.companyName } else { "Aperture" }
$vDesc = if ($verRaw.fileDescription) { $verRaw.fileDescription } else { "Aperture Media Player" }
$vLegal = if ($verRaw.legalCopyright) { $verRaw.legalCopyright } else { "Copyright (C) 2026 Aperture" }
$vProduct = if ($verRaw.productName) { $verRaw.productName } else { "Aperture" }
$vInternal = if ($verRaw.internalName) { $verRaw.internalName } else { "Aperture" }
$vOriginal = if ($verRaw.originalFilename) { $verRaw.originalFilename } else { "Aperture.exe" }
$vChannel = if ($verRaw.channel) { $verRaw.channel } else { "stable" }

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_loader_tests_" + [System.Guid]::NewGuid().ToString("N"))
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
#define APERTURE_VERSION_CSV $vMajor,$vMinor,$vPatch,$vBuild
#define APERTURE_VERSION_STRING "$vString"
#define APERTURE_VERSION_WIDE L"$vString"
#define APERTURE_DISPLAY_VERSION "$vDisplay"
#define APERTURE_DISPLAY_VERSION_WIDE L"$vDisplay"
#define APERTURE_COMPANY_NAME "$vCompany"
#define APERTURE_COMPANY_NAME_WIDE L"$vCompany"
#define APERTURE_FILE_DESCRIPTION "$vDesc"
#define APERTURE_FILE_DESCRIPTION_WIDE L"$vDesc"
#define APERTURE_FILE_VERSION "$vString"
#define APERTURE_FILE_VERSION_WIDE L"$vString"
#define APERTURE_INTERNAL_NAME "$vInternal"
#define APERTURE_INTERNAL_NAME_WIDE L"$vInternal"
#define APERTURE_LEGAL_COPYRIGHT "$vLegal"
#define APERTURE_LEGAL_COPYRIGHT_WIDE L"$vLegal"
#define APERTURE_ORIGINAL_FILENAME "$vOriginal"
#define APERTURE_ORIGINAL_FILENAME_WIDE L"$vOriginal"
#define APERTURE_PRODUCT_NAME "$vProduct"
#define APERTURE_PRODUCT_NAME_WIDE L"$vProduct"
#define APERTURE_PRODUCT_VERSION "$vString"
#define APERTURE_PRODUCT_VERSION_WIDE L"$vString"
#define APERTURE_COMMENTS "Aperture Media Player"
#define APERTURE_COMMENTS_WIDE L"Aperture Media Player"
#define APERTURE_LANG_ID 1033
#define APERTURE_CODEPAGE_ID 1200
#define APERTURE_TRANSLATION_STRING "040904B0"
#define APERTURE_BUILD_COMMIT "unavailable"
#define APERTURE_BUILD_COMMIT_WIDE L"unavailable"
#define APERTURE_BUILD_REPOSITORY "unavailable"
#define APERTURE_BUILD_REPOSITORY_WIDE L"unavailable"
#define APERTURE_BUILD_CHANNEL "$vChannel"
#define APERTURE_BUILD_CHANNEL_WIDE L"$vChannel"
#define APERTURE_TARGET_ARCH "x64"
#define APERTURE_TARGET_ARCH_WIDE L"x64"
#define APERTURE_COMPILER_NAME "MSVC"
#define APERTURE_COMPILER_NAME_WIDE L"MSVC"
#define APERTURE_COMPILER_VERSION "19.40"
#define APERTURE_COMPILER_VERSION_WIDE L"19.40"
#define APERTURE_WINDOWS_SDK_VERSION "10.0.19041.0+"
#define APERTURE_WINDOWS_SDK_VERSION_WIDE L"10.0.19041.0+"
#define APERTURE_LIBMPV_VERSION "0.38.0"
#define APERTURE_LIBMPV_CLIENT_API "2.2"
#define APERTURE_LIBMPV_ABI "2"
#define APERTURE_LIBMPV_SHA256 "unavailable"
#define APERTURE_LIBMPV_LICENSE "LGPL-2.1-or-later"
#define APERTURE_FONT_INTER_VERSION "4.0"
#define APERTURE_FONT_INTER_SHA256 "unavailable"
#define APERTURE_FONT_SPACE_GROTESK_VERSION "2.0"
#define APERTURE_FONT_SPACE_GROTESK_SHA256 "unavailable"
#define APERTURE_FONT_JETBRAINS_MONO_VERSION "2.304"
#define APERTURE_FONT_JETBRAINS_MONO_SHA256 "unavailable"

namespace Aperture::Version {
    inline constexpr uint32_t Major = APERTURE_VERSION_MAJOR;
    inline constexpr uint32_t Minor = APERTURE_VERSION_MINOR;
    inline constexpr uint32_t Patch = APERTURE_VERSION_PATCH;
    inline constexpr uint32_t Build = APERTURE_VERSION_BUILD;

    inline constexpr std::string_view String = APERTURE_VERSION_STRING;
    inline constexpr std::wstring_view WideString = APERTURE_VERSION_WIDE;
    inline constexpr std::string_view DisplayVersion = APERTURE_DISPLAY_VERSION;
    inline constexpr std::wstring_view DisplayVersionWide = APERTURE_DISPLAY_VERSION_WIDE;
    inline constexpr std::string_view CompanyName = APERTURE_COMPANY_NAME;
    inline constexpr std::wstring_view CompanyNameWide = APERTURE_COMPANY_NAME_WIDE;
    inline constexpr std::string_view FileDescription = APERTURE_FILE_DESCRIPTION;
    inline constexpr std::wstring_view FileDescriptionWide = APERTURE_FILE_DESCRIPTION_WIDE;
    inline constexpr std::string_view FileVersion = APERTURE_FILE_VERSION;
    inline constexpr std::wstring_view FileVersionWide = APERTURE_FILE_VERSION_WIDE;
    inline constexpr std::string_view InternalName = APERTURE_INTERNAL_NAME;
    inline constexpr std::wstring_view InternalNameWide = APERTURE_INTERNAL_NAME_WIDE;
    inline constexpr std::string_view LegalCopyright = APERTURE_LEGAL_COPYRIGHT;
    inline constexpr std::wstring_view LegalCopyrightWide = APERTURE_LEGAL_COPYRIGHT_WIDE;
    inline constexpr std::string_view OriginalFilename = APERTURE_ORIGINAL_FILENAME;
    inline constexpr std::wstring_view OriginalFilenameWide = APERTURE_ORIGINAL_FILENAME_WIDE;
    inline constexpr std::string_view ProductName = APERTURE_PRODUCT_NAME;
    inline constexpr std::wstring_view ProductNameWide = APERTURE_PRODUCT_NAME_WIDE;
    inline constexpr std::string_view ProductVersion = APERTURE_PRODUCT_VERSION;
    inline constexpr std::wstring_view ProductVersionWide = APERTURE_PRODUCT_VERSION_WIDE;
    inline constexpr std::string_view Comments = APERTURE_COMMENTS;
    inline constexpr std::wstring_view CommentsWide = APERTURE_COMMENTS_WIDE;

    inline constexpr uint16_t LanguageId = APERTURE_LANG_ID;
    inline constexpr uint16_t CodePageId = APERTURE_CODEPAGE_ID;

    inline constexpr std::string_view BuildCommit = APERTURE_BUILD_COMMIT;
    inline constexpr std::wstring_view BuildCommitWide = APERTURE_BUILD_COMMIT_WIDE;
    inline constexpr std::string_view BuildRepository = APERTURE_BUILD_REPOSITORY;
    inline constexpr std::wstring_view BuildRepositoryWide = APERTURE_BUILD_REPOSITORY_WIDE;
    inline constexpr std::string_view BuildChannel = APERTURE_BUILD_CHANNEL;
    inline constexpr std::wstring_view BuildChannelWide = APERTURE_BUILD_CHANNEL_WIDE;
    inline constexpr std::string_view TargetArch = APERTURE_TARGET_ARCH;
    inline constexpr std::wstring_view TargetArchWide = APERTURE_TARGET_ARCH_WIDE;
    inline constexpr std::string_view CompilerName = APERTURE_COMPILER_NAME;
    inline constexpr std::wstring_view CompilerNameWide = APERTURE_COMPILER_NAME_WIDE;
    inline constexpr std::string_view CompilerVersion = APERTURE_COMPILER_VERSION;
    inline constexpr std::wstring_view CompilerVersionWide = APERTURE_COMPILER_VERSION_WIDE;
    inline constexpr std::string_view WindowsSdkVersion = APERTURE_WINDOWS_SDK_VERSION;
    inline constexpr std::wstring_view WindowsSdkVersionWide = APERTURE_WINDOWS_SDK_VERSION_WIDE;

    inline constexpr std::string_view LibmpvVersion = APERTURE_LIBMPV_VERSION;
    inline constexpr std::string_view LibmpvClientApi = APERTURE_LIBMPV_CLIENT_API;
    inline constexpr std::string_view LibmpvAbi = APERTURE_LIBMPV_ABI;
    inline constexpr std::string_view LibmpvSha256 = APERTURE_LIBMPV_SHA256;
    inline constexpr std::string_view LibmpvLicense = APERTURE_LIBMPV_LICENSE;

    inline constexpr std::string_view FontInterVersion = APERTURE_FONT_INTER_VERSION;
    inline constexpr std::string_view FontInterSha256 = APERTURE_FONT_INTER_SHA256;
    inline constexpr std::string_view FontSpaceGroteskVersion = APERTURE_FONT_SPACE_GROTESK_VERSION;
    inline constexpr std::string_view FontSpaceGroteskSha256 = APERTURE_FONT_SPACE_GROTESK_SHA256;
    inline constexpr std::string_view FontJetbrainsMonoVersion = APERTURE_FONT_JETBRAINS_MONO_VERSION;
    inline constexpr std::string_view FontJetbrainsMonoSha256 = APERTURE_FONT_JETBRAINS_MONO_SHA256;
}
"@
[IO.File]::WriteAllText($genVersionHeader, $versionHeaderContent, [System.Text.Encoding]::UTF8)

$outExe = Join-Path $tempDir "Aperture_Loader_Tests.exe"
$mpvSource = if ([string]::IsNullOrWhiteSpace($LibmpvPath)) {
    Join-Path $rootDir "libmpv-2.dll"
} else {
    [IO.Path]::GetFullPath($LibmpvPath)
}
if (Test-Path -LiteralPath $mpvSource -PathType Leaf) {
    Copy-Item -LiteralPath $mpvSource -Destination (Join-Path $tempDir "libmpv-2.dll") -Force
} else {
    throw "libmpv-2.dll not found at: $mpvSource"
}

Write-Host "Building Aperture DLL Loader Tests..." -ForegroundColor Cyan

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\Settings.cpp`"",
    "`"$rootDir\src\Localization.cpp`"",
    "`"$rootDir\src\AudioConfig.cpp`"",
    "`"$rootDir\src\ChannelPolicy.cpp`"",
    "`"$rootDir\src\BuildInfo.cpp`"",
    "`"$rootDir\src\Diagnostics.cpp`"",
    "`"$rootDir\src\MpvPlayer.cpp`"",
    "`"$scriptDir\main.cpp`""
)

$libs = "advapi32.lib user32.lib gdi32.lib shell32.lib ole32.lib shlwapi.lib d2d1.lib d3d11.lib dxgi.lib windowscodecs.lib dwrite.lib dbghelp.lib"
$sourcesJoined = $srcFiles -join " "

$exitCode = 1
try {
    $clCmd = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$genDir`" /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" /Fo`"$tempDir\\`" $sourcesJoined $libs /Fe:`"$outExe`" /link /SUBSYSTEM:CONSOLE"
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed with code $LASTEXITCODE" }

    & $outExe $args
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw "Tests failed with code $exitCode" }
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit $exitCode
