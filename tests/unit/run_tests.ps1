param(
    [string]$Suite = "",
    [string]$Test = "",
    [string]$LibmpvPath = "",
    [switch]$List,
    [switch]$Help
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

$vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}
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
    "`"$rootDir\src\Localization.cpp`"",
    "`"$rootDir\src\MediaLibrary.cpp`"",
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
    "`"$rootDir\src\Renderer.cpp`"",
    "`"$rootDir\src\LibrarySnapshot.cpp`"",
    "`"$rootDir\src\MediaFoundation.cpp`"",
    "`"$rootDir\src\TooltipManager.cpp`"",
    "`"$rootDir\src\PlaylistIO.cpp`"",
    "`"$rootDir\src\NetworkStream.cpp`"",
    "`"$rootDir\src\TaskbarController.cpp`"",
    "`"$rootDir\src\Accessibility.cpp`"",
    "`"$scriptDir\main.cpp`""
)
$sourcesJoined = $srcFiles -join " "

$clCmd = "`"$vcvars`" && cl.exe /nologo /utf-8 /bigobj /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" /Fo`"$tempDir\\`" $sourcesJoined $libs /Fe:`"$exeOutput`" /link /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /MANIFESTINPUT:`"$rootDir\tests\support\native_tests.manifest`""

try {
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with code $LASTEXITCODE"
    }

    if (-not [string]::IsNullOrWhiteSpace($LibmpvPath)) {
        $resolvedMpv = [IO.Path]::GetFullPath($LibmpvPath)
        if (-not (Test-Path -LiteralPath $resolvedMpv -PathType Leaf)) {
            throw "libmpv-2.dll not found at: $resolvedMpv"
        }
        Copy-Item -LiteralPath $resolvedMpv -Destination (Join-Path $tempDir "libmpv-2.dll") -Force
    }

    $argsList = @()
    if ($Suite) { $argsList += "--suite", $Suite }
    if ($Test) { $argsList += "--test", $Test }
    if ($List) { $argsList += "--list" }
    if ($Help) { $argsList += "--help" }

    & $exeOutput $argsList
    $exitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
exit $exitCode
