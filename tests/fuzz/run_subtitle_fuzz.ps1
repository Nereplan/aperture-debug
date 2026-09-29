param(
    [int64]$Seed = 0,
    [int64]$Iterations = 0,
    [switch]$Verbose,
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

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_subtitle_fuzz_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "subtitle_fuzz.exe"

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib propsys.lib dxguid.lib imm32.lib"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "  Building Aperture Subtitle Fuzz Tests" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

$srcFiles = @(
    "`"$scriptDir\subtitle_fuzz.cpp`"",
    "`"$rootDir\src\Subtitles.cpp`""
)
$sourcesJoined = $srcFiles -join " "

$clCmd = "`"$vcvars`" >nul 2>&1 && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" $sourcesJoined $libs /Fo`"$tempDir\\`" /Fe:`"$exeOutput`" /link /SUBSYSTEM:CONSOLE"

$testExitCode = 1
try {
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with code $LASTEXITCODE"
    }

    $argsList = @()
    if ($Seed -gt 0) { $argsList += "--seed", $Seed }
    if ($Iterations -gt 0) { $argsList += "--iterations", $Iterations }
    if ($Verbose) { $argsList += "--verbose" }
    if ($Help) { $argsList += "--help" }

    Write-Host "`nRunning subtitle fuzz test runner...`n" -ForegroundColor Green
    & $exeOutput $argsList
    $testExitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $testExitCode
