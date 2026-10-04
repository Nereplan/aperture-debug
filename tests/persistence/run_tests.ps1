param(
    [string]$Suite = "",
    [string]$Test = "",
    [switch]$List,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

. (Join-Path $scriptDir '../support/native_build.ps1')
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_persistence_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "Aperture_Persistence_Tests.exe"

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib propsys.lib dxguid.lib imm32.lib"

Write-Host "Building Aperture Persistence Tests..." -ForegroundColor Cyan

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\Settings.cpp`"",
    "`"$rootDir\src\AudioConfig.cpp`"",
    "`"$rootDir\src\ChannelPolicy.cpp`"",
    "`"$rootDir\src\LibraryQuery.cpp`"",
    "`"$rootDir\src\Localization.cpp`"",
    "`"$rootDir\src\MediaLibrary.cpp`"",
    "`"$rootDir\src\MediaFoundation.cpp`"",
    "`"$rootDir\src\MediaProbeIsolation.cpp`"",
    "`"$rootDir\src\FolderWatcher.cpp`"",
    "`"$rootDir\src\PlaybackSession.cpp`"",
    "`"$scriptDir\main.cpp`""
)
$exitCode = 1
try {
    Build-ApertureTestHarness -Sources $srcFiles -Output $exeOutput -Libraries ($libs -split ' ')

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
        $resolved = [IO.Path]::GetFullPath($tempDir)
        $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
        if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^Aperture_persistence_tests_[a-f0-9]{32}$') { throw 'Unsafe test build cleanup.' }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

exit $exitCode
