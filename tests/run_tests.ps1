<#
.SYNOPSIS
    Central test runner and suite orchestrator for Aperture.

.DESCRIPTION
    Loads the authoritative test suite inventory from tests/suites.json,
    validates manifest integrity (unique IDs, repository-bounded paths,
    valid tiers, artifact cleanup declarations), and executes selected test
    suites with bounded process execution, deadlock-safe concurrent stream
    reading, enforced per-suite timeouts, process tree termination on timeout,
    aggregate or fail-fast reporting, and clear exit codes.

.PARAMETER Id
    One or more suite IDs to run (e.g. -Id unit,persistence). Aliases: -Suite, -Suites.

.PARAMETER Tier
    Filter suites by CI tier (e.g. pr, package, fuzz, manual, all). Defaults to 'pr' when no selection is made.

.PARAMETER Category
    Filter suites by category (e.g. unit, persistence, ipc, loader, diagnostics, package, fuzz, hardening, static_analysis, benchmark).

.PARAMETER List
    List all registered test suites in the inventory with their metadata.

.PARAMETER Json
    Emit output as JSON (compatible with -List and test run results).

.PARAMETER ValidateOnly
    Validate manifest schema and test suite file existence without running tests.

.PARAMETER FailFast
    Stop execution immediately upon encountering the first failed or timed out test suite.

.PARAMETER PassThruArgs
    Additional arguments passed through directly to underlying test scripts.

.PARAMETER ManifestPath
    Path to the test suites manifest JSON (defaults to tests/suites.json).

.EXAMPLE
    .\tests\run_tests.ps1 -List

.EXAMPLE
    .\tests\run_tests.ps1 -Tier pr

.EXAMPLE
    .\tests\run_tests.ps1 -Id unit,ipc

.EXAMPLE
    .\tests\run_tests.ps1 -ValidateOnly
#>

[CmdletBinding(DefaultParameterSetName = "Run")]
param(
    [Parameter(Position = 0, ParameterSetName = "Run")]
    [Alias("Suite", "Suites")]
    [string[]]$Id = @(),

    [Parameter(ParameterSetName = "Run")]
    [Alias("CiTier")]
    [string]$Tier = "",

    [Parameter(ParameterSetName = "Run")]
    [string]$Category = "",

    [Parameter(ParameterSetName = "List")]
    [switch]$List,

    [Parameter(ParameterSetName = "Run")]
    [Parameter(ParameterSetName = "List")]
    [switch]$Json,

    [Parameter(ParameterSetName = "Validate")]
    [switch]$ValidateOnly,

    [Parameter(ParameterSetName = "Run")]
    [switch]$FailFast,

    [Parameter(ParameterSetName = "Run")]
    [string[]]$PassThruArgs = @(),

    [string]$ManifestPath = ""
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Path Resolution
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir "..")).TrimEnd('\', '/')

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $scriptDir "suites.json"
}
$ManifestPath = [IO.Path]::GetFullPath($ManifestPath)

# ---------------------------------------------------------------------------
# Manifest Validation & Loading
# ---------------------------------------------------------------------------
function Test-SuiteManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Test manifest not found: $Path"
    }

    $rawJson = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $manifest = $rawJson | ConvertFrom-Json
    if ($null -eq $manifest) {
        throw "Test manifest could not be parsed as valid JSON: $Path"
    }

    if (-not $manifest.version -or $manifest.version -lt 1) {
        throw "Test manifest missing valid version number."
    }

    $validTiers = @($manifest.validTiers)
    if ($validTiers.Count -eq 0) {
        $validTiers = @("pr", "package", "fuzz", "manual")
    }

    $validCategories = @($manifest.validCategories)
    if ($validCategories.Count -eq 0) {
        $validCategories = @("unit", "persistence", "ipc", "loader", "diagnostics", "package", "fuzz", "hardening", "static_analysis", "benchmark")
    }

    $suites = @($manifest.suites)
    if ($suites.Count -eq 0) {
        throw "Test manifest contains no test suites."
    }

    $seenIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $errors = [System.Collections.Generic.List[string]]::new()

    foreach ($suite in $suites) {
        $id = $suite.id
        if ([string]::IsNullOrWhiteSpace($id)) {
            $errors.Add("Suite entry missing required 'id' field.")
            continue
        }

        if (-not $seenIds.Add($id)) {
            $errors.Add("Duplicate suite id detected: '$id'")
        }

        if ([string]::IsNullOrWhiteSpace($suite.name)) {
            $errors.Add("Suite '$id' missing 'name' field.")
        }

        if ([string]::IsNullOrWhiteSpace($suite.category)) {
            $errors.Add("Suite '$id' missing 'category' field.")
        } elseif (-not ($validCategories -contains $suite.category)) {
            $errors.Add("Suite '$id' has invalid category '$($suite.category)'. Valid categories: $($validCategories -join ', ')")
        }

        if ([string]::IsNullOrWhiteSpace($suite.tier)) {
            $errors.Add("Suite '$id' missing 'tier' field.")
        } elseif (-not ($validTiers -contains $suite.tier)) {
            $errors.Add("Suite '$id' has invalid tier '$($suite.tier)'. Valid tiers: $($validTiers -join ', ')")
        }

        if ([string]::IsNullOrWhiteSpace($suite.artifactPolicy)) {
            $errors.Add("Suite '$id' missing required 'artifactPolicy' cleanup declaration.")
        }

        if ($null -eq $suite.timeoutSeconds -or $suite.timeoutSeconds -le 0) {
            $errors.Add("Suite '$id' missing positive 'timeoutSeconds' field.")
        }

        if ([string]::IsNullOrWhiteSpace($suite.script)) {
            $errors.Add("Suite '$id' missing 'script' path.")
            continue
        }

        $scriptRel = $suite.script -replace '\\', '/'
        # Check for path traversal escaping repo root
        if ($scriptRel.StartsWith("/") -or $scriptRel.StartsWith("\") -or $scriptRel -match '^[a-zA-Z]:') {
            $errors.Add("Suite '$id' script must be a relative repository path, got: '$($suite.script)'")
            continue
        }

        $resolvedScript = [IO.Path]::GetFullPath((Join-Path $Root ($suite.script -replace '/', '\')))
        if (-not $resolvedScript.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -and $resolvedScript -ne $Root) {
            $errors.Add("Suite '$id' script path escapes repository root: '$($suite.script)'")
            continue
        }

        if (-not (Test-Path -LiteralPath $resolvedScript -PathType Leaf)) {
            $errors.Add("Suite '$id' script file does not exist on disk: '$($suite.script)' (resolved: '$resolvedScript')")
        }
    }

    if ($errors.Count -gt 0) {
        $msg = "Manifest validation failed with $($errors.Count) error(s):`n - " + ($errors -join "`n - ")
        throw $msg
    }

    return $manifest
}

# ---------------------------------------------------------------------------
# Bounded Process Execution with Deadlock-Safe Streaming & Tree Kill
# ---------------------------------------------------------------------------
function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [Parameter(Mandatory = $false)]
        [string[]]$ArgumentList = @(),
        [Parameter(Mandatory = $false)]
        [string]$WorkingDirectory = "",
        [Parameter(Mandatory = $false)]
        [int]$TimeoutSeconds = 0
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

    # Safe argument escaping for Windows command-line
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

    # Deadlock-safe asynchronous reading of stdout and stderr
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $timedOut = $false
    $timeoutMs = if ($TimeoutSeconds -gt 0) { $TimeoutSeconds * 1000 } else { [System.Threading.Timeout]::Infinite }

    if (-not $process.WaitForExit($timeoutMs)) {
        $timedOut = $true
        # Process exceeded timeout deadline: terminate entire process tree
        try {
            $killProc = Start-Process -FilePath "taskkill.exe" -ArgumentList @("/PID", "$($process.Id)", "/T", "/F") -NoNewWindow -PassThru -Wait
        } catch {
            try {
                $process.Kill()
            } catch {}
        }
        $process.WaitForExit(3000)
    } else {
        $process.WaitForExit()
    }

    # Ensure stream read tasks finish
    try {
        [System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask), 5000) | Out-Null
    } catch {}

    $stdoutText = if ($stdoutTask.IsCompleted -and -not $stdoutTask.IsFaulted) { $stdoutTask.Result } else { "" }
    $stderrText = if ($stderrTask.IsCompleted -and -not $stderrTask.IsFaulted) { $stderrTask.Result } else { "" }

    if (-not [string]::IsNullOrEmpty($stdoutText)) {
        [Console]::Out.Write($stdoutText)
    }
    if (-not [string]::IsNullOrEmpty($stderrText)) {
        [Console]::Error.Write($stderrText)
    }

    if ($timedOut) {
        Write-Host "`n[TIMEOUT] Suite exceeded timeout limit of ${TimeoutSeconds}s. Process tree (PID: $($process.Id)) killed." -ForegroundColor Red
    }

    $exitCode = if ($timedOut) { -1 } else { $process.ExitCode }
    $process.Dispose()

    return [PSCustomObject]@{
        ExitCode = $exitCode
        TimedOut = $timedOut
    }
}

# ---------------------------------------------------------------------------
# Load & Validate
# ---------------------------------------------------------------------------
try {
    $manifest = Test-SuiteManifest -Path $ManifestPath -Root $repoRoot
} catch {
    Write-Error $_.Exception.Message
    exit 2
}

if ($ValidateOnly) {
    Write-Host ("Manifest validation successful: {0} test suites verified in {1}" -f @($manifest.suites).Count, $ManifestPath) -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------------------
# List Mode
# ---------------------------------------------------------------------------
if ($List) {
    if ($Json) {
        $manifest.suites | ConvertTo-Json -Depth 5
    } else {
        Write-Host "================================================================================" -ForegroundColor Cyan
        Write-Host "  Aperture Test Suites Inventory ($(@($manifest.suites).Count) registered suites)" -ForegroundColor Cyan
        Write-Host "================================================================================" -ForegroundColor Cyan
        Write-Host ""
        
        $table = @($manifest.suites) | ForEach-Object {
            [PSCustomObject]@{
                ID          = $_.id
                Tier        = $_.tier
                Category    = $_.category
                Timeout     = "$($_.timeoutSeconds)s"
                Cleanup     = $_.artifactPolicy
                Script      = $_.script
            }
        }
        $table | Format-Table -AutoSize | Out-String | Write-Host

        Write-Host "Use -Tier <tier> (pr, package, fuzz, manual, all) or -Id <id> to run suites." -ForegroundColor DarkGray
    }
    exit 0
}

# ---------------------------------------------------------------------------
# Suite Selection
# ---------------------------------------------------------------------------
$allSuites = @($manifest.suites)
$selectedSuites = @()

if ($Id.Count -gt 0) {
    # Split comma-separated IDs if provided
    $targetIds = @()
    foreach ($item in $Id) {
        $targetIds += ($item -split ',') | ForEach-Object { $_.Trim() } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    }

    foreach ($targetId in $targetIds) {
        $matched = $allSuites | Where-Object { $_.id -ieq $targetId }
        if ($null -eq $matched) {
            Write-Error "Unknown test suite ID: '$targetId'. Use -List to view all available suites."
            exit 2
        }
        $selectedSuites += $matched
    }
} elseif (-not [string]::IsNullOrWhiteSpace($Tier)) {
    if ($Tier -ieq "all") {
        $selectedSuites = $allSuites
    } else {
        $selectedSuites = @($allSuites | Where-Object { $_.tier -ieq $Tier })
        if ($selectedSuites.Count -eq 0) {
            Write-Error "No test suites found for tier: '$Tier'. Valid tiers: $($manifest.validTiers -join ', ')"
            exit 2
        }
    }
} elseif (-not [string]::IsNullOrWhiteSpace($Category)) {
    $selectedSuites = @($allSuites | Where-Object { $_.category -ieq $Category })
    if ($selectedSuites.Count -eq 0) {
        Write-Error "No test suites found for category: '$Category'. Valid categories: $($manifest.validCategories -join ', ')"
        exit 2
    }
} else {
    # Default to 'pr' tier for standard test execution
    $selectedSuites = @($allSuites | Where-Object { $_.tier -ieq "pr" })
    Write-Host "No suite or tier specified. Running default 'pr' tier suites ($($selectedSuites.Count) suites)." -ForegroundColor Yellow
}

if ($selectedSuites.Count -eq 0) {
    Write-Error "No suites matched the selection criteria."
    exit 2
}

# ---------------------------------------------------------------------------
# Suite Execution
# ---------------------------------------------------------------------------
Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host ("  Aperture Test Runner: Executing {0} suite(s)" -f $selectedSuites.Count) -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

$results = [System.Collections.Generic.List[PSCustomObject]]::new()
$overallTimer = [System.Diagnostics.Stopwatch]::StartNew()
$failedCount = 0
$timeoutCount = 0

# Shared objects live only for this invocation and are removed even on failure.
$previousObjectPool = $env:APERTURE_NATIVE_TEST_OBJECTS
$objectPool = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-test-objects-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $objectPool | Out-Null
$env:APERTURE_NATIVE_TEST_OBJECTS = $objectPool
try {
foreach ($suite in $selectedSuites) {
    $suiteId = $suite.id
    $suiteName = $suite.name
    $suiteTier = $suite.tier
    $suiteCategory = $suite.category
    $suiteTimeout = [int]$suite.timeoutSeconds
    $scriptRel = $suite.script -replace '/', '\'
    $scriptFull = Join-Path $repoRoot $scriptRel
    $commandType = if (-not [string]::IsNullOrWhiteSpace($suite.command)) { $suite.command.ToLowerInvariant() } else { "" }

    Write-Host "`n>>> [Suite: $suiteId] $suiteName (Tier: $suiteTier, Category: $suiteCategory)" -ForegroundColor Cyan
    Write-Host "    Script: $scriptRel | Cleanup: $($suite.artifactPolicy) | Timeout: ${suiteTimeout}s" -ForegroundColor DarkGray

    $suiteArgs = @()
    if ($suite.arguments) {
        $suiteArgs += @($suite.arguments)
    }
    if ($PassThruArgs.Count -gt 0) {
        $suiteArgs += $PassThruArgs
    }

    $suiteTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $exitCode = 1
    $timedOut = $false

    try {
        $ext = [IO.Path]::GetExtension($scriptFull).ToLowerInvariant()
        if ($commandType -eq "python" -or $ext -eq ".py") {
            $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
            if ($null -eq $pythonCmd) {
                throw "Python executable not found in PATH for python test suite '$suiteId'."
            }
            $procResult = Invoke-BoundedProcess -Executable $pythonCmd.Source -ArgumentList (@($scriptFull) + $suiteArgs) -WorkingDirectory $repoRoot -TimeoutSeconds $suiteTimeout
            $exitCode = $procResult.ExitCode
            $timedOut = $procResult.TimedOut
        } elseif ($commandType -eq "cmd" -or $ext -eq ".bat" -or $ext -eq ".cmd") {
            $procResult = Invoke-BoundedProcess -Executable "cmd.exe" -ArgumentList (@("/c", $scriptFull) + $suiteArgs) -WorkingDirectory $repoRoot -TimeoutSeconds $suiteTimeout
            $exitCode = $procResult.ExitCode
            $timedOut = $procResult.TimedOut
        } else {
            # Default to PowerShell invocation
            $procResult = Invoke-BoundedProcess -Executable (Join-Path $PSHOME $(if ($PSVersionTable.PSVersion.Major -ge 6) { "pwsh.exe" } else { "powershell.exe" })) -ArgumentList (@("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scriptFull) + $suiteArgs) -WorkingDirectory $repoRoot -TimeoutSeconds $suiteTimeout
            $exitCode = $procResult.ExitCode
            $timedOut = $procResult.TimedOut
        }
    } catch {
        Write-Host "Execution error in suite '$suiteId': $($_.Exception.Message)" -ForegroundColor Red
        $exitCode = 1
    } finally {
        $suiteTimer.Stop()
    }

    $durationSec = [Math]::Round($suiteTimer.Elapsed.TotalSeconds, 2)
    $passed = ($exitCode -eq 0 -and -not $timedOut)

    if ($passed) {
        Write-Host ("<<< [PASS] {0} ({1}s)" -f $suiteId, $durationSec) -ForegroundColor Green
    } elseif ($timedOut) {
        Write-Host ("<<< [TIMEOUT] {0} (exceeded {1}s limit, elapsed: {2}s)" -f $suiteId, $suiteTimeout, $durationSec) -ForegroundColor Red
        $failedCount++
        $timeoutCount++
    } else {
        Write-Host ("<<< [FAIL] {0} (exit code: {1}, {2}s)" -f $suiteId, $exitCode, $durationSec) -ForegroundColor Red
        $failedCount++
    }

    $results.Add([PSCustomObject]@{
        Id          = $suiteId
        Name        = $suiteName
        Tier        = $suiteTier
        Category    = $suiteCategory
        Passed      = $passed
        TimedOut    = $timedOut
        ExitCode    = $exitCode
        DurationSec = $durationSec
    })

    if (-not $passed -and $FailFast) {
        Write-Warning "FailFast enabled: aborting remaining suites after failure in '$suiteId'."
        break
    }
}

} finally {
    $env:APERTURE_NATIVE_TEST_OBJECTS = $previousObjectPool
    $resolved = [IO.Path]::GetFullPath($objectPool)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-test-objects-[a-f0-9]{32}$') { throw 'Unsafe object pool cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$overallTimer.Stop()
$totalSec = [Math]::Round($overallTimer.Elapsed.TotalSeconds, 2)

# ---------------------------------------------------------------------------
# Summary Output
# ---------------------------------------------------------------------------
Write-Host "`n================================================================================" -ForegroundColor Cyan
Write-Host "  Test Execution Summary" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

if ($Json) {
    [PSCustomObject]@{
        TotalSuites = $selectedSuites.Count
        Executed    = $results.Count
        Passed      = ($results.Count - $failedCount)
        Failed      = $failedCount
        TimedOut    = $timeoutCount
        DurationSec = $totalSec
        Suites      = $results
    } | ConvertTo-Json -Depth 5
} else {
    $summaryTable = $results | ForEach-Object {
        [PSCustomObject]@{
            Suite    = $_.Id
            Tier     = $_.Tier
            Category = $_.Category
            Result   = if ($_.Passed) { "PASS" } elseif ($_.TimedOut) { "TIMEOUT" } else { "FAIL" }
            Duration = "$($_.DurationSec)s"
            ExitCode = $_.ExitCode
        }
    }
    $summaryTable | Format-Table -AutoSize | Out-String | Write-Host

    Write-Host ("Total Suites: {0} | Passed: {1} | Failed: {2} (Timeouts: {3}) | Total Time: {4}s" -f `
        $results.Count, ($results.Count - $failedCount), $failedCount, $timeoutCount, $totalSec) -ForegroundColor $(if ($failedCount -eq 0) { "Green" } else { "Red" })
}

if ($failedCount -gt 0) {
    exit 1
} else {
    exit 0
}
