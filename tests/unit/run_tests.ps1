param(
    [string]$Suite = "",
    [string]$Test = "",
    [string]$LibmpvPath = "",
    [switch]$AllowVisibleWindows,
    [switch]$List,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

. (Join-Path $scriptDir '../support/native_build.ps1')
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_unit_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "Aperture_Unit_Tests.exe"

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib uiautomationcore.lib advapi32.lib propsys.lib dxguid.lib imm32.lib runtimeobject.lib winhttp.lib"

Write-Host "Building Aperture Unit Tests..." -ForegroundColor Cyan

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\Settings.cpp`"",
    "`"$rootDir\src\ChannelPolicy.cpp`"",
    "`"$rootDir\src\HttpTransport.cpp`"",
    "`"$rootDir\src\InternetRadio.cpp`"",
    "`"$rootDir\src\Localization.cpp`"",
    "`"$rootDir\src\MediaLibrary.cpp`"",
    "`"$rootDir\src\FolderWatcher.cpp`"",
    "`"$rootDir\src\FileAssociations.cpp`"",
    "`"$rootDir\src\JumpList.cpp`"",
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
    "`"$rootDir\src\Renderer.cpp`"",
    "`"$rootDir\src\OverlayRenderer.cpp`"",
    "`"$rootDir\src\LibrarySnapshot.cpp`"",
    "`"$rootDir\src\MediaFoundation.cpp`"",
    "`"$rootDir\src\MediaProbeIsolation.cpp`"",
    "`"$rootDir\src\TooltipManager.cpp`"",
    "`"$rootDir\src\PlaylistIO.cpp`"",
    "`"$rootDir\src\NetworkStream.cpp`"",
    "`"$rootDir\src\Podcasts.cpp`"",
    "`"$rootDir\src\TaskbarController.cpp`"",
    "`"$rootDir\src\Accessibility.cpp`"",
    "`"$scriptDir\main.cpp`""
)
$exitCode = 1
try {
    Build-ApertureTestHarness -Sources $srcFiles -Output $exeOutput -Libraries ($libs -split ' ') -Manifest (Join-Path $rootDir 'tests/support/native_tests.manifest')

    $selfTest = Join-Path $tempDir 'framework-self-test.exe'
    Build-ApertureTestHarness -Sources @((Join-Path $rootDir 'tests/support/framework_self_test.cpp')) -Output $selfTest -Libraries @('ole32.lib')
    & $selfTest
    if ($LASTEXITCODE -ne 0) { throw 'Test runner self-test failed.' }
    if (-not $LibmpvPath) { $LibmpvPath = Join-Path $rootDir 'libmpv-2.dll' }

    if (-not [string]::IsNullOrWhiteSpace($LibmpvPath)) {
        $resolvedMpv = [IO.Path]::GetFullPath($LibmpvPath)
        if (-not (Test-Path -LiteralPath $resolvedMpv -PathType Leaf)) {
            throw "libmpv-2.dll not found at: $resolvedMpv"
        }
        Copy-Item -LiteralPath $resolvedMpv -Destination (Join-Path $tempDir "libmpv-2.dll") -Force
    }

    $argsList = @()
    if ($AllowVisibleWindows) { $argsList += "--allow-visible-windows" }
    if ($Suite) { $argsList += "--suite", $Suite }
    if ($Test) { $argsList += "--test", $Test }
    if ($List) { $argsList += "--list" }
    if ($Help) { $argsList += "--help" }

    & $exeOutput $argsList
    $exitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        $resolved = [IO.Path]::GetFullPath($tempDir)
        $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
        if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^Aperture_unit_tests_[a-f0-9]{32}$') { throw 'Unsafe test build cleanup.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
exit $exitCode
