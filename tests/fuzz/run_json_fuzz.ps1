param(
    [string]$Seed = "",
    [string]$Iterations = "",
    [string]$MaxLen = "",
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

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_json_fuzz_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "json_fuzz.exe"

$libs = "user32.lib gdi32.lib ole32.lib advapi32.lib shell32.lib"

Write-Host "Building Aperture JsonValue Fuzz Harness..." -ForegroundColor Cyan

$srcFiles = @(
    "`"$rootDir\src\Json.cpp`"",
    "`"$scriptDir\json_fuzz.cpp`""
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
    if ($Seed) { $argsList += "--seed", $Seed }
    if ($Iterations) { $argsList += "--iterations", $Iterations }
    if ($MaxLen) { $argsList += "--max-len", $MaxLen }
    if ($Verbose) { $argsList += "--verbose" }
    if ($Help) { $argsList += "--help" }

    Write-Host "`nRunning JsonValue mutation fuzz harness...`n" -ForegroundColor Green
    & $exeOutput $argsList
    $testExitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

exit $testExitCode
