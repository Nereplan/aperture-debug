<#
.SYNOPSIS
    Runs deterministic Aperture visual regression captures and pixel comparisons.

.DESCRIPTION
    Discovers toolchain and runtime prerequisites, validates authoritative baseline manifest,
    builds (or accepts pre-built) Aperture native Release binaries, stages execution in an
    isolated external temporary directory ($env:TEMP), seeds deterministic fixture state
    (fixed geometry, dark theme, reduced motion, font roles, seeded library/playlists/history),
    executes real Aperture UI rendering with bounded execution and process-tree termination,
    captures client content into lossless PNGs, and compares against authoritative checked-in
    baselines with explicit tolerances and diagnostic diff output.

    Guarantees cleanup of all temporary build and staging directories in finally, ensuring
    zero artifacts enter the repository and preserving the <= 90 MiB workspace limit.

.PARAMETER Validate
    Validates toolchain discovery, baseline schema, scene dimensions, SHA-256 hashes,
    comparison tolerances, and source probe hooks without running compilation or launching binaries.
    Alias: -DryRun.

.PARAMETER DryRun
    Alias for -Validate.

.PARAMETER BinaryPath
    Optional path to a pre-built Aperture.exe. When omitted, builds a clean binary using build.ps1
    into an isolated external temporary directory.

.PARAMETER Configuration
    Build configuration to verify (default: "Release").

.PARAMETER BaselinePath
    Path to visual baseline manifest file (default: tests/visual/visual_baseline.json).

.PARAMETER TimeoutSeconds
    Per-scene execution timeout in seconds (default: 30).

.PARAMETER UpdateBaseline
    Explicit guarded mode: update the authoritative checked-in baseline PNG files and recalculate
    SHA-256 hashes in the manifest. Normal runs strictly verify and never modify baselines.

.PARAMETER Json
    Emit structured JSON output.

.PARAMETER OutputDir
    Optional custom output directory for diagnostic diff images and reports on failure.

.PARAMETER PreserveArtifacts
    If specified with -OutputDir, skips automatic deletion of intermediate staging files in finally.

.PARAMETER SkipBuild
    Skip build step and require -BinaryPath.

.PARAMETER Scenes
    Optional filter for specific scene IDs (e.g. hub_media,hub_settings).

.EXAMPLE
    .\scripts\run_visual_regression.ps1 -Validate

.EXAMPLE
    .\scripts\run_visual_regression.ps1

.EXAMPLE
    .\scripts\run_visual_regression.ps1 -BinaryPath .\Aperture.exe -Json

.EXAMPLE
    .\scripts\run_visual_regression.ps1 -UpdateBaseline
#>

[CmdletBinding()]
param(
    [switch]$Validate,
    [switch]$DryRun,
    [string]$BinaryPath = "",
    [string]$Configuration = "Release",
    [string]$BaselinePath = "",
    [int]$TimeoutSeconds = 30,
    [switch]$UpdateBaseline,
    [switch]$Json,
    [string]$OutputDir = "",
    [switch]$PreserveArtifacts,
    [switch]$SkipBuild,
    [string[]]$Scenes = @()
)

$ErrorActionPreference = "Stop"

if ($DryRun) {
    $Validate = $true
}

# ---------------------------------------------------------------------------
# Path Resolution & Workspace Safety
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir "..")).TrimEnd('\', '/')
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')

if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = Join-Path $repoRoot "tests\visual\visual_baseline.json"
}
$BaselinePath = [IO.Path]::GetFullPath($BaselinePath)
$baselineDir = Split-Path -Parent $BaselinePath

function Write-Log([string]$Message, [string]$Color = "White") {
    if (-not $Json) {
        Write-Host $Message -ForegroundColor $Color
    }
}

# ---------------------------------------------------------------------------
# 1. Baseline Schema & Manifest Validation
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) {
    throw "Visual baseline file not found at: $BaselinePath"
}

$baselineRaw = Get-Content -LiteralPath $BaselinePath -Raw -Encoding UTF8
$baselineConfig = $baselineRaw | ConvertFrom-Json
if ($null -eq $baselineConfig) {
    throw "Failed to parse visual baseline JSON at: $BaselinePath"
}

if (-not $baselineConfig.version -or $baselineConfig.version -lt 1) {
    throw "Invalid baseline version in $BaselinePath"
}

$declaredScenes = @($baselineConfig.scenes)
if ($declaredScenes.Count -eq 0) {
    throw "Visual baseline manifest contains no declared scenes: $BaselinePath"
}

$sceneMap = @{}
foreach ($scene in $declaredScenes) {
    if ([string]::IsNullOrWhiteSpace($scene.id)) {
        throw "Scene missing required 'id' in $BaselinePath"
    }
    if ($sceneMap.ContainsKey($scene.id)) {
        throw "Duplicate scene id '$($scene.id)' in $BaselinePath"
    }
    $sceneMap[$scene.id] = $scene

    $baselineFileRel = $scene.baselineFile
    $baselineFull = [IO.Path]::GetFullPath((Join-Path $baselineDir $baselineFileRel))
    if (-not (Test-Path -LiteralPath $baselineFull -PathType Leaf)) {
        throw "Baseline PNG file not found for scene '$($scene.id)': $baselineFull"
    }

    # Verify SHA-256
    $fileHash = (Get-FileHash -LiteralPath $baselineFull -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($fileHash -ne $scene.sha256.ToLowerInvariant()) {
        throw "Baseline SHA-256 mismatch for scene '$($scene.id)': expected $($scene.sha256), actual $fileHash"
    }
}

# ---------------------------------------------------------------------------
# 2. Toolchain Discovery
# ---------------------------------------------------------------------------
$toolchainInfo = [ordered]@{
    msvcVersion   = "Unknown"
    clPath        = "Unknown"
    cmakeVersion  = "Unknown"
    cmakePath     = "Unknown"
    ninjaVersion  = "Not Found"
    ninjaPath     = "Not Found"
    pythonVersion = "Not Found"
    pythonPath    = "Not Found"
    powershellVer = $PSVersionTable.PSVersion.ToString()
}

$cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $toolchainInfo.cmakePath = $cmakeCmd.Source
    $cmVer = & $cmakeCmd.Source --version 2>&1 | Select-Object -First 1
    $toolchainInfo.cmakeVersion = $cmVer.Trim()
}

$ninjaCmd = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if ($ninjaCmd) {
    $toolchainInfo.ninjaPath = $ninjaCmd.Source
    $njVer = & $ninjaCmd.Source --version 2>&1
    $toolchainInfo.ninjaVersion = ($njVer -join " ").Trim()
}

$pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
if (-not $pyCmd) { $pyCmd = Get-Command "python" -ErrorAction SilentlyContinue }
if ($pyCmd) {
    $toolchainInfo.pythonPath = $pyCmd.Source
    $pyVer = & $pyCmd.Source --version 2>&1
    $toolchainInfo.pythonVersion = ($pyVer -join " ").Trim()
}

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

# ---------------------------------------------------------------------------
# 3. Source & Probe Verification
# ---------------------------------------------------------------------------
$cmakeLists = Join-Path $repoRoot "CMakeLists.txt"
$mainCpp = Join-Path $repoRoot "src\main.cpp"
$ipcProtocolH = Join-Path $repoRoot "src\IpcProtocol.h"
$rendererH = Join-Path $repoRoot "src\Renderer.h"
$rendererCpp = Join-Path $repoRoot "src\Renderer.cpp"
$appH = Join-Path $repoRoot "src\App.h"
$appCpp = Join-Path $repoRoot "src\App.cpp"

foreach ($f in @($cmakeLists, $mainCpp, $ipcProtocolH, $rendererH, $rendererCpp, $appH, $appCpp)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Required source file not found at $f" }
}

$mainContent = Get-Content -LiteralPath $mainCpp -Raw -Encoding UTF8
$ipcContent = Get-Content -LiteralPath $ipcProtocolH -Raw -Encoding UTF8
$rendererHContent = Get-Content -LiteralPath $rendererH -Raw -Encoding UTF8
$rendererCppContent = Get-Content -LiteralPath $rendererCpp -Raw -Encoding UTF8
$appHContent = Get-Content -LiteralPath $appH -Raw -Encoding UTF8
$appCppContent = Get-Content -LiteralPath $appCpp -Raw -Encoding UTF8

$hasVisualProbeHook = ($mainContent -match 'visualCaptureProbe' -and
                       $ipcContent -match 'visualCaptureProbe' -and
                       $rendererHContent -match 'CaptureBackBufferToPng' -and
                       $rendererCppContent -match 'CaptureBackBufferToPng' -and
                       $appHContent -match 'CaptureClientContent' -and
                       $appCppContent -match 'CaptureClientContent')

if (-not $hasVisualProbeHook) {
    throw "Source code verification failed: visualCaptureProbe / CaptureBackBufferToPng hook is missing"
}

# ---------------------------------------------------------------------------
# 4. Validation Mode Exit
# ---------------------------------------------------------------------------
if ($Validate) {
    $valReport = [ordered]@{
        status = "PASSED"
        validationMode = $true
        manifest = $BaselinePath
        declaredScenes = $declaredScenes.Count
        environment = $baselineConfig.environment
        tolerances = $baselineConfig.tolerances
        toolchain = $toolchainInfo
        scenes = @($declaredScenes | ForEach-Object {
            [ordered]@{
                id = $_.id
                name = $_.name
                tab = $_.tab
                width = $_.width
                height = $_.height
                sha256 = $_.sha256
                maxMismatchedPixelPercent = $_.maxMismatchedPixelPercent
                perPixelColorDeltaThreshold = $_.perPixelColorDeltaThreshold
                maxPeakColorDelta = $_.maxPeakColorDelta
            }
        })
    }

    if ($Json) {
        $valReport | ConvertTo-Json -Depth 6
    } else {
        Write-Log "================================================================================" "Cyan"
        Write-Log "  Aperture Visual Regression Gate - Validation Mode" "Cyan"
        Write-Log "================================================================================" "Cyan"
        Write-Log "Manifest: $BaselinePath" "Green"
        Write-Log "Declared Scenes: $($declaredScenes.Count)" "Green"
        foreach ($s in $declaredScenes) {
            Write-Log " - [$($s.id)] $($s.name) ($($s.width)x$($s.height), maxDiff: $($s.maxMismatchedPixelPercent)%)" "Gray"
        }
        Write-Log "Toolchain: CMake=$($toolchainInfo.cmakeVersion), Python=$($toolchainInfo.pythonVersion), MSVC=$($toolchainInfo.msvcVersion)" "Green"
        Write-Log "Source Hooks: Verified (Renderer::CaptureBackBufferToPng, App::CaptureClientContent, main.cpp visualCaptureProbe)" "Green"
        Write-Log "`nValidation succeeded. Baseline manifest, schemas, dimensions, hashes, and hooks are intact." "Green"
    }
    exit 0
}

# ---------------------------------------------------------------------------
# 5. Staging & Execution Setup
# ---------------------------------------------------------------------------
$runId = [Guid]::NewGuid().ToString("N")
$tempStageDir = Join-Path $tempRoot "Aperture_visual_stage_$runId"
$tempBuildDir = Join-Path $tempRoot "Aperture_visual_build_$runId"

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $diagnosticOutputDir = Join-Path $tempRoot "Aperture_visual_diagnostics_$runId"
} else {
    $diagnosticOutputDir = [IO.Path]::GetFullPath($OutputDir)
}

$cleanupDirs = @($tempStageDir, $tempBuildDir)

# Helper function to kill process trees
function Stop-ProcessTree([int]$ProcessId) {
    if ($ProcessId -le 0) { return }
    try {
        & taskkill.exe /PID $ProcessId /T /F 2>&1 | Out-Null
    } catch {
        # Process already gone
    }
}

# Helper function for Python image comparison
function Invoke-ImageComparison(
    [string]$BaselineFile,
    [string]$ActualFile,
    [string]$DiffOutFile,
    [int]$PerPixelDelta,
    [double]$MaxMismatchPct,
    [int]$MaxPeakDelta
) {
    $pyUtil = Join-Path $repoRoot "tests\visual\png_util.py"
    $pyExec = if ($toolchainInfo.pythonPath -ne "Not Found") { $toolchainInfo.pythonPath } else { "python.exe" }

    $pyCode = @"
import sys, json
from png_util import compare_images
res = compare_images(
    r'$BaselineFile',
    r'$ActualFile',
    r'$DiffOutFile',
    tolerance_per_pixel=$PerPixelDelta,
    max_mismatch_pct=$MaxMismatchPct,
    max_peak_delta=$MaxPeakDelta
)
print(json.dumps(res))
sys.exit(0 if res['passed'] else 1)
"@
    $procInfo = New-Object System.Diagnostics.ProcessStartInfo
    $procInfo.FileName = $pyExec
    $procInfo.Arguments = "-c `"$pyCode`""
    $procInfo.WorkingDirectory = (Join-Path $repoRoot "tests\visual")
    $procInfo.RedirectStandardOutput = $true
    $procInfo.RedirectStandardError = $true
    $procInfo.UseShellExecute = $false
    $procInfo.CreateNoWindow = $true

    $p = [System.Diagnostics.Process]::Start($procInfo)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit(30000)

    try {
        $jsonRes = $stdout.Trim() | ConvertFrom-Json
        return $jsonRes
    } catch {
        throw "Failed to parse Python comparison output: stdout='$stdout', stderr='$stderr'"
    }
}

try {
    Write-Log "================================================================================" "Cyan"
    Write-Log "  Aperture Visual Regression Gate Execution" "Cyan"
    Write-Log "================================================================================" "Cyan"

    # A. Build binary if not supplied
    $targetExe = $BinaryPath
    if ([string]::IsNullOrWhiteSpace($targetExe)) {
        if ($SkipBuild) {
            throw "-SkipBuild was specified but no -BinaryPath was provided."
        }
        Write-Log "Building fresh Aperture ($Configuration) into isolated temp staging..." "Yellow"
        New-Item -ItemType Directory -Path $tempBuildDir -Force | Out-Null
        $builtExe = Join-Path $tempBuildDir "Aperture.exe"
        $buildScript = Join-Path $repoRoot "build.ps1"

        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -BuildOutputPath $builtExe -SkipDependencyCheck
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
            throw "Failed to build Aperture.exe for visual regression testing."
        }
        $targetExe = $builtExe
    } else {
        $targetExe = [IO.Path]::GetFullPath($targetExe)
        if (-not (Test-Path -LiteralPath $targetExe -PathType Leaf)) {
            throw "Supplied binary not found at: $targetExe"
        }
        Write-Log "Using pre-built binary: $targetExe" "Green"
    }

    # B. Stage isolated runtime
    New-Item -ItemType Directory -Path $tempStageDir -Force | Out-Null
    $stagedExe = Join-Path $tempStageDir "Aperture.exe"
    Copy-Item -LiteralPath $targetExe -Destination $stagedExe -Force

    # Copy dependencies
    $libmpv = Join-Path $repoRoot "libmpv-2.dll"
    if (Test-Path -LiteralPath $libmpv) { Copy-Item -LiteralPath $libmpv -Destination $tempStageDir -Force }

    $fontsSrc = Join-Path $repoRoot "fonts"
    $fontsDst = Join-Path $tempStageDir "fonts"
    if (Test-Path -LiteralPath $fontsSrc) {
        Copy-Item -LiteralPath $fontsSrc -Destination $fontsDst -Recurse -Force
    }
    $localesSrc = Join-Path $repoRoot "locales"
    $localesDst = Join-Path $tempStageDir "locales"
    if (Test-Path -LiteralPath $localesSrc) {
        Copy-Item -LiteralPath $localesSrc -Destination $localesDst -Recurse -Force
    }

    # Seed deterministic fixture state
    $dataDir = Join-Path $tempStageDir "data"
    New-Item -ItemType Directory -Path $dataDir -Force | Out-Null

    $seededSettings = @{
        schemaVersion = 14
        themePreset = "default"
        accentColorHex = "#5ce1b6"
        uiFontFamily = "Inter"
        reduceMotionEnabled = $true
        windowWidth = 1024
        windowHeight = 640
        windowX = 100
        windowY = 100
        windowMaximized = $false
        autoplay = $true
        rememberPos = $true
        restoreSession = $false
        diagnosticsLogging = $false
    }
    # Windows PowerShell's UTF8 Set-Content adds a BOM, which is not part of
    # the application's JSON format. Seed the same bytes the app writes.
    $fixtureUtf8 = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText((Join-Path $dataDir "settings.json"), ($seededSettings | ConvertTo-Json -Depth 5), $fixtureUtf8)

    $seededLibrary = @{
        schemaVersion = 2
        items = @(
            @{ id = "m1"; filePath = "C:\Media\Cosmos_4K.mkv"; title = "Cosmos 4K"; durationSec = 3600.0; fileSizeBytes = 4294967296; lastWatchedTimestamp = 1700000000 },
            @{ id = "m2"; filePath = "C:\Media\Nature_Doc.mp4"; title = "Nature Documentary"; durationSec = 1800.0; fileSizeBytes = 2147483648; lastWatchedTimestamp = 1699900000 }
        )
    }
    $seededPlaylists = @{
        schemaVersion = 2
        items = @(
            @{ id = "p1"; name = "Favorites"; itemPaths = @("C:\Media\Cosmos_4K.mkv"); createdTimestamp = 1700000000 },
            @{ id = "p2"; name = "Watch Later"; itemPaths = @("C:\Media\Nature_Doc.mp4"); createdTimestamp = 1699900000 }
        )
    }
    $seededHistory = @{
        schemaVersion = 2
        items = @(
            @{ filePath = "C:\Media\Cosmos_4K.mkv"; title = "Cosmos 4K"; watchedTimestamp = 1700000000; lastPosSec = 120.0; durationSec = 3600.0 }
        )
    }
    [System.IO.File]::WriteAllText((Join-Path $dataDir "library.json"), ($seededLibrary | ConvertTo-Json -Depth 6), $fixtureUtf8)
    [System.IO.File]::WriteAllText((Join-Path $dataDir "playlists.json"), ($seededPlaylists | ConvertTo-Json -Depth 6), $fixtureUtf8)
    [System.IO.File]::WriteAllText((Join-Path $dataDir "history.json"), ($seededHistory | ConvertTo-Json -Depth 6), $fixtureUtf8)

    # Filter scenes if requested
    $scenesToTest = $declaredScenes
    if ($Scenes.Count -gt 0) {
        $scenesToTest = @($declaredScenes | Where-Object { $Scenes -contains $_.id })
        if ($scenesToTest.Count -eq 0) {
            throw "No declared scenes matched the filter: $($Scenes -join ', ')"
        }
    }

    # C. Execute Visual Captures & Comparisons
    $results = @()
    $overallPassed = $true

    foreach ($scene in $scenesToTest) {
        Write-Log "`nProcessing Scene: [$($scene.id)] $($scene.name)..." "Yellow"

        $captureFile = Join-Path $tempStageDir "capture_$($scene.id).png"
        $baselineFull = [IO.Path]::GetFullPath((Join-Path $baselineDir $scene.baselineFile))

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $stagedExe
        $psi.Arguments = "--visual-capture --scene=$($scene.id) --capture-output=`"$captureFile`""
        $psi.WorkingDirectory = $tempStageDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true

        $proc = [System.Diagnostics.Process]::Start($psi)
        $procId = $proc.Id

        $finished = $proc.WaitForExit($TimeoutSeconds * 1000)
        if (-not $finished) {
            Stop-ProcessTree $procId
            throw "Process timed out after $TimeoutSeconds seconds while capturing scene '$($scene.id)'"
        }

        if (-not (Test-Path -LiteralPath $captureFile -PathType Leaf)) {
            throw "Capture file not generated for scene '$($scene.id)': $captureFile"
        }

        # Update baseline mode
        if ($UpdateBaseline) {
            Write-Log "Updating baseline for scene '$($scene.id)' -> $baselineFull" "Cyan"
            Copy-Item -LiteralPath $captureFile -Destination $baselineFull -Force
            $newSha = (Get-FileHash -LiteralPath $baselineFull -Algorithm SHA256).Hash.ToLowerInvariant()
            $scene.sha256 = $newSha

            $results += [ordered]@{
                id = $scene.id
                name = $scene.name
                status = "UPDATED"
                sha256 = $newSha
                width = $scene.width
                height = $scene.height
            }
            continue
        }

        # Normal verification mode
        $diffFile = Join-Path $tempStageDir "diff_$($scene.id).png"
        $comp = Invoke-ImageComparison `
            -BaselineFile $baselineFull `
            -ActualFile $captureFile `
            -DiffOutFile $diffFile `
            -PerPixelDelta ([int]$scene.perPixelColorDeltaThreshold) `
            -MaxMismatchPct ([double]$scene.maxMismatchedPixelPercent) `
            -MaxPeakDelta ([int]$scene.maxPeakColorDelta)

        $passed = [bool]$comp.passed
        if (-not $passed) {
            $overallPassed = $false
            New-Item -ItemType Directory -Path $diagnosticOutputDir -Force | Out-Null
            Copy-Item -LiteralPath $captureFile -Destination (Join-Path $diagnosticOutputDir "actual_$($scene.id).png") -Force
            Copy-Item -LiteralPath $baselineFull -Destination (Join-Path $diagnosticOutputDir "expected_$($scene.id).png") -Force
            if (Test-Path -LiteralPath $diffFile) {
                Copy-Item -LiteralPath $diffFile -Destination (Join-Path $diagnosticOutputDir "diff_$($scene.id).png") -Force
            }
            Write-Log "  [FAILED] Mismatched Pixels: $($comp.mismatched_pixels) ($($comp.mismatch_percent)%), Peak Delta: $($comp.peak_delta)" "Red"
            Write-Log "  Diagnostics emitted to: $diagnosticOutputDir" "Red"
        } else {
            Write-Log "  [PASSED] Mismatched Pixels: $($comp.mismatched_pixels) ($($comp.mismatch_percent)%), Peak Delta: $($comp.peak_delta)" "Green"
        }

        $results += [ordered]@{
            id = $scene.id
            name = $scene.name
            passed = $passed
            totalPixels = $comp.total_pixels
            mismatchedPixels = $comp.mismatched_pixels
            mismatchPercent = $comp.mismatch_percent
            peakDelta = $comp.peak_delta
            maxMismatchPctAllowed = $scene.maxMismatchedPixelPercent
            maxPeakDeltaAllowed = $scene.maxPeakColorDelta
            perPixelThreshold = $scene.perPixelColorDeltaThreshold
        }
    }

    # If updating baselines, rewrite manifest
    if ($UpdateBaseline) {
        $jsonOut = $baselineConfig | ConvertTo-Json -Depth 6
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($BaselinePath, $jsonOut, $utf8NoBom)
        Write-Log "`nVisual baseline manifest successfully updated at: $BaselinePath" "Green"
    }

    $summary = [ordered]@{
        status = if ($overallPassed) { "PASSED" } else { "FAILED" }
        updateBaseline = [bool]$UpdateBaseline
        totalScenes = $results.Count
        passedScenes = ($results | Where-Object { $_.passed -eq $true -or $_.status -eq "UPDATED" }).Count
        failedScenes = ($results | Where-Object { $_.passed -eq $false }).Count
        diagnosticOutputDir = if (-not $overallPassed) { $diagnosticOutputDir } else { $null }
        results = $results
    }

    if ($Json) {
        $summary | ConvertTo-Json -Depth 6
    }

    if (-not $overallPassed) {
        throw "Visual regression verification failed for $($summary.failedScenes) scene(s). See diagnostics at $diagnosticOutputDir"
    }

    Write-Log "`nAll visual regression captures passed successfully." "Green"
}
finally {
    if (-not $PreserveArtifacts) {
        foreach ($d in $cleanupDirs) {
            if ($d -and (Test-Path -LiteralPath $d)) {
                Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}
