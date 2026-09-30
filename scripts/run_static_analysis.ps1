<#
.SYNOPSIS
    Runs deterministic MSVC C++17 native static code analysis (/analyze) on Aperture.

.DESCRIPTION
    Discovers the Visual Studio 2022 toolchain (MSVC cl.exe, vcvars64.bat, CMake, Ninja),
    validates prerequisites and source inventory from CMakeLists.txt, and executes MSVC native
    static analysis (/analyze, /analyze:external-, /external:anglebrackets, /external:W0) within an isolated external
    temporary directory with guaranteed cleanup in finally.

    Supports dry-run validation mode (-DryRun / -Validate), warnings-as-errors enforcement
    (-WarningsAsErrors), bounded execution timeout, and structured JSON output.

.PARAMETER DryRun
    Validates toolchain discovery and source inventory, displaying planned CMake configuration
    and compiler flags without running a full analysis build.

.PARAMETER Validate
    Alias for -DryRun to perform environment, toolchain, and source list validation.

.PARAMETER Configuration
    Build configuration to analyze (default: "Release"; supported: "Release", "Debug", "Sanitize").

.PARAMETER WarningsAsErrors
    Treat static analysis warnings as errors (/analyze:WX).

.PARAMETER TimeoutSeconds
    Maximum execution timeout in seconds (default: 300).

.PARAMETER Json
    Emit structured JSON output.

.PARAMETER OutputDir
    Optional custom output directory. When omitted, a unique isolated directory in $env:TEMP
    is used and automatically deleted in finally.

.PARAMETER PreserveArtifacts
    If specified with -OutputDir, skips automatic deletion of intermediate files in finally.

.EXAMPLE
    .\scripts\run_static_analysis.ps1 -DryRun

.EXAMPLE
    .\scripts\run_static_analysis.ps1

.EXAMPLE
    .\scripts\run_static_analysis.ps1 -WarningsAsErrors -Json
#>

[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Validate,
    [string]$Configuration = "Release",
    [switch]$WarningsAsErrors,
    [int]$TimeoutSeconds = 600,
    [switch]$Json,
    [string]$OutputDir = "",
    [switch]$PreserveArtifacts,
    [string]$BaselinePath = ""
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Path Resolution & Workspace Safety
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir "..")).TrimEnd('\', '/')
$cmakeListsPath = Join-Path $repoRoot "CMakeLists.txt"

if (-not (Test-Path -LiteralPath $cmakeListsPath -PathType Leaf)) {
    throw "Authoritative CMakeLists.txt not found at: $cmakeListsPath"
}

$defaultBaseline = Join-Path $scriptDir "static_analysis_baseline.json"
if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = $defaultBaseline
}
$BaselinePath = [IO.Path]::GetFullPath($BaselinePath)
# ---------------------------------------------------------------------------
# Logging Helper
# ---------------------------------------------------------------------------
function Write-Log([string]$Message, [string]$Color = "White") {
    if (-not $Json) {
        Write-Host $Message -ForegroundColor $Color
    }
}

# ---------------------------------------------------------------------------
# Normalization & Baseline Helpers
# ---------------------------------------------------------------------------
function Get-NormalizedWarning {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RawLine,
        [Parameter(Mandatory = $false)]
        [string]$Root = ""
    )

    $trimmed = $RawLine.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return $null
    }

    # Match MSVC warning line format:
    # <file>(<line>[,<col>]) : warning <code(C\d+)>: <message>
    if ($trimmed -match '^(?<file>.+?)\s*\((?<line>\d+)(?:,\d+)?\)\s*:\s*warning\s+(?<code>C\d+)\s*:\s*(?<msg>.+)$') {
        $rawFile = $Matches['file'].Trim()
        $code = $Matches['code'].Trim().ToUpperInvariant()
        $msg = $Matches['msg'].Trim()

        # Normalize file path to repo-relative forward-slash path (e.g. src/Accessibility.cpp)
        $relFile = $rawFile -replace '\\', '/'
        if ($relFile -match '(?i)(?:^|/)(src/.+)$') {
            $relFile = $Matches[1]
        } elseif (-not [string]::IsNullOrWhiteSpace($Root)) {
            $normalizedRoot = ([IO.Path]::GetFullPath($Root)).TrimEnd('\', '/') -replace '\\', '/'
            if ($relFile.StartsWith($normalizedRoot + "/", [System.StringComparison]::OrdinalIgnoreCase)) {
                $relFile = $relFile.Substring($normalizedRoot.Length + 1)
            }
        }

        # Strip volatile trailing analyzer trace coordinates (e.g. ": Lines: 92, 93, 95...")
        $cleanMsg = $msg -replace ':\s*Lines:\s*[\d,\s]+$', ''
        # Normalize secondary source declaration references containing absolute paths / line numbers
        $cleanMsg = $cleanMsg -replace "at line '\d+' of '[^']+'", "at line '<line>' of '<file>'"
        $cleanMsg = $cleanMsg -replace "See [a-zA-Z]:[\\/][^()]+\(\d+\)\.?", "See <sdk_header>."
        $cleanMsg = ($cleanMsg -replace '\s+', ' ').Trim()

        $fp = "${relFile}|${code}|${cleanMsg}"

        return [PSCustomObject]@{
            File        = $relFile
            Code        = $code
            Message     = $cleanMsg
            Fingerprint = $fp
            Raw         = $trimmed
        }
    }

    # Fallback for non-standard warning lines that still contain warning Cxxxx
    if ($trimmed -match 'warning\s+(?<codeDefault>C\d+)\s*:\s*(?<msgDefault>.+)$') {
        $codeDef = $Matches['codeDefault'].Trim().ToUpperInvariant()
        $msgDef = ($Matches['msgDefault'] -replace '\s+', ' ').Trim()
        $fpDef = "unknown|${codeDef}|${msgDef}"
        return [PSCustomObject]@{
            File        = "unknown"
            Code        = $codeDef
            Message     = $msgDef
            Fingerprint = $fpDef
            Raw         = $trimmed
        }
    }

    throw "Encountered warning line with invalid/unparseable structure: '$trimmed'"
}

function Test-StaticAnalysisBaseline {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Static analysis baseline file not found: $Path"
    }

    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $baseline = $raw | ConvertFrom-Json
    if ($null -eq $baseline) {
        throw "Static analysis baseline could not be parsed as valid JSON: $Path"
    }

    if (-not $baseline.version -or $baseline.version -lt 1) {
        throw "Baseline missing valid version number."
    }

    if ($null -eq $baseline.maxTotalWarnings -or $baseline.maxTotalWarnings -lt 0) {
        throw "Baseline missing valid maxTotalWarnings integer."
    }

    $fingerprints = @($baseline.allowedFingerprints)
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $errors = [System.Collections.Generic.List[string]]::new()
    $sumOccurrences = 0

    foreach ($fp in $fingerprints) {
        if ([string]::IsNullOrWhiteSpace($fp.file)) {
            $errors.Add("Baseline entry missing 'file' path.")
            continue
        }
        if ([string]::IsNullOrWhiteSpace($fp.code) -or $fp.code -notmatch '^C\d+$') {
            $errors.Add("Baseline entry has invalid 'code': '$($fp.code)'")
        }
        if ([string]::IsNullOrWhiteSpace($fp.message)) {
            $errors.Add("Baseline entry for $($fp.file) missing 'message'.")
        }
        if ($null -eq $fp.maxOccurrences -or $fp.maxOccurrences -le 0) {
            $errors.Add("Baseline entry for $($fp.file)/$($fp.code) has non-positive maxOccurrences: $($fp.maxOccurrences)")
        }

        $sumOccurrences += [int]$fp.maxOccurrences
        $key = "$($fp.file)|$($fp.code)|$($fp.message)"
        if (-not $seen.Add($key)) {
            $errors.Add("Duplicate baseline fingerprint detected: '$key'")
        }

        $resolvedFile = Join-Path $Root ($fp.file -replace '/', '\')
        if (-not (Test-Path -LiteralPath $resolvedFile -PathType Leaf)) {
            $errors.Add("Baseline references non-existent file: '$($fp.file)'")
        }
    }

    if ($sumOccurrences -ne $baseline.maxTotalWarnings) {
        $errors.Add("Baseline maxTotalWarnings ($($baseline.maxTotalWarnings)) does not equal sum of maxOccurrences ($sumOccurrences).")
    }

    if ($errors.Count -gt 0) {
        throw "Baseline validation failed with $($errors.Count) error(s):`n - " + ($errors -join "`n - ")
    }

    return $baseline
}
# ---------------------------------------------------------------------------
# 1. Toolchain & Environment Discovery
# ---------------------------------------------------------------------------
$toolchain = [ordered]@{
    vsPath          = $null
    vcvars64        = $null
    msvcVersion     = "Unknown"
    cmakeExe        = $null
    cmakeVersion    = "Unknown"
    ninjaExe        = $null
    ninjaVersion    = "Unknown"
    generator       = "Unknown"
}

# Discover Visual Studio 2022 installation
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $toolchain.vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

if (-not $toolchain.vsPath -or -not (Test-Path -LiteralPath $toolchain.vsPath)) {
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($cand in $candidates) {
        if (Test-Path -LiteralPath $cand) {
            $toolchain.vsPath = $cand
            break
        }
    }
}

if ($toolchain.vsPath) {
    $vcvars = Join-Path $toolchain.vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path -LiteralPath $vcvars) {
        $toolchain.vcvars64 = $vcvars
    }
    $vcToolsVerFile = Join-Path $toolchain.vsPath "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
    if (Test-Path -LiteralPath $vcToolsVerFile) {
        $toolchain.msvcVersion = (Get-Content -LiteralPath $vcToolsVerFile -Raw).Trim()
    }
}

# Discover CMake executable
$cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if ($cmakeCmd -and (Test-Path -LiteralPath $cmakeCmd.Source)) {
    $toolchain.cmakeExe = $cmakeCmd.Source
} elseif ($toolchain.vsPath) {
    $bundledCmake = Join-Path $toolchain.vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $bundledCmake) {
        $toolchain.cmakeExe = $bundledCmake
    }
}

if ($toolchain.cmakeExe) {
    try {
        $cmakeVerOutput = & $toolchain.cmakeExe --version 2>$null
        if ($cmakeVerOutput -and ($cmakeVerOutput[0] -match 'cmake version\s+([0-9.]+)')) {
            $toolchain.cmakeVersion = $Matches[1]
        }
    } catch {}
}

# Discover Ninja executable (optional but preferred for fast deterministic builds)
$ninjaCmd = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
if ($ninjaCmd -and (Test-Path -LiteralPath $ninjaCmd.Source)) {
    $toolchain.ninjaExe = $ninjaCmd.Source
} elseif ($toolchain.vsPath) {
    $bundledNinja = Join-Path $toolchain.vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path -LiteralPath $bundledNinja) {
        $toolchain.ninjaExe = $bundledNinja
    }
}

if ($toolchain.ninjaExe) {
    try {
        $ninjaVerOutput = & $toolchain.ninjaExe --version 2>$null
        if ($ninjaVerOutput) {
            $toolchain.ninjaVersion = ($ninjaVerOutput -join "").Trim()
        }
    } catch {}
    $toolchain.generator = "Ninja"
} else {
    $toolchain.generator = "Visual Studio 17 2022"
}

# ---------------------------------------------------------------------------
# 2. Extract Authoritative Source Inventory from CMakeLists.txt
# ---------------------------------------------------------------------------
$cmakeContent = Get-Content -LiteralPath $cmakeListsPath -Raw -Encoding UTF8
$sources = [System.Collections.Generic.List[string]]::new()

if ($cmakeContent -match 'set\s*\(\s*APERTURE_SOURCES\s+([^)]+)\)') {
    $rawBlock = $Matches[1]
    $lines = $rawBlock -split "[\r\n]+"
    foreach ($line in $lines) {
        $trimmed = $line.Trim()
        if (-not [string]::IsNullOrWhiteSpace($trimmed) -and -not $trimmed.StartsWith("#")) {
            $sources.Add($trimmed)
        }
    }
}

$missingSources = [System.Collections.Generic.List[string]]::new()
foreach ($src in $sources) {
    $srcPath = [IO.Path]::GetFullPath((Join-Path $repoRoot ($src -replace '/', '\')))
    if (-not (Test-Path -LiteralPath $srcPath -PathType Leaf)) {
        $missingSources.Add($src)
    }
}

# ---------------------------------------------------------------------------
# 3. Dry-Run / Validation Mode
# ---------------------------------------------------------------------------
$isValidationMode = ($DryRun -or $Validate)

$analysisFlags = @(
    "/analyze",
    "/analyze:external-",
    "/external:anglebrackets",
    "/external:W0"
)
if ($WarningsAsErrors) {
    $analysisFlags += "/analyze:WX"
}

if ($isValidationMode) {
    Write-Log "================================================================================" "Cyan"
    Write-Log "  Aperture Static Analysis Gate: Validation Mode" "Cyan"
    Write-Log "================================================================================" "Cyan"
    Write-Log "Toolchain Discovery:" "Yellow"
    Write-Log "  VS 2022 Path:       $(if ($toolchain.vsPath) { $toolchain.vsPath } else { '<Not Found>' })" "Gray"
    Write-Log "  vcvars64.bat:       $(if ($toolchain.vcvars64) { $toolchain.vcvars64 } else { '<Not Found>' })" "Gray"
    Write-Log "  MSVC Toolset:       $($toolchain.msvcVersion)" "Gray"
    Write-Log "  CMake Executable:   $(if ($toolchain.cmakeExe) { $toolchain.cmakeExe } else { '<Not Found>' }) ($($toolchain.cmakeVersion))" "Gray"
    Write-Log "  Ninja Executable:   $(if ($toolchain.ninjaExe) { $toolchain.ninjaExe } else { '<Not Found, using MSBuild>' }) ($($toolchain.ninjaVersion))" "Gray"
    Write-Log "  Target Generator:   $($toolchain.generator)" "Gray"
    Write-Log "--------------------------------------------------------------------------------" "DarkGray"
    Write-Log "Source Inventory (from CMakeLists.txt):" "Yellow"
    Write-Log "  Total Sources:      $($sources.Count) translation units" "Gray"
    Write-Log "  Missing Sources:    $($missingSources.Count)" "Gray"
    Write-Log "Analysis Configuration:" "Yellow"
    Write-Log "  Configuration:      $Configuration" "Gray"
    Write-Log "  Compiler Flags:     $($analysisFlags -join ' ')" "Gray"
    Write-Log "  WarningsAsErrors:   $([bool]$WarningsAsErrors)" "Gray"
    Write-Log "  Isolation:          External temporary directory in `$env:TEMP (guaranteed cleanup)" "Gray"

    $baselineObj = $null
    $baselineErrors = $null
    try {
        $baselineObj = Test-StaticAnalysisBaseline -Path $BaselinePath -Root $repoRoot
        Write-Log "Baseline Policy:" "Yellow"
        Write-Log "  Baseline File:      $BaselinePath" "Gray"
        Write-Log "  Max Total Warnings: $($baselineObj.maxTotalWarnings)" "Gray"
        Write-Log "  Unique Allowed:     $($baselineObj.allowedFingerprints.Count) fingerprints" "Gray"
    } catch {
        $baselineErrors = $_.Exception.Message
        Write-Log "`n[FAIL] Baseline validation error: $baselineErrors" "Red"
    }

    $valid = ($null -ne $toolchain.vsPath -and
              $null -ne $toolchain.vcvars64 -and
              $null -ne $toolchain.cmakeExe -and
              $missingSources.Count -eq 0 -and
              $null -eq $baselineErrors)

    if ($Json) {
        $jsonObj = [ordered]@{
            gate             = "static_analysis"
            mode             = "validation"
            valid            = $valid
            toolchain        = $toolchain
            configuration    = $Configuration
            analysisFlags    = $analysisFlags
            warningsAsErrors = [bool]$WarningsAsErrors
            sourceCount      = $sources.Count
            missingSources   = @($missingSources)
            baselinePath     = $BaselinePath
            baselineValid    = ($null -eq $baselineErrors)
            maxTotalWarnings = if ($baselineObj) { $baselineObj.maxTotalWarnings } else { $null }
            fingerprintCount = if ($baselineObj) { $baselineObj.allowedFingerprints.Count } else { $null }
        }
        $jsonObj | ConvertTo-Json -Depth 5
    }

    if (-not $valid) {
        if ($missingSources.Count -gt 0) {
            Write-Log "`n[FAIL] Missing source files: $($missingSources -join ', ')" "Red"
        }
        if (-not $toolchain.vsPath -or -not $toolchain.vcvars64) {
            Write-Log "`n[FAIL] Visual Studio 2022 x64 toolchain (vcvars64.bat) not found." "Red"
        }
        if (-not $toolchain.cmakeExe) {
            Write-Log "`n[FAIL] CMake executable not found." "Red"
        }
        exit 1
    }

    Write-Log "`n[PASS] Static analysis gate configuration and toolchain validated successfully." "Green"
    exit 0
}

# ---------------------------------------------------------------------------
# 4. Prerequisite Verification for Execution Mode
# ---------------------------------------------------------------------------
if (-not $toolchain.vsPath -or -not $toolchain.vcvars64) {
    throw "Visual Studio 2022 installation or vcvars64.bat not found. Required for MSVC static analysis."
}
if (-not $toolchain.cmakeExe) {
    throw "CMake executable not found. Ensure CMake is installed or available in PATH / Visual Studio."
}
if ($missingSources.Count -gt 0) {
    throw "CMakeLists.txt references source files that do not exist: $($missingSources -join ', ')"
}

# ---------------------------------------------------------------------------
# 5. Isolated External Temporary Build Directory Setup
# ---------------------------------------------------------------------------
$customOutput = -not [string]::IsNullOrWhiteSpace($OutputDir)
$tempDir = if ($customOutput) {
    [IO.Path]::GetFullPath($OutputDir)
} else {
    Join-Path ([IO.Path]::GetTempPath()) ("Aperture_static_analysis_" + [Guid]::NewGuid().ToString("N"))
}

New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

# ---------------------------------------------------------------------------
# Bounded Process Runner Helper with Deadlock-Safe Streaming & Tree Kill
# ---------------------------------------------------------------------------
function Invoke-BoundedProcessRunner {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $false)]
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $false)]
        [string]$WorkingDirectory = "",
        [Parameter(Mandatory = $false)]
        [int]$TimeoutSec = 0
    )

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Executable
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $psi.WorkingDirectory = $WorkingDirectory
    }

    $escapedArgs = @()
    foreach ($arg in $ArgumentList) {
        if ([string]::IsNullOrEmpty($arg)) {
            $escapedArgs += '""'
        } elseif ($arg -match '[\s"]') {
            $escapedArgs += '"' + ($arg -replace '(\\*)(")', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
        } else {
            $escapedArgs += $arg
        }
    }
    $psi.Arguments = $escapedArgs -join " "

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi

    $started = $process.Start()
    if (-not $started) {
        throw "Failed to start process: $Executable"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $timedOut = $false
    $timeoutMs = if ($TimeoutSec -gt 0) { $TimeoutSec * 1000 } else { [System.Threading.Timeout]::Infinite }

    if (-not $process.WaitForExit($timeoutMs)) {
        $timedOut = $true
        try {
            Start-Process -FilePath "taskkill.exe" -ArgumentList @("/PID", "$($process.Id)", "/T", "/F") -NoNewWindow -PassThru -Wait | Out-Null
        } catch {
            try { $process.Kill() } catch {}
        }
        $process.WaitForExit(3000)
    } else {
        $process.WaitForExit()
    }

    try {
        [System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask), 5000) | Out-Null
    } catch {}

    $stdoutText = if ($stdoutTask.IsCompleted -and -not $stdoutTask.IsFaulted) { $stdoutTask.Result } else { "" }
    $stderrText = if ($stderrTask.IsCompleted -and -not $stderrTask.IsFaulted) { $stderrTask.Result } else { "" }

    $exitCode = if ($timedOut) { -1 } else { $process.ExitCode }
    $process.Dispose()

    return [PSCustomObject]@{
        ExitCode  = $exitCode
        TimedOut  = $timedOut
        StdOut    = $stdoutText
        StdErr    = $stderrText
    }
}

# ---------------------------------------------------------------------------
# 6. Execute Static Analysis Build
# ---------------------------------------------------------------------------
Write-Log "================================================================================" "Cyan"
Write-Log "  Aperture MSVC C++17 Native Static Code Analysis Gate" "Cyan"
Write-Log "================================================================================" "Cyan"
Write-Log "Configuration:     $Configuration" "Gray"
Write-Log "Generator:         $($toolchain.generator)" "Gray"
Write-Log "MSVC Toolset:      $($toolchain.msvcVersion)" "Gray"
Write-Log "Flags:             $($analysisFlags -join ' ')" "Gray"
Write-Log "Staging Directory: $tempDir" "DarkGray"
Write-Log "Timeout:           ${TimeoutSeconds}s" "DarkGray"
Write-Log "--------------------------------------------------------------------------------" "DarkGray"

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$configureSuccess = $false
$buildSuccess = $false
$analysisOutput = ""
$warningsList = [System.Collections.Generic.List[string]]::new()
$errorsList = [System.Collections.Generic.List[string]]::new()

$wxVal = if ($WarningsAsErrors) { "ON" } else { "OFF" }

try {
    # Step A: CMake Configure
    Write-Log "`n[1/2] Configuring CMake with Static Analysis enabled..." "Yellow"

    $configureCmdScript = Join-Path $tempDir "configure.cmd"
    $cmakeGenArgs = if ($toolchain.generator -eq "Ninja" -and $toolchain.ninjaExe) {
        "-G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DAPERTURE_ENABLE_STATIC_ANALYSIS=ON -DAPERTURE_STATIC_ANALYSIS_WX=$wxVal -DCMAKE_MAKE_PROGRAM=`"$($toolchain.ninjaExe)`""
    } else {
        "-G `"Visual Studio 17 2022`" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release;Debug;Sanitize -DCMAKE_BUILD_TYPE=$Configuration -DAPERTURE_ENABLE_STATIC_ANALYSIS=ON -DAPERTURE_STATIC_ANALYSIS_WX=$wxVal"
    }

    $configureBatch = @"
@echo off
setlocal
call "$($toolchain.vcvars64)"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
"$($toolchain.cmakeExe)" -S "$repoRoot" -B "$tempDir" $cmakeGenArgs
exit /b %ERRORLEVEL%
"@
    [IO.File]::WriteAllText($configureCmdScript, $configureBatch, [System.Text.Encoding]::ASCII)

    $configureResult = Invoke-BoundedProcessRunner -Executable "cmd.exe" -ArgumentList @("/d", "/c", $configureCmdScript) -WorkingDirectory $repoRoot -TimeoutSec $TimeoutSeconds

    if ($configureResult.TimedOut) {
        throw "CMake configuration timed out after ${TimeoutSeconds}s."
    }
    if ($configureResult.ExitCode -ne 0) {
        Write-Log $configureResult.StdOut "DarkGray"
        Write-Log $configureResult.StdErr "Red"
        throw "CMake configuration failed with exit code $($configureResult.ExitCode)."
    }
    $configureSuccess = $true
    Write-Log "CMake configuration succeeded." "Green"

    # Step B: CMake Build & Static Analysis Pass
    Write-Log "`n[2/2] Running MSVC Static Analysis (/analyze) across $($sources.Count) translation units..." "Yellow"

    $buildCmdScript = Join-Path $tempDir "build.cmd"
    $buildBatch = @"
@echo off
setlocal
call "$($toolchain.vcvars64)"
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%
"$($toolchain.cmakeExe)" --build "$tempDir" --config $Configuration --target Aperture
exit /b %ERRORLEVEL%
"@
    [IO.File]::WriteAllText($buildCmdScript, $buildBatch, [System.Text.Encoding]::ASCII)

    $buildResult = Invoke-BoundedProcessRunner -Executable "cmd.exe" -ArgumentList @("/d", "/c", $buildCmdScript) -WorkingDirectory $repoRoot -TimeoutSec $TimeoutSeconds
    $combinedOutput = ($buildResult.StdOut + "`n" + $buildResult.StdErr)
    $analysisOutput = $combinedOutput

    # Parse output for compiler/analyzer warnings and errors
    $outputLines = $combinedOutput -split "[\r\n]+"
    foreach ($line in $outputLines) {
        if ($line -match ':\s*warning\s+(C[0-9]+)\s*:\s*(.+)$') {
            $warningsList.Add($line.Trim())
        } elseif ($line -match ':\s*error\s+(C[0-9]+)\s*:\s*(.+)$') {
            $errorsList.Add($line.Trim())
        }
    }

    if ($buildResult.TimedOut) {
        throw "Static analysis build timed out after ${TimeoutSeconds}s."
    }
    if ($buildResult.ExitCode -ne 0) {
        Write-Log $combinedOutput "DarkGray"
        throw "MSVC Static Analysis build failed with exit code $($buildResult.ExitCode) ($($errorsList.Count) errors, $($warningsList.Count) warnings)."
    }

    $buildSuccess = $true
}
finally {
    $sw.Stop()
    $duration = [Math]::Round($sw.Elapsed.TotalSeconds, 2)

    # Guaranteed cleanup of isolated temporary directory
    if (-not $customOutput -or -not $PreserveArtifacts) {
        if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
            Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ---------------------------------------------------------------------------
# 7. Reporting & Final Status
# ---------------------------------------------------------------------------
    # ---------------------------------------------------------------------------
    # Warning Normalization & Baseline Enforcement
    # ---------------------------------------------------------------------------
    $baseline = Test-StaticAnalysisBaseline -Path $BaselinePath -Root $repoRoot

    $parsedWarnings = [System.Collections.Generic.List[PSCustomObject]]::new()
    foreach ($rawWarn in $warningsList) {
        $norm = Get-NormalizedWarning -RawLine $rawWarn -Root $repoRoot
        if ($null -eq $norm -or [string]::IsNullOrWhiteSpace($norm.Fingerprint)) {
            throw "Encountered warning line where fingerprint could not be computed: '$rawWarn'"
        }
        $parsedWarnings.Add($norm)
    }

    $newWarnings = [System.Collections.Generic.List[PSCustomObject]]::new()
    $countIncreases = [System.Collections.Generic.List[PSCustomObject]]::new()
    $allowedWarnings = [System.Collections.Generic.List[PSCustomObject]]::new()
    $resolvedWarnings = [System.Collections.Generic.List[PSCustomObject]]::new()

    # Map baseline by fingerprint key
    $baselineMap = [System.Collections.Generic.Dictionary[string, int]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $baseline.allowedFingerprints) {
        $key = "$($entry.file)|$($entry.code)|$($entry.message)"
        $baselineMap[$key] = [int]$entry.maxOccurrences
    }

    # Group observed warnings by fingerprint
    $observedCounts = [System.Collections.Generic.Dictionary[string, int]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $observedSamples = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    foreach ($w in $parsedWarnings) {
        $key = $w.Fingerprint
        if ([string]::IsNullOrWhiteSpace($key)) {
            throw "Encountered warning object with missing/null fingerprint: $($w.Raw)"
        }
        if (-not $observedCounts.ContainsKey($key)) {
            $observedCounts[$key] = 0
            $observedSamples[$key] = $w.Raw
        }
        $observedCounts[$key] = $observedCounts[$key] + 1
    }

    foreach ($key in @($observedCounts.Keys)) {
        if ([string]::IsNullOrWhiteSpace($key)) {
            continue
        }
        $actualCount = $observedCounts[$key]
        if (-not $baselineMap.ContainsKey($key)) {
            $newWarnings.Add([PSCustomObject]@{
                Fingerprint = $key
                Count       = $actualCount
                Sample      = $observedSamples[$key]
            })
        } else {
            $maxAllowed = $baselineMap[$key]
            if ($actualCount -gt $maxAllowed) {
                $countIncreases.Add([PSCustomObject]@{
                    Fingerprint = $key
                    ActualCount = $actualCount
                    MaxAllowed  = $maxAllowed
                    Sample      = $observedSamples[$key]
                })
            } else {
                $allowedWarnings.Add([PSCustomObject]@{
                    Fingerprint = $key
                    ActualCount = $actualCount
                    MaxAllowed  = $maxAllowed
                })
            }
        }
    }

    # Check for resolved / fixed warnings
    foreach ($key in @($baselineMap.Keys)) {
        if ([string]::IsNullOrWhiteSpace($key)) {
            continue
        }
        $maxAllowed = $baselineMap[$key]
        $actual = if ($observedCounts.ContainsKey($key)) { $observedCounts[$key] } else { 0 }
        if ($actual -lt $maxAllowed) {
            $resolvedWarnings.Add([PSCustomObject]@{
                Fingerprint = $key
                BaselineMax = $maxAllowed
                ActualCount = $actual
                Resolved    = ($maxAllowed - $actual)
            })
        }
    }

    # Check for resolved / fixed warnings
    foreach ($kvp in $baselineMap) {
        $key = $kvp.Key
        $maxAllowed = $kvp.Value
        $actual = if ($observedCounts.ContainsKey($key)) { $observedCounts[$key] } else { 0 }
        if ($actual -lt $maxAllowed) {
            $resolvedWarnings.Add([PSCustomObject]@{
                Fingerprint = $key
                BaselineMax = $maxAllowed
                ActualCount = $actual
                Resolved    = ($maxAllowed - $actual)
            })
        }
    }

    $totalWarningCount = $parsedWarnings.Count
    $totalExceeded = ($totalWarningCount -gt $baseline.maxTotalWarnings)

    $passed = ($configureSuccess -and $buildSuccess -and $errorsList.Count -eq 0)
    if ($WarningsAsErrors) {
        if ($totalWarningCount -gt 0) {
            $passed = $false
        }
    } else {
        if ($newWarnings.Count -gt 0 -or $countIncreases.Count -gt 0 -or $totalExceeded) {
            $passed = $false
        }
    }

    Write-Log "================================================================================" "Cyan"
    Write-Log "  Static Analysis Summary" "Cyan"
    Write-Log "================================================================================" "Cyan"
    Write-Log "Duration:          ${duration}s" "Gray"
    Write-Log "Sources Analyzed:  $($sources.Count)" "Gray"
    Write-Log "Total Warnings:    $totalWarningCount (Baseline: $($baseline.maxTotalWarnings))" $(if ($totalExceeded) { "Red" } elseif ($totalWarningCount -gt 0) { "Yellow" } else { "Green" })
    Write-Log "New Warnings:      $($newWarnings.Count)" $(if ($newWarnings.Count -gt 0) { "Red" } else { "Green" })
    Write-Log "Count Regressions: $($countIncreases.Count)" $(if ($countIncreases.Count -gt 0) { "Red" } else { "Green" })
    Write-Log "Resolved Warnings: $($resolvedWarnings.Count)" $(if ($resolvedWarnings.Count -gt 0) { "Green" } else { "Gray" })
    Write-Log "Analysis Errors:   $($errorsList.Count)" $(if ($errorsList.Count -gt 0) { "Red" } else { "Green" })
    Write-Log "Result:            $(if ($passed) { 'PASS' } else { 'FAIL' })" $(if ($passed) { "Green" } else { "Red" })

    if ($newWarnings.Count -gt 0) {
        Write-Log "`n[REGRESSION] New Warning Fingerprints ($($newWarnings.Count)):" "Red"
        foreach ($nw in $newWarnings) {
            Write-Log "  - [Count: $($nw.Count)] $($nw.Fingerprint)" "Red"
            Write-Log "    Sample: $($nw.Sample)" "DarkGray"
        }
    }

    if ($countIncreases.Count -gt 0) {
        Write-Log "`n[REGRESSION] Warning Count Increases ($($countIncreases.Count)):" "Red"
        foreach ($ci in $countIncreases) {
            Write-Log "  - [Observed: $($ci.ActualCount), Max Allowed: $($ci.MaxAllowed)] $($ci.Fingerprint)" "Red"
            Write-Log "    Sample: $($ci.Sample)" "DarkGray"
        }
    }

    if ($resolvedWarnings.Count -gt 0) {
        Write-Log "`n[RESOLVED] Resolved/Reduced Warnings ($($resolvedWarnings.Count)):" "Green"
        foreach ($rw in $resolvedWarnings) {
            Write-Log "  - [Reduced by $($rw.Resolved): now $($rw.ActualCount), was $($rw.BaselineMax)] $($rw.Fingerprint)" "Green"
        }
    }

    if ($errorsList.Count -gt 0) {
        Write-Log "`nAnalysis Errors ($($errorsList.Count)):" "Red"
        foreach ($e in $errorsList) {
            Write-Log "  $e" "Red"
        }
    }

    if ($Json) {
        $resultObj = [ordered]@{
            gate             = "static_analysis"
            passed           = $passed
            durationSec      = $duration
            configuration    = $Configuration
            warningsAsErrors = [bool]$WarningsAsErrors
            sourcesAnalyzed  = $sources.Count
            totalWarnings    = $totalWarningCount
            baselineMaxTotal = $baseline.maxTotalWarnings
            newWarningCount  = $newWarnings.Count
            increaseCount    = $countIncreases.Count
            resolvedCount    = $resolvedWarnings.Count
            errorCount       = $errorsList.Count
            newWarnings      = @($newWarnings)
            countIncreases   = @($countIncreases)
            resolvedWarnings = @($resolvedWarnings)
            errors           = @($errorsList)
        }
        $resultObj | ConvertTo-Json -Depth 5
    }

if ($passed) {
    exit 0
} else {
    exit 1
}
