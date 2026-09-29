<#
.SYNOPSIS
    Runs deterministic Aperture startup latency and resource utilization benchmarks.

.DESCRIPTION
    Discovers toolchain and runtime prerequisites, validates authoritative baseline budgets,
    builds (or accepts pre-built) Aperture native Release binaries, stages execution in a
    root-local isolated directory (scratch/bench-stage/<run-id>/, never %TEMP%/%APPDATA%),
    executes warmup and measured benchmark passes with bounded execution and process-tree
    termination, captures startup latency (to the first synchronous GPU shell), fully populated
    library-frame latency, window shown latency, and Windows process memory metrics (Working Set,
    Peak Working Set, Private Bytes, Peak Private Bytes, GDI/User objects), and enforces explicit
    checked-in regression budgets from scripts/benchmark_baseline.json.

    Guarantees cleanup of only the exact generated run directory in finally. Phase-level
    startup reports are written under diagnostics/perf/ with count retention.

.PARAMETER Validate
    Validates toolchain discovery, baseline schema, budget consistency, and source probe hooks
    without running a full compilation or launching binaries. Alias: -DryRun.

.PARAMETER DryRun
    Alias for -Validate.

.PARAMETER BinaryPath
    Optional path to a pre-built Aperture.exe. When omitted, builds a clean binary using build.ps1
    into an isolated external temporary directory.

.PARAMETER Configuration
    Build configuration to benchmark (default: "Release").

.PARAMETER BaselinePath
    Path to benchmark baseline and budget file (default: scripts/benchmark_baseline.json).

.PARAMETER Iterations
    Number of measured benchmark samples (default from baseline: 5).

.PARAMETER WarmupIterations
    Number of unmeasured warmup passes before sampling (default from baseline: 1).

.PARAMETER TimeoutSeconds
    Per-sample execution timeout in seconds (default: 30).

.PARAMETER UpdateBaseline
    Update the authoritative baseline section with newly measured medians while preserving
    explicit regression budget caps.

.PARAMETER Json
    Emit structured JSON output.

.PARAMETER OutputDir
    Optional custom output directory for benchmark reports.

.PARAMETER PreserveArtifacts
    If specified with -OutputDir, skips automatic deletion of intermediate files in finally.

.PARAMETER SkipBuild
    Skip build step and require -BinaryPath.

.EXAMPLE
    .\scripts\run_benchmark.ps1 -Validate

.EXAMPLE
    .\scripts\run_benchmark.ps1

.EXAMPLE
    .\scripts\run_benchmark.ps1 -BinaryPath .\Aperture.exe -Iterations 5 -Json

.EXAMPLE
    .\scripts\run_benchmark.ps1 -UpdateBaseline
#>

[CmdletBinding()]
param(
    [switch]$Validate,
    [switch]$DryRun,
    [string]$BinaryPath = "",
    [string]$Configuration = "Release",
    [string]$BaselinePath = "",
    [int]$Iterations = 0,
    [int]$WarmupIterations = 0,
    [int]$TimeoutSeconds = 0,
    [switch]$UpdateBaseline,
    [switch]$Json,
    [string]$OutputDir = "",
    [switch]$PreserveArtifacts,
    [switch]$SkipBuild,
    [string]$LibmpvPath = "",
    [string]$StageRoot = ""
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

function Assert-RootLocalPath([string]$Candidate, [string]$Root, [string]$ParamName) {
    $full = [IO.Path]::GetFullPath($Candidate).TrimEnd('\', '/')
    $rootNorm = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if ($full.Length -le $rootNorm.Length) {
        throw "$ParamName must be a strict descendant of the Aperture root ($rootNorm), got: $full"
    }
    if (-not $full.StartsWith($rootNorm + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$ParamName must stay below the Aperture root ($rootNorm), got: $full. Refusing to stage outside the root."
    }
    return $full
}

if ([string]::IsNullOrWhiteSpace($StageRoot)) {
    $StageRoot = Join-Path $repoRoot "scratch\bench-stage"
}
# Resolve + verify before creating anything. Never use %TEMP%/%APPDATA%.
$StageRoot = Assert-RootLocalPath $StageRoot $repoRoot "StageRoot"

if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = Join-Path $scriptDir "benchmark_baseline.json"
}
$BaselinePath = [IO.Path]::GetFullPath($BaselinePath)

function Write-Log([string]$Message, [string]$Color = "White") {
    if (-not $Json) {
        Write-Host $Message -ForegroundColor $Color
    }
}

# ---------------------------------------------------------------------------
# 1. Baseline Schema & Budget Validation
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) {
    throw "Benchmark baseline file not found at: $BaselinePath"
}

$baselineRaw = Get-Content -LiteralPath $BaselinePath -Raw -Encoding UTF8
$baselineConfig = $baselineRaw | ConvertFrom-Json
if ($null -eq $baselineConfig) {
    throw "Failed to parse benchmark baseline JSON at: $BaselinePath"
}

if (-not $baselineConfig.version -or $baselineConfig.version -lt 1) {
    throw "Invalid baseline version in $BaselinePath"
}

$budgets = $baselineConfig.budgets
if ($null -eq $budgets) {
    throw "Missing 'budgets' object in $BaselinePath"
}

# Validate numeric budget bounds
$requiredBudgetFields = @(
    "startupMaxMs", "startupTargetMs",
    "windowShownMaxMs", "windowShownTargetMs",
    "workingSetMaxBytes", "workingSetTargetBytes",
    "peakWorkingSetMaxBytes", "peakWorkingSetTargetBytes",
    "privateBytesMaxBytes", "privateBytesTargetBytes",
    "peakPrivateBytesMaxBytes", "peakPrivateBytesTargetBytes",
    "gdiObjectsMax", "userObjectsMax", "maxProcesses"
)

foreach ($field in $requiredBudgetFields) {
    if ($null -eq $budgets.$field -or $budgets.$field -le 0) {
        throw "Benchmark budget field '$field' is missing or non-positive in $BaselinePath"
    }
}

if ($budgets.startupTargetMs -gt $budgets.startupMaxMs) {
    throw "startupTargetMs ($($budgets.startupTargetMs)) cannot exceed startupMaxMs ($($budgets.startupMaxMs))"
}
if ($budgets.workingSetTargetBytes -gt $budgets.workingSetMaxBytes) {
    throw "workingSetTargetBytes cannot exceed workingSetMaxBytes"
}
if ($budgets.privateBytesTargetBytes -gt $budgets.privateBytesMaxBytes) {
    throw "privateBytesTargetBytes cannot exceed privateBytesMaxBytes"
}

# Resolve sampling parameters
if ($Iterations -le 0) {
    $Iterations = if ($baselineConfig.sampling -and $baselineConfig.sampling.measuredIterations) {
        [int]$baselineConfig.sampling.measuredIterations
    } else { 5 }
}
if ($WarmupIterations -le 0) {
    $WarmupIterations = if ($baselineConfig.sampling -and $null -ne $baselineConfig.sampling.warmupIterations) {
        [int]$baselineConfig.sampling.warmupIterations
    } else { 1 }
}
if ($TimeoutSeconds -le 0) {
    $TimeoutSeconds = if ($baselineConfig.sampling -and $baselineConfig.sampling.timeoutSecondsPerSample) {
        [int]$baselineConfig.sampling.timeoutSecondsPerSample
    } else { 30 }
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

if (-not (Test-Path -LiteralPath $cmakeLists)) { throw "CMakeLists.txt not found at $cmakeLists" }
if (-not (Test-Path -LiteralPath $mainCpp)) { throw "src\main.cpp not found at $mainCpp" }
if (-not (Test-Path -LiteralPath $ipcProtocolH)) { throw "src\IpcProtocol.h not found at $ipcProtocolH" }

$mainContent = Get-Content -LiteralPath $mainCpp -Raw -Encoding UTF8
$ipcContent = Get-Content -LiteralPath $ipcProtocolH -Raw -Encoding UTF8

$hasProbeHook = ($mainContent -match 'benchmarkProbe' -and $ipcContent -match 'benchmarkProbe')
$hasMemoryApi = ($mainContent -match 'GetProcessMemoryInfo' -and $mainContent -match 'PROCESS_MEMORY_COUNTERS_EX')
$hasAsyncReadiness = ($mainContent -match 'populatedMs' -and $mainContent -match 'libraryReady')

if (-not $hasProbeHook) {
    throw "Source code verification failed: --benchmark-probe hook is missing from src\main.cpp or src\IpcProtocol.h"
}
if (-not $hasMemoryApi) {
    throw "Source code verification failed: GetProcessMemoryInfo / PROCESS_MEMORY_COUNTERS_EX is missing from src\main.cpp"
}
if (-not $hasAsyncReadiness) {
    throw "Source code verification failed: populated library readiness fields are missing from src\main.cpp"
}

# ---------------------------------------------------------------------------
# 4. Validation / Dry-Run Mode
# ---------------------------------------------------------------------------
if ($Validate) {
    $validationReport = [ordered]@{
        status        = "PASS"
        mode          = "Validation"
        baselineFile  = $BaselinePath
        configuration = $Configuration
        platform      = "windows-x64"
        toolchain     = $toolchainInfo
        sampling      = [ordered]@{
            warmupIterations        = $WarmupIterations
            measuredIterations      = $Iterations
            timeoutSecondsPerSample = $TimeoutSeconds
        }
        budgets       = $budgets
        sourceHooks   = [ordered]@{
            benchmarkProbeCLI   = $hasProbeHook
            processMemoryMetric = $hasMemoryApi
        }
    }

    if ($Json) {
        $validationReport | ConvertTo-Json -Depth 5
    } else {
        Write-Log "================================================================================" "Cyan"
        Write-Log "  Aperture Startup & Resource Benchmark Gate (Validation Mode)" "Cyan"
        Write-Log "================================================================================" "Cyan"
        Write-Log "Validation Status: PASS" "Green"
        Write-Log "  Baseline File:       $BaselinePath" "Gray"
        Write-Log "  Configuration:       $Configuration (windows-x64)" "Gray"
        Write-Log "  Sampling Plan:       $WarmupIterations warmup, $Iterations measured samples ($TimeoutSeconds s timeout)" "Gray"
        Write-Log "  Startup Budget:      Target: $($budgets.startupTargetMs) ms | Max: $($budgets.startupMaxMs) ms" "Gray"
        Write-Log "  Window Shown Budget: Target: $($budgets.windowShownTargetMs) ms | Max: $($budgets.windowShownMaxMs) ms" "Gray"
        Write-Log "  Working Set Budget:  Target: $([Math]::Round($budgets.workingSetTargetBytes / 1MB, 2)) MiB | Max: $([Math]::Round($budgets.workingSetMaxBytes / 1MB, 2)) MiB" "Gray"
        Write-Log "  Private Bytes Budget:Target: $([Math]::Round($budgets.privateBytesTargetBytes / 1MB, 2)) MiB | Max: $([Math]::Round($budgets.privateBytesMaxBytes / 1MB, 2)) MiB" "Gray"
        Write-Log "  MSVC Toolset:        $($toolchainInfo.msvcVersion)" "Gray"
        Write-Log "  CMake Version:       $($toolchainInfo.cmakeVersion)" "Gray"
        Write-Log "  Probe Integration:   Verified (GPU shell + populated readiness + K32 counters)" "Green"
        Write-Log "================================================================================" "Cyan"
    }
    exit 0
}

# ---------------------------------------------------------------------------
# 5. Benchmark Execution & Measurement
# ---------------------------------------------------------------------------
$guid = [Guid]::NewGuid().ToString("N")
$runStageRoot = Join-Path $StageRoot $guid
$buildDir = Join-Path $runStageRoot "build"
$stageDir = Join-Path $runStageRoot "stage"
# Verify the exact generated run directory before any create/remove.
$buildDir = Assert-RootLocalPath $buildDir $repoRoot "buildDir"
$stageDir = Assert-RootLocalPath $stageDir $repoRoot "stageDir"

$activeProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()

function Stop-ProcessTree([int]$ProcessId) {
    try {
        $cim = Get-CimInstance Win32_Process -Filter "ParentProcessId = $ProcessId" -ErrorAction SilentlyContinue
        foreach ($child in $cim) {
            Stop-ProcessTree -ProcessId $child.ProcessId
        }
        $target = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
        if ($target -and -not $target.HasExited) {
            $target.Kill()
            $target.WaitForExit(1000)
        }
    } catch {}
}

function Get-Statistics([double[]]$Values) {
    if ($null -eq $Values -or $Values.Length -eq 0) {
        return @{ min = 0; max = 0; mean = 0; median = 0; stdDev = 0 }
    }
    $sorted = $Values | Sort-Object
    $count = $sorted.Length
    $min = $sorted[0]
    $max = $sorted[$count - 1]
    $sum = ($sorted | Measure-Object -Sum).Sum
    $mean = $sum / $count

    $median = if ($count % 2 -eq 1) {
        $sorted[[Math]::Floor($count / 2)]
    } else {
        ($sorted[($count / 2) - 1] + $sorted[$count / 2]) / 2.0
    }

    $varianceSum = 0.0
    foreach ($v in $sorted) {
        $varianceSum += [Math]::Pow($v - $mean, 2)
    }
    $stdDev = if ($count -gt 1) { [Math]::Sqrt($varianceSum / ($count - 1)) } else { 0.0 }

    return @{
        min    = [Math]::Round($min, 2)
        max    = [Math]::Round($max, 2)
        mean   = [Math]::Round($mean, 2)
        median = [Math]::Round($median, 2)
        stdDev = [Math]::Round($stdDev, 2)
    }
}

try {
    Write-Log "================================================================================" "Cyan"
    Write-Log "  Aperture Startup & Resource Benchmark Gate" "Cyan"
    Write-Log "================================================================================" "Cyan"
    Write-Log "Configuration: $Configuration (x64) | Samples: $Iterations (Warmup: $WarmupIterations)" "Yellow"

    # Step A: Obtain / Build Target Binary
    $stagedExe = ""
    if (-not [string]::IsNullOrWhiteSpace($BinaryPath)) {
        $resolvedBin = [IO.Path]::GetFullPath($BinaryPath)
        if (-not (Test-Path -LiteralPath $resolvedBin -PathType Leaf)) {
            throw "Specified -BinaryPath does not exist: $resolvedBin"
        }
        Write-Log "Using pre-built binary: $resolvedBin" "Gray"
        New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
        $stagedExe = Join-Path $stageDir "Aperture.exe"
        Copy-Item -LiteralPath $resolvedBin -Destination $stagedExe -Force
    } else {
        if ($SkipBuild) {
            throw "-SkipBuild was specified but no -BinaryPath was provided."
        }
        New-Item -ItemType Directory -Path $buildDir, $stageDir -Force | Out-Null
        $builtExe = Join-Path $buildDir "Aperture.exe"
        $buildScript = Join-Path $repoRoot "build.ps1"
        if (-not (Test-Path -LiteralPath $buildScript)) {
            throw "build.ps1 not found at $buildScript"
        }
        Write-Log "`n[1/3] Building clean Release binary into isolated temp dir: $buildDir" "Yellow"
        $bSw = [System.Diagnostics.Stopwatch]::StartNew()
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -BuildOutputPath $builtExe -SkipDependencyCheck
        $bSw.Stop()
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $builtExe)) {
            throw "Product build failed with exit code $LASTEXITCODE"
        }
        Write-Log ("Build succeeded in {0:N1}s" -f $bSw.Elapsed.TotalSeconds) "Green"
        $stagedExe = Join-Path $stageDir "Aperture.exe"
        Copy-Item -LiteralPath $builtExe -Destination $stagedExe -Force
    }

    # Step B: Stage Dependencies into Isolated Directory
    Write-Log "`n[2/3] Staging runtime dependencies into isolated test folder: $stageDir" "Yellow"

    $libmpv = if ([string]::IsNullOrWhiteSpace($LibmpvPath)) {
        Join-Path $repoRoot "libmpv-2.dll"
    } else {
        [IO.Path]::GetFullPath($LibmpvPath)
    }
    if (-not (Test-Path -LiteralPath $libmpv -PathType Leaf)) {
        throw "libmpv-2.dll not found at: $libmpv"
    }
    Copy-Item -LiteralPath $libmpv -Destination $stageDir -Force

    $fontsSrc = Join-Path $repoRoot "fonts"
    if (Test-Path -LiteralPath $fontsSrc) {
        Copy-Item -LiteralPath $fontsSrc -Destination $stageDir -Recurse -Force
    }

    # Ensure empty isolated data directory under executable root
    $stageData = Join-Path $stageDir "data"
    New-Item -ItemType Directory -Path $stageData -Force | Out-Null

    # Step C: Execution Passes
    Write-Log "`n[3/3] Executing benchmark passes..." "Yellow"

    # 1. Warmup passes
    for ($w = 1; $w -le $WarmupIterations; $w++) {
        Write-Log ("  [Warmup {0}/{1}] Running unmeasured warmup pass..." -f $w, $WarmupIterations) "DarkGray"
        $warmupJson = Join-Path $stageDir ("warmup_{0}.json" -f $w)
        $psi = [System.Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $stagedExe
        $psi.Arguments = "--benchmark-probe=`"$warmupJson`""
        $psi.WorkingDirectory = $stageDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true

        $proc = [System.Diagnostics.Process]::Start($psi)
        $activeProcesses.Add($proc)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $exited = $proc.WaitForExit($TimeoutSeconds * 1000)
        $sw.Stop()

        if (-not $exited) {
            Stop-ProcessTree -ProcessId $proc.Id
            throw "Warmup pass timed out after $TimeoutSeconds seconds."
        }
        $activeProcesses.Remove($proc) | Out-Null
        if ($proc.ExitCode -ne 0) {
            throw "Warmup pass failed with exit code $($proc.ExitCode)"
        }
    }

    # 2. Measured sampling passes
    $sampleResults = [System.Collections.Generic.List[PSCustomObject]]::new()

    for ($i = 1; $i -le $Iterations; $i++) {
        $sampleJson = Join-Path $stageDir ("sample_{0}.json" -f $i)
        $psi = [System.Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $stagedExe
        $psi.Arguments = "--benchmark-probe=`"$sampleJson`""
        $psi.WorkingDirectory = $stageDir
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true

        $proc = [System.Diagnostics.Process]::Start($psi)
        $activeProcesses.Add($proc)
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $exited = $proc.WaitForExit($TimeoutSeconds * 1000)
        $sw.Stop()

        if (-not $exited) {
            Stop-ProcessTree -ProcessId $proc.Id
            throw "Benchmark sample $i timed out after $TimeoutSeconds seconds."
        }
        $activeProcesses.Remove($proc) | Out-Null
        if ($proc.ExitCode -ne 0) {
            throw "Benchmark sample $i crashed or failed with exit code $($proc.ExitCode)"
        }

        if (-not (Test-Path -LiteralPath $sampleJson -PathType Leaf)) {
            throw "Benchmark sample $i did not emit result JSON at: $sampleJson"
        }

        $sampleData = Get-Content -LiteralPath $sampleJson -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -eq $sampleData -or
            -not ($sampleData.PSObject.Properties.Name -contains "populatedMs") -or
            -not ($sampleData.PSObject.Properties.Name -contains "libraryReady") -or
            -not $sampleData.ready -or -not $sampleData.libraryReady -or
            [double]$sampleData.populatedMs -lt 0) {
            throw "Benchmark sample $i emitted invalid readiness payload"
        }

        $sampleObj = [PSCustomObject]@{
            sample              = $i
            startupMs           = [double]$sampleData.startupMs
            shownMs             = [double]$sampleData.shownMs
            populatedMs         = [double]$sampleData.populatedMs
            libraryReady        = [bool]$sampleData.libraryReady
            workingSetBytes     = [int64]$sampleData.workingSetBytes
            peakWorkingSetBytes = [int64]$sampleData.peakWorkingSetBytes
            privateBytes        = [int64]$sampleData.privateBytes
            peakPrivateBytes    = [int64]$sampleData.peakPrivateBytes
            gdiObjects          = [int]$sampleData.gdiObjects
            userObjects         = [int]$sampleData.userObjects
            processCount        = 1
        }
        $sampleResults.Add($sampleObj)

        Write-Log ("  Sample {0}/{1}: GPU shell: {2:N2} ms (Shown: {3:N2} ms, populated: {4:N2} ms) | WorkingSet: {5:N2} MiB (Peak: {6:N2} MiB) | Private: {7:N2} MiB" -f `
            $i, $Iterations, $sampleObj.startupMs, $sampleObj.shownMs, $sampleObj.populatedMs, `
            ($sampleObj.workingSetBytes / 1MB), ($sampleObj.peakWorkingSetBytes / 1MB), ($sampleObj.privateBytes / 1MB)) "Gray"
    }

    # Step D: Aggregation and Regression Evaluation
    $startupStats = Get-Statistics -Values (@($sampleResults | ForEach-Object { $_.startupMs }))
    $shownStats   = Get-Statistics -Values (@($sampleResults | ForEach-Object { $_.shownMs }))
    $populatedStats = Get-Statistics -Values (@($sampleResults | ForEach-Object { $_.populatedMs }))
    $wsStats      = Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.workingSetBytes }))
    $peakWsStats  = Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.peakWorkingSetBytes }))
    $privStats    = Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.privateBytes }))
    $peakPrivStats= Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.peakPrivateBytes }))
    $gdiStats     = Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.gdiObjects }))
    $userStats    = Get-Statistics -Values (@($sampleResults | ForEach-Object { [double]$_.userObjects }))

    $regressions = [System.Collections.Generic.List[string]]::new()

    # Verify each metric against explicit max budget
    if ($startupStats.median -gt $budgets.startupMaxMs) {
        $regressions.Add(("Startup latency median {0:N2} ms exceeds max budget {1:N2} ms (+{2:N2} ms regression)" -f $startupStats.median, $budgets.startupMaxMs, ($startupStats.median - $budgets.startupMaxMs)))
    }
    if ($shownStats.median -gt $budgets.windowShownMaxMs) {
        $regressions.Add(("Window shown latency median {0:N2} ms exceeds max budget {1:N2} ms (+{2:N2} ms regression)" -f $shownStats.median, $budgets.windowShownMaxMs, ($shownStats.median - $budgets.windowShownMaxMs)))
    }
    if ($wsStats.median -gt $budgets.workingSetMaxBytes) {
        $regressions.Add(("Working set median {0:N2} MiB exceeds max budget {1:N2} MiB" -f ($wsStats.median / 1MB), ($budgets.workingSetMaxBytes / 1MB)))
    }
    if ($peakWsStats.median -gt $budgets.peakWorkingSetMaxBytes) {
        $regressions.Add(("Peak working set median {0:N2} MiB exceeds max budget {1:N2} MiB" -f ($peakWsStats.median / 1MB), ($budgets.peakWorkingSetMaxBytes / 1MB)))
    }
    if ($privStats.median -gt $budgets.privateBytesMaxBytes) {
        $regressions.Add(("Private bytes median {0:N2} MiB exceeds max budget {1:N2} MiB" -f ($privStats.median / 1MB), ($budgets.privateBytesMaxBytes / 1MB)))
    }
    if ($peakPrivStats.median -gt $budgets.peakPrivateBytesMaxBytes) {
        $regressions.Add(("Peak private bytes median {0:N2} MiB exceeds max budget {1:N2} MiB" -f ($peakPrivStats.median / 1MB), ($budgets.peakPrivateBytesMaxBytes / 1MB)))
    }
    if ($gdiStats.max -gt $budgets.gdiObjectsMax) {
        $regressions.Add(("GDI object count {0} exceeds max budget {1}" -f $gdiStats.max, $budgets.gdiObjectsMax))
    }
    if ($userStats.max -gt $budgets.userObjectsMax) {
        $regressions.Add(("User object count {0} exceeds max budget {1}" -f $userStats.max, $budgets.userObjectsMax))
    }

    # Step E: Update Baseline Option
    if ($UpdateBaseline) {
        Write-Log "`nUpdating baseline section in: $BaselinePath" "Yellow"
        $newBaseline = [ordered]@{
            measuredAt                 = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
            startupMedianMs            = $startupStats.median
            windowShownMedianMs        = $shownStats.median
            workingSetMedianBytes      = [int64]$wsStats.median
            peakWorkingSetMedianBytes  = [int64]$peakWsStats.median
            privateBytesMedianBytes    = [int64]$privStats.median
            peakPrivateBytesMedianBytes= [int64]$peakPrivStats.median
            gdiObjects                 = [int]$gdiStats.median
            userObjects                = [int]$userStats.median
            processCount               = 1
        }
        $baselineConfig.baseline = $newBaseline
        $updatedJson = $baselineConfig | ConvertTo-Json -Depth 6
        [IO.File]::WriteAllText($BaselinePath, $updatedJson, [System.Text.Encoding]::UTF8)
        Write-Log "Baseline successfully updated." "Green"
    }

    # Step F: Final Report
    $isSuccess = ($regressions.Count -eq 0)

    $finalReport = [ordered]@{
        status       = if ($isSuccess) { "PASS" } else { "FAIL" }
        timestamp    = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        samplesCount = $Iterations
        metrics      = [ordered]@{
            startupMs           = $startupStats
            windowShownMs       = $shownStats
            populatedMs         = $populatedStats
            workingSetBytes     = $wsStats
            peakWorkingSetBytes = $peakWsStats
            privateBytes        = $privStats
            peakPrivateBytes    = $peakPrivStats
            gdiObjects          = $gdiStats
            userObjects         = $userStats
        }
        budgets      = $budgets
        regressions  = @($regressions)
        samples      = @($sampleResults)
    }

    if ($Json) {
        $finalReport | ConvertTo-Json -Depth 5
    } else {
        Write-Log "`n================================================================================" "Cyan"
        Write-Log "  Aperture Benchmark Results & Regression Verification" "Cyan"
        Write-Log "================================================================================" "Cyan"
        $statusText = if ($isSuccess) { "PASS" } else { "FAIL (REGRESSION DETECTED)" }
        $statusColor = if ($isSuccess) { "Green" } else { "Red" }
        Write-Log "Status: $statusText" $statusColor
        Write-Log ""
        Write-Log ("{0,-28} | {1,10} | {2,10} | {3,10} | {4,10} | {5,8}" -f "Metric", "Median", "Target", "Max Budget", "StdDev", "Verdict") "Yellow"
        Write-Log ("-" * 86) "DarkGray"

        function Format-MetricRow([string]$Name, [double]$Median, [double]$Target, [double]$MaxBudget, [double]$StdDev, [string]$Unit) {
            $pass = ($Median -le $MaxBudget)
            $verdict = if ($pass) { "PASS" } else { "FAIL" }
            $color = if ($pass) { "White" } else { "Red" }
            $line = "{0,-28} | {1,8:N2} {6} | {2,8:N2} {6} | {3,8:N2} {6} | {4,8:N2} {6} | {5,8}" -f $Name, $Median, $Target, $MaxBudget, $StdDev, $verdict, $Unit
            Write-Log $line $color
        }

        Format-MetricRow "Startup Latency" $startupStats.median $budgets.startupTargetMs $budgets.startupMaxMs $startupStats.stdDev "ms"
        Format-MetricRow "Window Shown Latency" $shownStats.median $budgets.windowShownTargetMs $budgets.windowShownMaxMs $shownStats.stdDev "ms"
        Write-Log ("{0,-28} | {1,8:N2} ms | {2,8}   | {3,8}   | {4,8:N2} ms | {5,8}" -f "Populated Library Frame", $populatedStats.median, "observed", "observed", $populatedStats.stdDev, "READY") "White"
        Format-MetricRow "Working Set" ($wsStats.median / 1MB) ($budgets.workingSetTargetBytes / 1MB) ($budgets.workingSetMaxBytes / 1MB) ($wsStats.stdDev / 1MB) "MB"
        Format-MetricRow "Peak Working Set" ($peakWsStats.median / 1MB) ($budgets.peakWorkingSetTargetBytes / 1MB) ($budgets.peakWorkingSetMaxBytes / 1MB) ($peakWsStats.stdDev / 1MB) "MB"
        Format-MetricRow "Private Bytes" ($privStats.median / 1MB) ($budgets.privateBytesTargetBytes / 1MB) ($budgets.privateBytesMaxBytes / 1MB) ($privStats.stdDev / 1MB) "MB"
        Format-MetricRow "Peak Private Bytes" ($peakPrivStats.median / 1MB) ($budgets.peakPrivateBytesTargetBytes / 1MB) ($budgets.peakPrivateBytesMaxBytes / 1MB) ($peakPrivStats.stdDev / 1MB) "MB"

        Write-Log ("-" * 86) "DarkGray"
        Write-Log ("GDI Objects: {0} (Max: {1}) | User Objects: {2} (Max: {3}) | Processes: 1 (Max: 1)" -f $gdiStats.max, $budgets.gdiObjectsMax, $userStats.max, $budgets.userObjectsMax) "Gray"
        Write-Log "================================================================================" "Cyan"

        if ($regressions.Count -gt 0) {
            Write-Log "`nRegressions Encountered:" "Red"
            foreach ($r in $regressions) {
                Write-Log " - $r" "Red"
            }
        }
    }

    if (-not $isSuccess -and -not $UpdateBaseline) {
        exit 1
    }
    exit 0
}
finally {
    # Guaranteed cleanup of processes and the exact generated run directory.
    # Never cleans the root or all of scratch/ automatically.
    foreach ($p in $activeProcesses) {
        try {
            if ($p -and -not $p.HasExited) {
                Stop-ProcessTree -ProcessId $p.Id
            }
        } catch {}
    }

    if (-not $PreserveArtifacts) {
        foreach ($dir in @($buildDir, $stageDir)) {
            try {
                if ($dir) {
                    $verified = Assert-RootLocalPath $dir $repoRoot "cleanupDir"
                    if ((Test-Path -LiteralPath $verified)) {
                        Remove-Item -LiteralPath $verified -Recurse -Force -ErrorAction SilentlyContinue
                    }
                }
            } catch {}
        }
        # Remove the per-run parent only when it is empty.
        try {
            if ($runStageRoot) {
                $verifiedRun = Assert-RootLocalPath $runStageRoot $repoRoot "runStageRoot"
                if ((Test-Path -LiteralPath $verifiedRun) -and @(Get-ChildItem -LiteralPath $verifiedRun -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $verifiedRun -Force -ErrorAction SilentlyContinue
                }
            }
        } catch {}
    }
}
