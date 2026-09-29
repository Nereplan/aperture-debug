param(
    [switch]$Help,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

if ($Help) {
    Write-Host "Aperture Direct2D/DirectWrite Rendering Integration Test Runner"
    Write-Host "Usage: .\run_tests.ps1 [-Verbose] [-Help]"
    exit 0
}

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

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_integration_tests_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "Aperture_Integration_Tests.exe"

$libs = "d2d1.lib dwrite.lib user32.lib gdi32.lib ole32.lib advapi32.lib shell32.lib"
$sourceFile = Join-Path $scriptDir "test_render.cpp"

Write-Host "Building Aperture Rendering Integration Tests..." -ForegroundColor Cyan

$clCmd = "`"$vcvars`" >nul 2>&1 && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" `"$sourceFile`" $libs /Fo`"$tempDir\\`" /Fe:`"$exeOutput`" /link /SUBSYSTEM:CONSOLE"

$exitCode = 1
try {
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with code $LASTEXITCODE"
    }

    Write-Host "Running rendering integration tests..." -ForegroundColor Green
    & $exeOutput
    $exitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
