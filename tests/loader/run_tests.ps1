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

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_loader_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

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
    "`"$rootDir\src\Diagnostics.cpp`"",
    "`"$rootDir\src\MpvPlayer.cpp`"",
    "`"$scriptDir\main.cpp`""
)

$libs = "advapi32.lib user32.lib gdi32.lib shell32.lib ole32.lib shlwapi.lib d2d1.lib d3d11.lib dxgi.lib windowscodecs.lib dwrite.lib dbghelp.lib"
$sourcesJoined = $srcFiles -join " "

$exitCode = 1
try {
    $clCmd = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" /Fo`"$tempDir\\`" $sourcesJoined $libs /Fe:`"$outExe`" /link /SUBSYSTEM:CONSOLE"
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
