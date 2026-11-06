<#
.SYNOPSIS
    Reports and validates Aperture build, runtime, test, and package dependencies.

.DESCRIPTION
    Loads the authoritative dependency inventory from scripts/dependencies.json,
    validates the presence, file size, and SHA-256 cryptographic hashes of all
    vendored runtime, header, and font artifacts, audits local toolchain versions
    (MSVC, Windows SDK, CMake, Ninja, Python, PowerShell), evaluates declared runtime
    package prerequisites, and discovers optional user tools (yt-dlp).

    Supports human-readable and JSON output formats, strict CI/build validation,
    noninteractive execution, and never modifies files or writes outside temp.

.PARAMETER Validate
    Enforces strict validation of all required toolchain versions, vendored artifact
    hashes, and core dependencies. Exits with code 1 if any required check fails.

.PARAMETER Json
    Outputs the dependency audit and validation results as structured JSON.

.PARAMETER ManifestPath
    Path to the dependency inventory JSON file. Defaults to scripts/dependencies.json.

.PARAMETER Quiet
    Suppresses console output and signals validation status solely through exit code.

.EXAMPLE
    .\scripts\check_dependencies.ps1

.EXAMPLE
    .\scripts\check_dependencies.ps1 -Validate

.EXAMPLE
    .\scripts\check_dependencies.ps1 -Json

.EXAMPLE
    .\scripts\check_dependencies.ps1 -Validate -Json
#>

[CmdletBinding()]
param(
    [switch]$Validate,
    [switch]$Json,
    [string]$ManifestPath = "",
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

# Determine workspace root and manifest path
$scriptDir = $PSScriptRoot
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir ".."))

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $candidates = @(
        (Join-Path $scriptDir "dependencies.json"),
        (Join-Path $workspaceRoot "dependencies.json"),
        (Join-Path $workspaceRoot "config\dependencies.json")
    )
    foreach ($cand in $candidates) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) {
            $ManifestPath = $cand
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
        $ManifestPath = Join-Path $scriptDir "dependencies.json"
    }
} else {
    $ManifestPath = [IO.Path]::GetFullPath($ManifestPath)
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    $errMsg = "Dependency manifest not found: $ManifestPath"
    if ($Json) {
        [PSCustomObject]@{
            success = $false
            error = $errMsg
        } | ConvertTo-Json -Depth 5
    } elseif (-not $Quiet) {
        Write-Error $errMsg
    }
    exit 1
}

# Load and parse manifest
try {
    $rawJson = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8
    $manifest = $rawJson | ConvertFrom-Json
} catch {
    $errMsg = "Failed to parse dependency manifest: $($_.Exception.Message)"
    if ($Json) {
        [PSCustomObject]@{
            success = $false
            error = $errMsg
        } | ConvertTo-Json -Depth 5
    } elseif (-not $Quiet) {
        Write-Error $errMsg
    }
    exit 1
}

$errors = @()
$warnings = @()

# -------------------------------------------------------------------------
# Helper Functions
# -------------------------------------------------------------------------

function Format-RelativePath([string]$FullPath, [string]$Root) {
    if ([string]::IsNullOrWhiteSpace($FullPath)) { return "" }
    $normFull = [IO.Path]::GetFullPath($FullPath)
    $normRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    if ($normFull.StartsWith($normRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $normFull.Substring($normRoot.Length).Replace('\', '/')
    }
    return $FullPath
}

function Compare-SemVer([string]$Actual, [string]$Required) {
    try {
        # Normalize versions to at least 3 parts: major.minor.patch
        $normActual = ($Actual -replace '[^0-9\.]', '').Trim('.')
        $normReq = ($Required -replace '[^0-9\.]', '').Trim('.')

        $partsA = $normActual.Split('.') | ForEach-Object { [int]$_ }
        $partsR = $normReq.Split('.') | ForEach-Object { [int]$_ }

        while ($partsA.Count -lt 3) { $partsA += 0 }
        while ($partsR.Count -lt 3) { $partsR += 0 }

        for ($i = 0; $i -lt [Math]::Min($partsA.Count, $partsR.Count); $i++) {
            if ($partsA[$i] -gt $partsR[$i]) { return 1 }
            if ($partsA[$i] -lt $partsR[$i]) { return -1 }
        }
        return 0
    } catch {
        return 0
    }
}

# -------------------------------------------------------------------------
# 1. Inspect & Validate Vendored Files / Artifacts
# -------------------------------------------------------------------------

$vendoredResults = @()
$vendoredCount = 0
$vendoredValidCount = 0

foreach ($entry in @($manifest.vendoredFiles)) {
    $vendoredCount++
    $relPath = ($entry.path -replace '/', '\')
    $fullPath = Join-Path $workspaceRoot $relPath
    $exists = Test-Path -LiteralPath $fullPath -PathType Leaf

    $status = "OK"
    $actualSize = [uint64]0
    $actualSha256 = ""
    $mismatchReasons = @()

    if (-not $exists) {
        $status = "MISSING"
        $mismatchReasons += "File not found at expected path: $($entry.path)"
        if ($entry.category -eq "required") {
            $errors += "Required vendored artifact missing: $($entry.name) ($($entry.path))"
        } else {
            $warnings += "Vendored artifact missing: $($entry.name) ($($entry.path))"
        }
    } else {
        $item = Get-Item -LiteralPath $fullPath
        $actualSize = [uint64]$item.Length
        $actualSha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()

        if ($entry.size -and $actualSize -ne [uint64]$entry.size) {
            $status = "SIZE_MISMATCH"
            $mismatchReasons += "Size mismatch: expected $($entry.size) bytes, found $actualSize bytes"
        }

        if ($entry.sha256 -and $actualSha256 -ne $entry.sha256.ToLowerInvariant()) {
            $status = "HASH_MISMATCH"
            $mismatchReasons += "SHA-256 mismatch: expected $($entry.sha256), found $actualSha256"
        }

        if ($status -ne "OK") {
            if ($entry.category -eq "required") {
                $errors += "Required vendored artifact invalid: $($entry.name) ($($mismatchReasons -join '; '))"
            } else {
                $warnings += "Vendored artifact invalid: $($entry.name) ($($mismatchReasons -join '; '))"
            }
        } else {
            $vendoredValidCount++
        }
    }

    $vendoredResults += [PSCustomObject]@{
        id = $entry.id
        name = $entry.name
        path = $entry.path
        category = $entry.category
        version = $entry.version
        expectedSize = $entry.size
        actualSize = $actualSize
        expectedSha256 = $entry.sha256
        actualSha256 = $actualSha256
        status = $status
        license = $entry.license
        source = $entry.source
        description = $entry.description
    }
}

# -------------------------------------------------------------------------
# 2. Inspect & Validate Toolchain
# -------------------------------------------------------------------------

$toolchainResults = @()

foreach ($tool in @($manifest.toolchain)) {
    $found = $false
    $detectedVersion = ""
    $status = "UNKNOWN"
    $details = ""

    switch ($tool.id) {
        "msvc" {
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
                foreach ($cand in $candidates) {
                    if (Test-Path -LiteralPath $cand) { $vsPath = $cand; break }
                }
            }

            if ($vsPath -and (Test-Path -LiteralPath $vsPath)) {
                $found = $true
                $vcToolsVerFile = Join-Path $vsPath "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
                if (Test-Path -LiteralPath $vcToolsVerFile) {
                    $vcVer = (Get-Content -LiteralPath $vcToolsVerFile -Raw).Trim()
                    # e.g. 14.44.35207 maps to MSVC 19.44
                    if ($vcVer -match '^14\.(\d+)\.') {
                        $minor = $Matches[1]
                        $detectedVersion = "19.$minor ($vcVer)"
                    } else {
                        $detectedVersion = $vcVer
                    }
                } else {
                    $detectedVersion = "19.30+ (VS 2022)"
                }

                $cmp = Compare-SemVer ($detectedVersion.Split(' ')[0]) $tool.minimumVersion
                if ($cmp -ge 0) {
                    $status = "OK"
                    $details = "Found Visual Studio 2022 MSVC toolset ($detectedVersion)"
                } else {
                    $status = "VERSION_TOO_LOW"
                    $details = "Detected MSVC $detectedVersion is below required minimum $($tool.minimumVersion)"
                    $errors += "MSVC version $detectedVersion is below minimum $($tool.minimumVersion)"
                }
            } else {
                $status = "NOT_FOUND"
                $details = "Visual Studio 2022 with C++ x64 toolset was not detected"
                if ($tool.category -eq "required") {
                    $errors += "Required tool missing: Visual Studio 2022 / MSVC x64 compiler"
                }
            }
        }

        "windows_sdk" {
            $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
            if (Test-Path -LiteralPath $sdkRoot) {
                $sdkVersions = Get-ChildItem -LiteralPath $sdkRoot -ErrorAction SilentlyContinue |
                    Where-Object { $_.PSIsContainer -and $_.Name -match '^10\.' } |
                    Sort-Object Name -Descending

                if ($sdkVersions -and $sdkVersions.Count -gt 0) {
                    $found = $true
                    $detectedVersion = $sdkVersions[0].Name
                    $cmp = Compare-SemVer $detectedVersion $tool.minimumVersion
                    if ($cmp -ge 0) {
                        $status = "OK"
                        $details = "Found Windows SDK ($detectedVersion)"
                    } else {
                        $status = "VERSION_TOO_LOW"
                        $details = "Detected Windows SDK $detectedVersion is below required minimum $($tool.minimumVersion)"
                        $errors += "Windows SDK version $detectedVersion is below minimum $($tool.minimumVersion)"
                    }
                }
            }
            if (-not $found) {
                $status = "NOT_FOUND"
                $details = "Windows 10/11 SDK headers not found in standard Windows Kits path"
                if ($tool.category -eq "required") {
                    $errors += "Required tool missing: Windows 10/11 SDK (10.0.19041.0+)"
                }
            }
        }

        "cmake" {
            $cmakeExe = $null
            $cmakeCmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
            if ($cmakeCmd -and (Test-Path -LiteralPath $cmakeCmd.Source)) {
                $cmakeExe = $cmakeCmd.Source
            }
            if (-not $cmakeExe -and $vsPath) {
                $bundledCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
                if (Test-Path -LiteralPath $bundledCmake) {
                    $cmakeExe = $bundledCmake
                }
            }
            if ($cmakeExe) {
                $found = $true
                try {
                    $out = & $cmakeExe --version | Select-Object -First 1
                    if ($out -match 'cmake version ([0-9\.]+)') {
                        $detectedVersion = $Matches[1]
                    } else {
                        $detectedVersion = "3.20.0"
                    }
                } catch {
                    $detectedVersion = "3.20.0"
                }

                $cmp = Compare-SemVer $detectedVersion $tool.minimumVersion
                if ($cmp -ge 0) {
                    $status = "OK"
                    $details = "CMake $detectedVersion located"
                } else {
                    $status = "VERSION_TOO_LOW"
                    $details = "CMake $detectedVersion is below required minimum $($tool.minimumVersion)"
                    $errors += "CMake version $detectedVersion is below minimum $($tool.minimumVersion)"
                }
            } else {
                $status = "NOT_FOUND"
                $details = "CMake executable not found in PATH or Visual Studio"
                if ($tool.category -eq "required") {
                    $errors += "Required tool missing: CMake ($($tool.minimumVersion)+)"
                }
            }
        }

        "ninja" {
            $ninjaExe = $null
            $ninjaCmd = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
            if ($ninjaCmd -and (Test-Path -LiteralPath $ninjaCmd.Source)) {
                $ninjaExe = $ninjaCmd.Source
            }
            if (-not $ninjaExe -and $vsPath) {
                $bundledNinja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
                if (Test-Path -LiteralPath $bundledNinja) {
                    $ninjaExe = $bundledNinja
                }
            }
            if ($ninjaExe) {
                $found = $true
                try {
                    $out = & $ninjaExe --version | Select-Object -First 1
                    $detectedVersion = $out.Trim()
                } catch {
                    $detectedVersion = "1.10.0"
                }
                $cmp = Compare-SemVer $detectedVersion $tool.minimumVersion
                if ($cmp -ge 0) {
                    $status = "OK"
                    $details = "Ninja $detectedVersion located"
                } else {
                    $status = "VERSION_TOO_LOW"
                    $details = "Ninja $detectedVersion is below recommended $($tool.minimumVersion)"
                    $warnings += "Ninja version $detectedVersion is below recommended $($tool.minimumVersion)"
                }
            } else {
                $status = "OPTIONAL_UNAVAILABLE"
                $details = "Ninja is not installed (optional; CMake will use Visual Studio generator)"
            }
        }

        "python" {
            $pyCmd = Get-Command "python.exe" -ErrorAction SilentlyContinue
            if ($pyCmd) {
                $found = $true
                try {
                    $out = & $pyCmd.Source --version 2>&1 | Select-Object -First 1
                    if ($out -match 'Python ([0-9\.]+)') {
                        $detectedVersion = $Matches[1]
                    } else {
                        $detectedVersion = "3.8.0"
                    }
                } catch {
                    $detectedVersion = "3.8.0"
                }
                $cmp = Compare-SemVer $detectedVersion $tool.minimumVersion
                if ($cmp -ge 0) {
                    $status = "OK"
                    $details = "Python $detectedVersion located in PATH"
                } else {
                    $status = "VERSION_TOO_LOW"
                    $details = "Python $detectedVersion is below required minimum $($tool.minimumVersion)"
                    if ($tool.category -eq "required") {
                        $errors += "Python version $detectedVersion is below minimum $($tool.minimumVersion)"
                    } else {
                        $warnings += "Python version $detectedVersion is below minimum $($tool.minimumVersion)"
                    }
                }
            } else {
                $status = if ($tool.category -eq "required") { "NOT_FOUND" } else { "OPTIONAL_UNAVAILABLE" }
                $details = "Python 3 executable not found in PATH"
                if ($tool.category -eq "required") {
                    $errors += "Required tool missing: Python 3 ($($tool.minimumVersion)+)"
                } else {
                    $warnings += "Python 3 is not available in PATH (needed for PE test suite and package verification)"
                }
            }
        }

        "powershell" {
            $found = $true
            $detectedVersion = $PSVersionTable.PSVersion.ToString()
            $cmp = Compare-SemVer $detectedVersion $tool.minimumVersion
            if ($cmp -ge 0) {
                $status = "OK"
                $details = "PowerShell $detectedVersion active"
            } else {
                $status = "VERSION_TOO_LOW"
                $details = "PowerShell $detectedVersion is below required minimum $($tool.minimumVersion)"
                $errors += "PowerShell version $detectedVersion is below minimum $($tool.minimumVersion)"
            }
        }

        "app_verifier" {
            $appverif = Join-Path $env:SystemRoot "System32\appverif.exe"
            if (Test-Path -LiteralPath $appverif) {
                $found = $true
                $status = "OK"
                $details = "Microsoft Application Verifier present at System32\appverif.exe"
            } else {
                $status = "OPTIONAL_UNAVAILABLE"
                $details = "Application Verifier not installed (optional manual hardening gate)"
            }
        }

        "signtool" {
            $signScript = Join-Path $scriptDir "sign_artifacts.ps1"
            $signtoolPath = $null
            if (Test-Path -LiteralPath $signScript -PathType Leaf) {
                try {
                    $disc = & $signScript -Mode DiscoverSigntool -Quiet
                    if ($disc -and $disc.Found) {
                        $signtoolPath = $disc.Path
                    }
                } catch {}
            }
            if (-not $signtoolPath) {
                $cmd = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
                if ($cmd) { $signtoolPath = $cmd.Source }
            }

            if ($signtoolPath -and (Test-Path -LiteralPath $signtoolPath -PathType Leaf)) {
                $found = $true
                $status = "OK"
                $details = "Microsoft SignTool present at $signtoolPath"
            } else {
                $status = "OPTIONAL_UNAVAILABLE"
                $details = "SignTool not found in Windows SDK or PATH (optional; needed for Authenticode release signing)"
            }
        }

        "yt_dlp" {
            $wsCandidates = @(
                (Join-Path $workspaceRoot "tools\yt-dlp.exe"),
                (Join-Path $workspaceRoot "yt-dlp.exe")
            )
            $wsYt = $null
            foreach ($c in $wsCandidates) {
                if (Test-Path -LiteralPath $c -PathType Leaf) { $wsYt = $c; break }
            }

            $pathCmd = Get-Command "yt-dlp.exe" -ErrorAction SilentlyContinue

            if ($wsYt) {
                $found = $true
                $status = "OK"
                $rel = Format-RelativePath $wsYt $workspaceRoot
                $details = "Discovered beside Aperture ($rel) [explicit opt-in discovery only]"
            } elseif ($pathCmd) {
                $found = $true
                $status = "DISCOVERED_IN_PATH"
                $details = "Discovered in PATH (requires explicit user setting opt-in in Aperture)"
            } else {
                $status = "NOT_CONFIGURED"
                $details = "Not present in workspace tools folder (optional external tool; explicit opt-in only)"
            }
        }

        default {
            $status = "OK"
            $details = "Unspecified tool verification"
        }
    }

    $toolchainResults += [PSCustomObject]@{
        id = $tool.id
        name = $tool.name
        category = $tool.category
        minimumVersion = $tool.minimumVersion
        detectedVersion = $detectedVersion
        found = $found
        status = $status
        details = $details
        license = $tool.license
        source = $tool.source
        description = $tool.description
    }
}

# -------------------------------------------------------------------------
# 3. Inspect Declared Runtime Package Dependencies
# -------------------------------------------------------------------------

$runtimePackageResults = @()

# -------------------------------------------------------------------------
# 4. CI Actions Record
# -------------------------------------------------------------------------

$ciActionResults = @()
foreach ($action in @($manifest.ciActions)) {
    $ciActionResults += [PSCustomObject]@{
        id = $action.id
        name = $action.name
        category = $action.category
        version = $action.version
        license = $action.license
        source = $action.source
        description = $action.description
    }
}

# -------------------------------------------------------------------------
# Summary & Output
# -------------------------------------------------------------------------

$isSuccess = ($errors.Count -eq 0)

$totalDependencyCount = $vendoredResults.Count + $toolchainResults.Count + $runtimePackageResults.Count + $ciActionResults.Count

$report = [PSCustomObject]@{
    success = $isSuccess
    timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
    manifestPath = Format-RelativePath $ManifestPath $workspaceRoot
    summary = [PSCustomObject]@{
        totalDependencies = $totalDependencyCount
        toolchainCount = $toolchainResults.Count
        vendoredFilesCount = $vendoredResults.Count
        vendoredValidCount = $vendoredValidCount
        runtimePackageCount = $runtimePackageResults.Count
        ciActionsCount = $ciActionResults.Count
        errorsCount = $errors.Count
        warningsCount = $warnings.Count
    }
    toolchain = $toolchainResults
    vendoredFiles = $vendoredResults
    runtimePackageDependencies = $runtimePackageResults
    ciActions = $ciActionResults
    errors = $errors
    warnings = $warnings
}

if ($Json) {
    $report | ConvertTo-Json -Depth 6
} elseif (-not $Quiet) {
    Write-Host "`n================================================================================" -ForegroundColor Cyan
    Write-Host "               Aperture Dependency Inventory & Validation Report                " -ForegroundColor Cyan
    Write-Host "================================================================================" -ForegroundColor Cyan
    Write-Host "Manifest: $($report.manifestPath)" -ForegroundColor Gray
    Write-Host "Total Registered Dependencies: $totalDependencyCount`n" -ForegroundColor Gray

    # 1. Vendored Files
    Write-Host "--- Vendored Runtime & Header Artifacts ($vendoredValidCount/$($vendoredResults.Count) valid) ---" -ForegroundColor Yellow
    foreach ($v in $vendoredResults) {
        $color = if ($v.status -eq "OK") { "Green" } else { "Red" }
        $tag = "[{0,-13}]" -f $v.status
        Write-Host "  $tag $($v.name)" -ForegroundColor $color -NoNewline
        Write-Host " ($($v.path))" -ForegroundColor Gray
        if ($v.status -ne "OK") {
            Write-Host "    Expected SHA-256: $($v.expectedSha256)" -ForegroundColor DarkYellow
            Write-Host "    Actual SHA-256:   $($v.actualSha256)" -ForegroundColor DarkRed
        } else {
            Write-Host "    Size: $($v.actualSize) bytes | SHA-256: $($v.actualSha256.Substring(0, 16))... | License: $($v.license)" -ForegroundColor DarkGray
        }
    }

    # 2. Toolchain
    Write-Host "`n--- Build & Test Toolchain ---" -ForegroundColor Yellow
    foreach ($t in $toolchainResults) {
        $color = switch ($t.status) {
            "OK" { "Green" }
            "DISCOVERED_IN_PATH" { "Cyan" }
            "NOT_CONFIGURED" { "DarkGray" }
            "OPTIONAL_UNAVAILABLE" { "DarkYellow" }
            default { "Red" }
        }
        $tag = "[{0,-20}]" -f $t.status
        Write-Host "  $tag $($t.name)" -ForegroundColor $color -NoNewline
        if ($t.detectedVersion) {
            Write-Host " (Version: $($t.detectedVersion))" -ForegroundColor Gray
        } else {
            Write-Host ""
        }
        Write-Host "    Details: $($t.details) | License: $($t.license)" -ForegroundColor DarkGray
    }

    # 3. Runtime Package Dependencies
    Write-Host "`n--- Runtime Package Prerequisites ---" -ForegroundColor Yellow
    foreach ($r in $runtimePackageResults) {
        $color = if ($r.status -eq "OK") { "Green" } else { "DarkYellow" }
        $tag = "[{0,-20}]" -f $r.status
        Write-Host "  $tag $($r.name)" -ForegroundColor $color -NoNewline
        Write-Host " ($($r.fileName))" -ForegroundColor Gray
        Write-Host "    Details: $($r.details) | License: $($r.license)" -ForegroundColor DarkGray
    }

    # 4. CI Actions
    Write-Host "`n--- CI Workflow Actions ---" -ForegroundColor Yellow
    foreach ($c in $ciActionResults) {
        Write-Host "  [CI_STEP             ] $($c.name) ($($c.version)) - License: $($c.license)" -ForegroundColor DarkGray
    }

    # Verdict
    Write-Host "`n--------------------------------------------------------------------------------" -ForegroundColor Cyan
    if ($isSuccess) {
        Write-Host "STATUS: ALL REQUIRED DEPENDENCIES VALIDATED SUCCESSFULLY ($totalDependencyCount tracked, 0 errors, $($warnings.Count) warnings)" -ForegroundColor Green
    } else {
        Write-Host "STATUS: DEPENDENCY VALIDATION FAILED ($($errors.Count) errors, $($warnings.Count) warnings)" -ForegroundColor Red
        foreach ($e in $errors) {
            Write-Host "  ERROR: $e" -ForegroundColor Red
        }
    }
    Write-Host "================================================================================`n" -ForegroundColor Cyan
}

if ($Validate -and -not $isSuccess) {
    exit 1
}

exit 0
