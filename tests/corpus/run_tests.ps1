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

. (Join-Path $scriptDir '../support/native_build.ps1')

# Create unique external isolated temporary build directory
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_corpus_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
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
    (Join-Path $rootDir "src\MediaProbeIsolation.cpp"),
    (Join-Path $rootDir "src\FolderWatcher.cpp"),
    (Join-Path $rootDir "src\LibraryQuery.cpp"),
    (Join-Path $rootDir "src\GlobalSearch.cpp"),
    (Join-Path $rootDir "src\AudioConfig.cpp"),
    (Join-Path $rootDir "src\TextInputModel.cpp"),
    (Join-Path $rootDir "src\ChannelPolicy.cpp"),
    (Join-Path $scriptDir "main.cpp")
)

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib uiautomationcore.lib advapi32.lib propsys.lib dxguid.lib imm32.lib runtimeobject.lib winhttp.lib bcrypt.lib crypt32.lib wintrust.lib"

$exitCode = 1
try {
    Write-Host "Building Aperture Parser & Media Corpus Tests..." -ForegroundColor Cyan
    Build-ApertureTestHarness -Sources $srcFiles -Output $exeOutput -Libraries ($libs -split ' ')

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
        $resolved = [IO.Path]::GetFullPath($tempDir)
        $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
        if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^Aperture_corpus_tests_[a-f0-9]{32}$') { throw 'Unsafe test build cleanup.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

exit $exitCode
