<#
.SYNOPSIS
    Test runner for Aperture startup and resource benchmark policy validation.

.DESCRIPTION
    Runs deterministic Python benchmark policy and schema verification tests
    (test_benchmark_policy.py) and executes the PowerShell benchmark validation gate
    (scripts/run_benchmark.ps1 -Validate).
#>

[CmdletBinding()]
param(
    [switch]$Validate,
    [switch]$Json,
    [switch]$List,
    [string[]]$PassThruArgs = @()
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir "..\..")).TrimEnd('\', '/')

if ($List) {
    Write-Host "Benchmark Suite Tests:"
    Write-Host " - test_baseline_json_exists_and_parses"
    Write-Host " - test_schema_json_exists_and_parses"
    Write-Host " - test_budgets_consistency_and_bounds"
    Write-Host " - test_baseline_medians_satisfy_budgets"
    Write-Host " - test_sampling_parameters"
    Write-Host " - test_regression_detection_math"
    Write-Host " - test_source_code_probe_hooks"
    Write-Host " - test_cmakelists_psapi_link"
    exit 0
}

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "  Aperture Benchmark Policy & Schema Verification Suite" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

# 1. Run Python Policy Tests
$pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
if (-not $pyCmd) { $pyCmd = Get-Command "python" -ErrorAction SilentlyContinue }

if ($pyCmd) {
    $policyTestPy = Join-Path $scriptDir "test_benchmark_policy.py"
    Write-Host "`n[1/2] Running Python Benchmark Policy Tests..." -ForegroundColor Yellow
    & $pyCmd.Source $policyTestPy
    if ($LASTEXITCODE -ne 0) {
        throw "Python benchmark policy tests failed with exit code $LASTEXITCODE"
    }
    Write-Host "Python policy tests passed." -ForegroundColor Green
} else {
    Write-Warning "Python not found in PATH; skipping Python policy unit tests."
}

# 2. Run PowerShell Benchmark Runner Validation Mode
$benchScript = Join-Path $repoRoot "scripts\run_benchmark.ps1"
if (-not (Test-Path -LiteralPath $benchScript -PathType Leaf)) {
    throw "run_benchmark.ps1 not found at $benchScript"
}

Write-Host "`n[2/2] Running Benchmark Gate Validation Mode (-Validate)..." -ForegroundColor Yellow
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $benchScript -Validate
if ($LASTEXITCODE -ne 0) {
    throw "Benchmark script validation mode failed with exit code $LASTEXITCODE"
}

Write-Host "`nAll benchmark policy and schema verifications passed successfully." -ForegroundColor Green
exit 0
