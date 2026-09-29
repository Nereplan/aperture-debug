param(
    [string]$Seed = "0xABCD12345678",
    [int]$Cases = 250,
    [switch]$Verbose,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

if ($Help) {
    Write-Host "Aperture Profile Fuzz Runner"
    Write-Host "Usage: .\run_profile_fuzz.ps1 [-Seed <uint64>] [-Cases <int>] [-Verbose] [-Help]"
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

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_profile_fuzz_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "profile_fuzz.exe"

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib propsys.lib dxguid.lib imm32.lib"
Write-Host "Building Aperture Profile Deterministic Fuzz Harness..." -ForegroundColor Cyan

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$rootDir\src\Settings.cpp`"",
    "`"$rootDir\src\MediaLibrary.cpp`"",
    "`"$scriptDir\profile_fuzz.cpp`""
)
$sourcesJoined = $srcFiles -join " "

$exitCode = 0
try {
    $clCmd = "`"$vcvars`" >nul 2>&1 && cl.exe /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$rootDir\src`" /I `"$rootDir`" /I `"$rootDir\tests\support`" /I `"$scriptDir`" /Fo`"$tempDir\\`" $sourcesJoined $libs /Fe:`"$exeOutput`" /link /SUBSYSTEM:CONSOLE"
    cmd.exe /c $clCmd
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed with code $LASTEXITCODE"
    }

    $argsList = @()
    if ($Seed) { $argsList += "--seed", $Seed }
    if ($Cases) { $argsList += "--cases", "$Cases" }
    if ($Verbose) { $argsList += "--verbose" }

    Write-Host "`nRunning profile deterministic fuzz tests..." -ForegroundColor Yellow
    & $exeOutput $argsList
    $exitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $exitCode
