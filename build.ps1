param(
    [string]$BuildOutputPath = "",
    [string]$PdbOutputPath = "",
    [switch]$Debug,
    [switch]$Sanitize,
    [switch]$Analyze,
    [switch]$SkipDependencyCheck
)

$ErrorActionPreference = "Stop"
if ($Debug -and $Sanitize) {
    throw "-Debug and -Sanitize are mutually exclusive build configurations."
}
if ($Sanitize -and $Analyze) {
    throw "-Sanitize and -Analyze are mutually exclusive build configurations."
}
$config = if ($Sanitize) { "Sanitize" } elseif ($Debug) { "Debug" } else { "Release" }
if ([string]::IsNullOrWhiteSpace($BuildOutputPath)) {
    $defaultName = switch ($config) {
        "Debug" { "Aperture.exe" }
        "Sanitize" { "Aperture.Sanitize.exe" }
        default { "Aperture.Release.exe" }
    }
    $BuildOutputPath = Join-Path $PSScriptRoot ("debug\" + $defaultName)
}
$BuildOutputPath = [IO.Path]::GetFullPath($BuildOutputPath)
$outDir = [IO.Path]::GetDirectoryName($BuildOutputPath)
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

# 0. Dependency and Vendored Artifact Integrity Check
if (-not $SkipDependencyCheck) {
    $depScript = Join-Path $PSScriptRoot "scripts\check_dependencies.ps1"
    if (Test-Path -LiteralPath $depScript) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $depScript -Validate -Quiet
        if ($LASTEXITCODE -ne 0) {
            throw "Dependency validation failed. Run '.\scripts\check_dependencies.ps1' for detailed diagnosis."
        }
    }
}

# 1. Discover Visual Studio 2022 installation and vcvars64.bat
$vsPath = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath -or -not (Test-Path -LiteralPath $vsPath)) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($cand in $candidates) {
        if (Test-Path -LiteralPath $cand) { $vsPath = $cand; break }
    }
}

if (-not $vsPath -or -not (Test-Path -LiteralPath $vsPath)) {
    throw "Visual Studio 2022 installation not found!"
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

# 2. Discover CMake executable
$cmakeExe = $null
$cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if ($cmakeCmd -and (Test-Path -LiteralPath $cmakeCmd.Source)) {
    $cmakeExe = $cmakeCmd.Source
}
if (-not $cmakeExe) {
    $bundledCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $bundledCmake) {
        $cmakeExe = $bundledCmake
    }
}
if (-not $cmakeExe -or -not (Test-Path -LiteralPath $cmakeExe)) {
    throw "CMake executable not found! Ensure CMake is installed and in PATH or Visual Studio 2022 with C++ CMake tools is installed."
}

# 3. Discover Ninja executable if available
$ninjaExe = $null
$ninjaCmd = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if ($ninjaCmd -and (Test-Path -LiteralPath $ninjaCmd.Source)) {
    $ninjaExe = $ninjaCmd.Source
}
if (-not $ninjaExe) {
    $bundledNinja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path -LiteralPath $bundledNinja) {
        $ninjaExe = $bundledNinja
    }
}

Write-Host "========================================" -ForegroundColor Cyan
$analysisSuffix = if ($Analyze) { ", Static Analysis" } else { "" }
Write-Host "  Building Aperture Native ($config$analysisSuffix)" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_cmake_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

try {
    if ($ninjaExe) {
        $analysisFlag = if ($Analyze) { "-DAPERTURE_ENABLE_STATIC_ANALYSIS=ON" } else { "" }
        Write-Host "Configuring CMake (Ninja toolchain, $config)..." -ForegroundColor Yellow
        $configureCmd = "call `"$vcvars`" && `"$cmakeExe`" -S `"$PSScriptRoot`" -B `"$tempDir`" -G Ninja -DCMAKE_BUILD_TYPE=$config $analysisFlag -DCMAKE_MAKE_PROGRAM=`"$ninjaExe`""
        cmd.exe /c $configureCmd
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed with exit code $LASTEXITCODE"
        }

        Write-Host "Building target Aperture ($config)..." -ForegroundColor Yellow
        $buildCmd = "call `"$vcvars`" && `"$cmakeExe`" --build `"$tempDir`" --config $config --target Aperture"
        cmd.exe /c $buildCmd
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed with exit code $LASTEXITCODE"
        }
    }
    else {
        Write-Host "Configuring CMake (Visual Studio 17 2022 generator, $config)..." -ForegroundColor Yellow
        $configureArgs = @(
            "-S", $PSScriptRoot,
            "-B", $tempDir,
            "-G", "Visual Studio 17 2022",
            "-A", "x64",
            "-DCMAKE_CONFIGURATION_TYPES=Release;Debug;Sanitize",
            "-DCMAKE_BUILD_TYPE=$config"
        )
        if ($Analyze) {
            $configureArgs += "-DAPERTURE_ENABLE_STATIC_ANALYSIS=ON"
        }
        & $cmakeExe @configureArgs
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed with exit code $LASTEXITCODE"
        }

        Write-Host "Building target Aperture ($config)..." -ForegroundColor Yellow
        $buildArgs = @(
            "--build", $tempDir,
            "--config", $config,
            "--target", "Aperture"
        )
        & $cmakeExe @buildArgs
        if ($LASTEXITCODE -ne 0) {
            throw "CMake build failed with exit code $LASTEXITCODE"
        }
    }

    $candidatePaths = @(
        (Join-Path $tempDir "$config\Aperture.exe"),
        (Join-Path $tempDir "Aperture.exe")
    )
    $builtExe = $null
    foreach ($cand in $candidatePaths) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $builtExe = $cand
            break
        }
    }

    if (-not $builtExe) {
        throw "Build succeeded but output executable was not found in $tempDir"
    }

    Copy-Item -LiteralPath $builtExe -Destination $BuildOutputPath -Force

    # Discover and copy corresponding PDB file if produced
    $candidatePdbPaths = @(
        (Join-Path $tempDir "$config\Aperture.pdb"),
        (Join-Path $tempDir "Aperture.pdb")
    )
    $builtPdb = $null
    foreach ($cand in $candidatePdbPaths) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $builtPdb = $cand
            break
        }
    }
    $finalPdbPath = $null
    if (-not [string]::IsNullOrWhiteSpace($PdbOutputPath)) {
        if (-not $builtPdb) {
            throw "PDB output was requested but the build did not produce Aperture.pdb."
        }
        $finalPdbPath = [IO.Path]::GetFullPath($PdbOutputPath)
        $pdbDir = [IO.Path]::GetDirectoryName($finalPdbPath)
        if ($pdbDir -and -not (Test-Path -LiteralPath $pdbDir)) {
            New-Item -ItemType Directory -Path $pdbDir -Force | Out-Null
        }
        Copy-Item -LiteralPath $builtPdb -Destination $finalPdbPath -Force
    }

    Write-Host "`nSUCCESS! Built native binary: $BuildOutputPath" -ForegroundColor Green
    $fileInfo = Get-Item -LiteralPath $BuildOutputPath
    Write-Host ("Binary size: {0:N0} bytes ({1:N2} MB)" -f $fileInfo.Length, ($fileInfo.Length / 1MB)) -ForegroundColor Cyan
    if ($finalPdbPath -and (Test-Path -LiteralPath $finalPdbPath)) {
        $pdbInfo = Get-Item -LiteralPath $finalPdbPath
        Write-Host ("PDB size:    {0:N0} bytes ({1:N2} MB) -> {2}" -f $pdbInfo.Length, ($pdbInfo.Length / 1MB), $finalPdbPath) -ForegroundColor Cyan
    }
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
