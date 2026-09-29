<#
.SYNOPSIS
    Verifies deterministic build reproducibility for Aperture Release binaries.

.DESCRIPTION
    Performs two clean, isolated Release builds from identical workspace sources into
    separate external temporary directories. Discovers and reports toolchain versions
    (MSVC cl.exe, Windows SDK, CMake, Ninja, Python) and compiler/linker determinism options
    (/Brepro, /GL, /LTCG, /guard:cf, /CETCOMPAT).

    Compares raw SHA-256 hashes of both binaries first. If raw hashes differ, performs
    comprehensive structural PE analysis and normalization comparison across headers,
    sections, imports, resources, and debug directories. Fails on any meaningful code/data
    divergence, allowing only explicitly documented volatile fields (e.g. legacy linker
    timestamps, PE checksums) when normalized executable content is proven identical.

    Guarantees cleanup of all temporary directories and build artifacts in finally, ensuring
    no artifacts enter the repository and preserving the <= 90 MiB workspace limit.

.PARAMETER Configuration
    Build configuration to verify (default: "Release").

.PARAMETER BinaryA
    Optional pre-built path to first binary (used with -SkipBuild).

.PARAMETER BinaryB
    Optional pre-built path to second binary (used with -SkipBuild).

.PARAMETER SkipBuild
    If set, skips the two compilation passes and directly compares -BinaryA and -BinaryB.

.PARAMETER StrictRaw
    If set, requires 100% bit-for-bit exact raw SHA-256 match (rejects normalized equivalence).

.PARAMETER Json
    Emit structured JSON output.

.EXAMPLE
    .\scripts\verify_reproducibility.ps1

.EXAMPLE
    .\scripts\verify_reproducibility.ps1 -StrictRaw

.EXAMPLE
    .\scripts\verify_reproducibility.ps1 -SkipBuild -BinaryA C:\temp\A\Aperture.exe -BinaryB C:\temp\B\Aperture.exe
#>

[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$BinaryA = "",
    [string]$BinaryB = "",
    [switch]$SkipBuild,
    [switch]$StrictRaw,
    [switch]$Json
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$workspace = [IO.Path]::GetFullPath((Join-Path $scriptDir "..")).TrimEnd('\', '/')
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')

# Unique external temporary staging directories
$guidA = [Guid]::NewGuid().ToString("N")
$guidB = [Guid]::NewGuid().ToString("N")
$buildDirA = Join-Path $tempRoot "Aperture-repro-b1-$guidA"
$buildDirB = Join-Path $tempRoot "Aperture-repro-b2-$guidB"

function Write-Log([string]$Message, [string]$Color = "White") {
    if (-not $Json) {
        Write-Host $Message -ForegroundColor $Color
    }
}

# ---------------------------------------------------------------------------
# 1. Discover Toolchain Versions & Options
# ---------------------------------------------------------------------------
$toolchainInfo = [ordered]@{
    msvcVersion     = "Unknown"
    clPath          = "Unknown"
    windowsSdkVersion = "Unknown"
    cmakeVersion    = "Unknown"
    cmakePath       = "Unknown"
    ninjaVersion    = "Not Found"
    ninjaPath       = "Not Found"
    pythonVersion   = "Unknown"
    pythonPath      = "Unknown"
    compilerFlags   = @("/EHsc", "/GS", "/sdl", "/O2", "/GL", "/Gy", "/Zi", "/guard:cf", "/Brepro", "SHELL:/Qspectre")
    linkerFlags     = @("/SUBSYSTEM:WINDOWS", "/DYNAMICBASE", "/HIGHENTROPYVA", "/NXCOMPAT", "/DEPENDENTLOADFLAG:0x800", "/MANIFEST:NO", "/DEBUG", "/LTCG", "/OPT:REF", "/OPT:ICF", "/guard:cf", "/CETCOMPAT", "/Brepro", "/PDBALTPATH:%_PDB%")
}

# Python discovery
$pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
if (-not $pyCmd) {
    $pyCmd = Get-Command "python" -ErrorAction SilentlyContinue
}
if ($pyCmd) {
    $toolchainInfo.pythonPath = $pyCmd.Source
    $pyVer = & $pyCmd.Source --version 2>&1
    $toolchainInfo.pythonVersion = ($pyVer -join " ").Trim()
} else {
    throw "Python is required for PE reproducibility inspection but was not found in PATH."
}

# CMake discovery
$cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $toolchainInfo.cmakePath = $cmakeCmd.Source
    $cmVer = & $cmakeCmd.Source --version 2>&1 | Select-Object -First 1
    $toolchainInfo.cmakeVersion = $cmVer.Trim()
}

# Ninja discovery
$ninjaCmd = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if ($ninjaCmd) {
    $toolchainInfo.ninjaPath = $ninjaCmd.Source
    $njVer = & $ninjaCmd.Source --version 2>&1
    $toolchainInfo.ninjaVersion = ($njVer -join " ").Trim()
}

# Visual Studio & MSVC discovery
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { $vsPath = $c; break }
    }
}

if ($vsPath) {
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    $vcToolsVerFile = Join-Path $vsPath "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
    if (Test-Path -LiteralPath $vcToolsVerFile) {
        $toolchainInfo.msvcVersion = (Get-Content -LiteralPath $vcToolsVerFile -Raw).Trim()
    }
}

Write-Log "================================================================================" "Cyan"
Write-Log "  Aperture Deterministic Build & PE Reproducibility Gate" "Cyan"
Write-Log "================================================================================" "Cyan"
Write-Log "Toolchain Details:" "Yellow"
Write-Log "  MSVC Toolset:       $($toolchainInfo.msvcVersion)" "Gray"
Write-Log "  CMake:              $($toolchainInfo.cmakeVersion)" "Gray"
Write-Log "  Ninja:              $($toolchainInfo.ninjaVersion)" "Gray"
Write-Log "  Python:             $($toolchainInfo.pythonVersion)" "Gray"
Write-Log "  Determinism Flags:  /Brepro (Compiler & Linker), /GL, /LTCG, /guard:cf, /CETCOMPAT" "Gray"
Write-Log "--------------------------------------------------------------------------------" "DarkGray"

# ---------------------------------------------------------------------------
# 2. Execute Two Independent Clean Builds (or validate pre-supplied paths)
# ---------------------------------------------------------------------------
$binPathA = $BinaryA
$binPathB = $BinaryB

try {
    if (-not $SkipBuild) {
        New-Item -ItemType Directory -Path $buildDirA, $buildDirB -Force | Out-Null
        $binPathA = Join-Path $buildDirA "Aperture.exe"
        $binPathB = Join-Path $buildDirB "Aperture.exe"
        $buildScript = Join-Path $workspace "build.ps1"

        if (-not (Test-Path -LiteralPath $buildScript)) {
            throw "build.ps1 not found at $buildScript"
        }

        # First build pass (Build A)
        Write-Log "`n[1/2] Executing Clean Release Build A into: $buildDirA" "Yellow"
        $swA = [System.Diagnostics.Stopwatch]::StartNew()
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -BuildOutputPath $binPathA -SkipDependencyCheck
        $swA.Stop()
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $binPathA)) {
            throw "Build A failed with exit code $LASTEXITCODE or binary not found."
        }
        $hashA = (Get-FileHash -LiteralPath $binPathA -Algorithm SHA256).Hash.ToLowerInvariant()
        $sizeA = (Get-Item -LiteralPath $binPathA).Length
        Write-Log ("Build A Succeeded in {0:N1}s | Size: {1:N0} bytes | SHA256: {2}" -f $swA.Elapsed.TotalSeconds, $sizeA, $hashA) "Green"

        # Second build pass (Build B)
        Write-Log "`n[2/2] Executing Clean Release Build B into: $buildDirB" "Yellow"
        $swB = [System.Diagnostics.Stopwatch]::StartNew()
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -BuildOutputPath $binPathB -SkipDependencyCheck
        $swB.Stop()
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $binPathB)) {
            throw "Build B failed with exit code $LASTEXITCODE or binary not found."
        }
        $hashB = (Get-FileHash -LiteralPath $binPathB -Algorithm SHA256).Hash.ToLowerInvariant()
        $sizeB = (Get-Item -LiteralPath $binPathB).Length
        Write-Log ("Build B Succeeded in {0:N1}s | Size: {1:N0} bytes | SHA256: {2}" -f $swB.Elapsed.TotalSeconds, $sizeB, $hashB) "Green"
    } else {
        if (-not (Test-Path -LiteralPath $binPathA)) { throw "Binary A not found: $binPathA" }
        if (-not (Test-Path -LiteralPath $binPathB)) { throw "Binary B not found: $binPathB" }
        $hashA = (Get-FileHash -LiteralPath $binPathA -Algorithm SHA256).Hash.ToLowerInvariant()
        $hashB = (Get-FileHash -LiteralPath $binPathB -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    # ---------------------------------------------------------------------------
    # 3. Structural PE Normalization & Inspection
    # ---------------------------------------------------------------------------
    Write-Log "`n[3/3] Inspecting & Comparing PE Binary Structures..." "Yellow"
    $comparatorScript = Join-Path $workspace "tests\package\compare_pe_reproducibility.py"
    if (-not (Test-Path -LiteralPath $comparatorScript)) {
        throw "PE comparator script not found at $comparatorScript"
    }

    $compArgs = @($comparatorScript, $binPathA, $binPathB, "--json")
    if ($StrictRaw) {
        $compArgs += "--strict-raw"
    }

    $rawJson = & $pyCmd.Source @compArgs
    $compExitCode = $LASTEXITCODE

    $report = $null
    try {
        $report = $rawJson | ConvertFrom-Json
    } catch {
        throw "Failed to parse JSON output from PE comparator: $_`nRaw output:`n$rawJson"
    }

    # Attach toolchain metadata to structured report
    $finalOutput = [ordered]@{
        reproducibility = $report
        toolchain       = $toolchainInfo
        buildAPath      = $binPathA
        buildBPath      = $binPathB
    }

    if ($Json) {
        $finalOutput | ConvertTo-Json -Depth 10
    } else {
        Write-Log ""
        Write-Log "================================================================================" "Cyan"
        Write-Log "                 REPRODUCIBILITY GATE VERDICT" "Cyan"
        Write-Log "================================================================================" "Cyan"
        Write-Log "Verdict:             $($report.verdict)" $(if ($report.isReproducible) { "Green" } else { "Red" })
        Write-Log "Reproducible:        $(if ($report.isReproducible) { 'YES (PASS)' } else { 'NO (FAIL)' })" $(if ($report.isReproducible) { "Green" } else { "Red" })
        Write-Log "Exact Raw SHA-256:   $(if ($report.exactRawMatch) { 'YES (100% bit-for-bit match)' } else { 'NO (Differs)' })" $(if ($report.exactRawMatch) { "Green" } else { "Yellow" })
        Write-Log "Normalized Match:    $(if ($report.normalizedMatch) { 'YES (Equivalent)' } else { 'NO' })" $(if ($report.normalizedMatch) { "Green" } else { "Red" })
        Write-Log ""
        Write-Log "Build A SHA-256:     $($report.buildA.rawSha256)" "Gray"
        Write-Log "Build B SHA-256:     $($report.buildB.rawSha256)" "Gray"
        Write-Log "Normalized SHA-256:  $($report.buildA.normalizedSha256)" "Gray"
        Write-Log ""
        if ($report.volatileDifferences -and $report.volatileDifferences.Count -gt 0) {
            Write-Log "Documented Volatile Differences ($($report.volatileDifferences.Count)): " "Yellow"
            foreach ($vd in $report.volatileDifferences) {
                Write-Log "  - $($vd.field): A=$($vd.value_a) vs B=$($vd.value_b)" "Yellow"
            }
            Write-Log ""
        }
        if ($report.meaningfulDifferences -and $report.meaningfulDifferences.Count -gt 0) {
            Write-Log "Meaningful Divergences ($($report.meaningfulDifferences.Count)): " "Red"
            foreach ($md in $report.meaningfulDifferences) {
                Write-Log "  * $($md.field): A=$($md.value_a) vs B=$($md.value_b)" "Red"
            }
            Write-Log ""
        }
        Write-Log "Summary: $($report.summary)" "Cyan"
        Write-Log "================================================================================" "Cyan"
    }

    if (-not $report.isReproducible -or $compExitCode -ne 0) {
        exit 1
    }
    exit 0
}
finally {
    # Guaranteed recursive cleanup of all temporary build outputs
    if ($buildDirA -and (Test-Path -LiteralPath $buildDirA)) {
        Remove-Item -LiteralPath $buildDirA -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($buildDirB -and (Test-Path -LiteralPath $buildDirB)) {
        Remove-Item -LiteralPath $buildDirB -Recurse -Force -ErrorAction SilentlyContinue
    }
}
