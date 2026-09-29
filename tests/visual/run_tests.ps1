<#
.SYNOPSIS
    Test runner for Aperture visual regression gate and policy validation.

.DESCRIPTION
    Runs deterministic Python visual policy and schema verification tests
    (test_visual_policy.py) and executes the PowerShell visual regression validation gate
    (scripts/run_visual_regression.ps1 -Validate).
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
    Write-Host "Visual Regression Suite Tests:"
    Write-Host " - test_baseline_json_exists_and_parses"
    Write-Host " - test_schema_json_exists_and_parses"
    Write-Host " - test_environment_consistency"
    Write-Host " - test_scenes_and_tolerances_bounds"
    Write-Host " - test_baseline_files_exist_and_match_hashes_and_dimensions"
    Write-Host " - test_pure_python_png_decode_encode_roundtrip"
    Write-Host " - test_image_comparison_math_and_thresholds"
    Write-Host " - test_source_code_probe_hooks"
    Write-Host " - test_suites_json_registration"
    exit 0
}

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "  Aperture Visual Regression Policy & Schema Verification Suite" -ForegroundColor Cyan
Write-Host "================================================================================" -ForegroundColor Cyan

# 1. Run Python Policy Tests
$pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
if (-not $pyCmd) { $pyCmd = Get-Command "python" -ErrorAction SilentlyContinue }

if ($pyCmd) {
    $policyTestPy = Join-Path $scriptDir "test_visual_policy.py"
    Write-Host "`n[1/2] Running Python Visual Policy Tests..." -ForegroundColor Yellow
    & $pyCmd.Source $policyTestPy
    if ($LASTEXITCODE -ne 0) {
        throw "Python visual policy tests failed with exit code $LASTEXITCODE"
    }
    Write-Host "Python policy tests passed." -ForegroundColor Green
} else {
    Write-Warning "Python not found in PATH; skipping Python policy unit tests."
}

# 2. Run PowerShell Visual Regression Runner Validation Mode
$visualScript = Join-Path $repoRoot "scripts\run_visual_regression.ps1"
if (-not (Test-Path -LiteralPath $visualScript -PathType Leaf)) {
    throw "run_visual_regression.ps1 not found at $visualScript"
}

Write-Host "`n[2/2] Running Visual Regression Gate Validation Mode (-Validate)..." -ForegroundColor Yellow
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $visualScript -Validate
if ($LASTEXITCODE -ne 0) {
    throw "Visual regression script validation mode failed with exit code $LASTEXITCODE"
}

Write-Host "`nAll visual regression policy and schema verifications passed successfully." -ForegroundColor Green
exit 0
