<#
.SYNOPSIS
    Verifies an Aperture release package distribution.

.DESCRIPTION
    Validates manifest integrity, file presence, SHA-256 hashes, x64 PE architecture,
    static C/C++ runtime linkage (no dynamic MSVC runtimes), runtime dependency closure,
    and performs isolated root-local staging startup with exact-PID lifecycle verification
    and guaranteed cleanup without modifying the original package.

.PARAMETER PackageDirectory
    Path to the packaged release directory containing manifest.json, Aperture.exe, etc.

.PARAMETER StageRoot
    Root-local staging parent (default scratch/bench-stage). All stages and cleanup
    paths must remain strict descendants of the Aperture root.

.PARAMETER StartupTimeoutSeconds
    Maximum time in seconds to wait for the staged Aperture main window to appear (default 15).

.PARAMETER RequireSigned
    If specified, requires the package to have a valid SHA-256 Authenticode signature and fails if unsigned.

.EXAMPLE
    .\scripts\verify_package.ps1 -PackageDirectory .\dist\Aperture-release-20260829-120000

.EXAMPLE
    .\scripts\verify_package.ps1 -PackageDirectory .\dist\Aperture-release-20260829-120000 -RequireSigned
#>

param(
    [Parameter(Mandatory = $false)]
    [string]$PackageDirectory = "",
    [int]$StartupTimeoutSeconds = 15,
    [switch]$RequireSigned,
    [string]$StageRoot = ""
)

$ErrorActionPreference = "Stop"
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..")).TrimEnd('\', '/')

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
    $StageRoot = Join-Path $workspace "scratch\bench-stage"
}
$StageRoot = Assert-RootLocalPath $StageRoot $workspace "StageRoot"

$autoCleanTemporaryPackage = $false
$tempPackageDir = $null

if ([string]::IsNullOrWhiteSpace($PackageDirectory)) {
    $distDir = Join-Path $workspace "dist"
    $candidatePkgs = if (Test-Path -LiteralPath $distDir -PathType Container) {
        @(Get-ChildItem -LiteralPath $distDir -Directory | Where-Object { Test-Path (Join-Path $_.FullName "manifest.json") -PathType Leaf } | Sort-Object LastWriteTime -Descending)
    } else { @() }

    if ($candidatePkgs.Count -gt 0) {
        $PackageDirectory = $candidatePkgs[0].FullName
    } else {
        $pkgScript = Join-Path $PSScriptRoot "package_release.ps1"
        if (-not (Test-Path -LiteralPath $pkgScript -PathType Leaf)) {
            throw "Package verification failed: Cannot auto-stage package, package_release.ps1 not found"
        }
        $tempPkgName = "Aperture-pkg-verify-auto-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
        $tempPackageDir = Assert-RootLocalPath (Join-Path $StageRoot $tempPkgName) $workspace "tempPackageDir"
        & $pkgScript -OutputDirectory $tempPackageDir
        $PackageDirectory = $tempPackageDir
        $autoCleanTemporaryPackage = $true
    }
}

$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
function Fail([string]$Message) { throw "Package verification failed: $Message" }

function Assert-ManifestFiles([string]$Root, $Manifest) {
    foreach ($entry in @($Manifest.files)) {
        $path = Join-Path $Root ($entry.path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "Manifest file is missing: $($entry.path)" }
        $info = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([uint64]$info.Length -ne [uint64]$entry.size -or $hash -ne $entry.sha256) { Fail "Manifest hash/size mismatch: $($entry.path)" }
    }
    $binaryPath = Join-Path $Root "Aperture.exe"
    $binaryHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($binaryHash -ne $Manifest.binarySha256) { Fail "Binary hash does not match manifest" }
}

if (-not (Test-Path -LiteralPath $PackageDirectory -PathType Container)) { Fail "Package directory not found: $PackageDirectory" }
$manifestPath = Join-Path $PackageDirectory "manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { Fail "manifest.json is missing" }
$manifestBytesBefore = [Convert]::ToBase64String([IO.File]::ReadAllBytes($manifestPath))
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.application -ne "Aperture" -or $manifest.configuration -ne "Release") { Fail "manifest identity/configuration is invalid" }

# Authoritative version verification
$versionJsonPath = Join-Path $PSScriptRoot "..\version.json"
if (Test-Path -LiteralPath $versionJsonPath -PathType Leaf) {
    $verConfig = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.version -ne $verConfig.versionString -and $manifest.version -ne $verConfig.displayVersion) {
        Fail "Package manifest version '$($manifest.version)' does not match authoritative version '$($verConfig.versionString)'"
    }
}

# Authoritative channel verification
$policyScript = Join-Path $PSScriptRoot "validate_channel_policy.ps1"
if (Test-Path -LiteralPath $policyScript) {
    . $policyScript
}
$channelPolicy = if (Get-Command "Get-ApertureChannelPolicy" -ErrorAction SilentlyContinue) {
    Get-ApertureChannelPolicy
} else { $null }

if ($channelPolicy -and $manifest.channel) {
    if (-not (Test-ApertureChannelValid -ChannelId $manifest.channel -Policy $channelPolicy)) {
        Fail "Package manifest channel '$($manifest.channel)' is not a valid channel in channels.json"
    }
    if (-not (Test-ApertureChannelPrerelease -ChannelId $manifest.channel -VersionString $manifest.version -Policy $channelPolicy)) {
        Fail "Package manifest version '$($manifest.version)' violates prerelease rules for channel '$($manifest.channel)'"
    }
}

# Authoritative dependency inventory verification
$depManifestPath = Join-Path $PSScriptRoot "dependencies.json"
if (-not (Test-Path -LiteralPath $depManifestPath -PathType Leaf)) {
    $depManifestPath = Join-Path $PSScriptRoot "..\scripts\dependencies.json"
}
if (-not (Test-Path -LiteralPath $depManifestPath -PathType Leaf)) { Fail "Authoritative dependency inventory missing: $depManifestPath" }
$depManifest = Get-Content -LiteralPath $depManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

$requiredPackageFiles = @("Aperture.exe")
foreach ($vendored in @($depManifest.vendoredFiles)) {
    if ($vendored.category -eq "required" -and ($vendored.path -ieq "libmpv-2.dll" -or $vendored.path -match '^fonts/')) {
        $rel = $vendored.path -replace '/', '\'
        $requiredPackageFiles += $rel
        $pkgFilePath = Join-Path $PackageDirectory $rel
        if (-not (Test-Path -LiteralPath $pkgFilePath -PathType Leaf)) { Fail "Required package file is missing: $rel" }
        $actualHash = (Get-FileHash -LiteralPath $pkgFilePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($vendored.sha256 -and $actualHash -ne $vendored.sha256.ToLowerInvariant()) {
            Fail "Package file $rel SHA-256 mismatch against authoritative inventory: expected $($vendored.sha256), found $actualHash"
        }
    }
}
foreach ($relative in $requiredPackageFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDirectory $relative) -PathType Leaf)) { Fail "Required package file is missing: $relative" }
}
$originalData = Join-Path $PackageDirectory "data"
if (-not (Test-Path -LiteralPath $originalData -PathType Container)) { Fail "Writable data directory is missing" }
if (@(Get-ChildItem -LiteralPath $originalData -Force).Count -ne 0) { Fail "Original package data directory must be empty before verification" }
$localesPkgDir = Join-Path $PackageDirectory "locales"
if (-not (Test-Path -LiteralPath $localesPkgDir -PathType Container)) { Fail "Required locales directory is missing" }
$enLocalePath = Join-Path $localesPkgDir "en-US.json"
if (-not (Test-Path -LiteralPath $enLocalePath -PathType Leaf)) { Fail "Required canonical locale asset is missing: locales/en-US.json" }
Assert-ManifestFiles $PackageDirectory $manifest

# Verify no debug symbol files or symbol archives are present in the portable package
$forbiddenSymbolFiles = @(Get-ChildItem -LiteralPath $PackageDirectory -Recurse -File | Where-Object {
    $_.Extension -ieq ".pdb" -or $_.Name -match '\.pdb$' -or $_.Name -match '\.symbols\.zip$' -or $_.Name -match '\.symbols\.json$'
})
if ($forbiddenSymbolFiles.Count -gt 0) {
    Fail "Portable package contains forbidden debug symbol or symbol archive files: $(($forbiddenSymbolFiles | ForEach-Object { $_.FullName }) -join ', ')"
}

# -------------------------------------------------------------------------
# Authenticode Signature Verification
# -------------------------------------------------------------------------
$exePath = Join-Path $PackageDirectory "Aperture.exe"
$signScript = Join-Path $PSScriptRoot "sign_artifacts.ps1"
$isSignedManifest = ($null -ne $manifest.signature -and $manifest.signature.signed -eq $true)

if ($isSignedManifest -or $RequireSigned) {
    $authSig = Get-AuthenticodeSignature -LiteralPath $exePath
    if ($authSig.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned) {
        Fail "Package binary Aperture.exe is unsigned, but signed package was declared in manifest or -RequireSigned was specified"
    }
    if ($authSig.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        Fail "Package binary Aperture.exe signature status is '$($authSig.Status)' (expected 'Valid'): $($authSig.StatusMessage)"
    }
    if ($null -eq $authSig.TimeStamperCertificate) {
        Fail "Package binary Aperture.exe is missing RFC 3161 timestamp signature"
    }

    # Validate digest algorithm
    $signerCert = $authSig.SignerCertificate
    $sigThumbprint = if ($signerCert) { ($signerCert.Thumbprint -replace '\s','').ToUpperInvariant() } else { "" }
    $sigSubject = if ($signerCert) { $signerCert.Subject } else { "" }

    $algFriendly = if ($signerCert -and $signerCert.SignatureAlgorithm) { $signerCert.SignatureAlgorithm.FriendlyName } else { "" }
    if ($algFriendly -and $algFriendly -notmatch 'sha256' -and $algFriendly -notmatch 'SHA256') {
        Fail "Authenticode signature digest algorithm must be SHA256, found: $algFriendly"
    }

    if ($manifest.signature -and $manifest.signature.signerThumbprint) {
        $expectedThumb = ($manifest.signature.signerThumbprint -replace '\s','').ToUpperInvariant()
        if ($sigThumbprint -ne $expectedThumb) {
            Fail "Authenticode signer thumbprint ($sigThumbprint) does not match manifest ($expectedThumb)"
        }
    }

    Write-Host ("Authenticode verification passed: Valid signature with RFC 3161 timestamp, SHA-256 by {0} (Thumbprint: {1})" -f $sigSubject, $sigThumbprint) -ForegroundColor Green
} else {
    if ($RequireSigned) {
        Fail "Package is unsigned but -RequireSigned was specified"
    }
    Write-Host "Authenticode: Unsigned (standard reproducible package)" -ForegroundColor Cyan
}
# Tracked PE dependency inspection
$dependencyScript = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\tests\package\inspect_pe_imports.py"))
if (-not (Test-Path -LiteralPath $dependencyScript -PathType Leaf)) { Fail "Dependency evidence script is missing: $dependencyScript" }
$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $pythonCommand) { Fail "Python is required for PE dependency evidence (PE import inspector)" }

$targetFiles = @(
    (Join-Path $PackageDirectory "Aperture.exe"),
    (Join-Path $PackageDirectory "libmpv-2.dll")
)

$dependencyJson = & $pythonCommand.Source $dependencyScript @targetFiles
if ($LASTEXITCODE -ne 0) { Fail "PE dependency evidence failed" }
$dependencyReport = $dependencyJson | ConvertFrom-Json

foreach ($entry in @($dependencyReport.reports)) {
    if ($entry.machine -ne "0x8664") { Fail "Package PE is not x64: $($entry.path) machine=$($entry.machine)" }
}

$exeReport = $dependencyReport.reports | Where-Object { [IO.Path]::GetFileName($_.path) -ieq "Aperture.exe" }
if ($null -eq $exeReport) { Fail "Aperture.exe inspection report is missing" }
$exeImports = @($exeReport.imports) | ForEach-Object { $_.ToUpperInvariant() }

foreach ($name in @("MSVCP140.DLL", "MSVCP140D.DLL", "VCRUNTIME140.DLL", "VCRUNTIME140D.DLL", "VCRUNTIME140_1.DLL", "VCRUNTIME140_1D.DLL", "UCRTBASED.DLL")) {
    if ($exeImports -contains $name) { Fail "Release executable still imports dynamic runtime: $name" }
}

# Stage root-locally under scratch/bench-stage, never beside/inside the package
# or under the OS temp root. The stage name is unique and is validated again
# before any recursive cleanup.
$stageName = "Aperture-package-verify-{0}-{1}" -f $PID, ([Guid]::NewGuid().ToString("N"))
$stagePath = Assert-RootLocalPath (Join-Path $StageRoot $stageName) $workspace "stagePath"
if (Test-Path -LiteralPath $stagePath) { Fail "Unique verification stage already exists" }
New-Item -ItemType Directory -Path $stagePath -Force | Out-Null
$stagePath = Assert-RootLocalPath $stagePath $workspace "stagePath"

$process = $null
try {
    foreach ($item in @(Get-ChildItem -LiteralPath $PackageDirectory -Force)) {
        Copy-Item -LiteralPath $item.FullName -Destination $stagePath -Recurse -Force
    }
    $stageExe = Join-Path $stagePath "Aperture.exe"
    # The staged package must resolve its application DLLs from its own folder;
    # do not rely on a Visual Studio developer shell or workspace PATH entries.
    $savedPath = $env:Path
    try {
        $env:Path = $stagePath
        $process = Start-Process -FilePath $stageExe -WorkingDirectory $stagePath -PassThru
    } finally {
        $env:Path = $savedPath
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { Fail "Staged process exited early with code $($process.ExitCode)" }
        if ($process.MainWindowHandle -ne 0) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($process.HasExited -or $process.MainWindowHandle -eq 0) { Fail "Staged main window did not appear within ${StartupTimeoutSeconds}s" }
    Write-Host ("Main window detected for isolated PID {0}" -f $process.Id) -ForegroundColor Green
} finally {
    if ($null -ne $process) {
        try {
            $process.Refresh()
            if (-not $process.HasExited) {
                $process.CloseMainWindow() | Out-Null
                if (-not $process.WaitForExit(5000)) {
                    # Only the exact PID launched by this verification is eligible.
                    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                    $process.WaitForExit(2000)
                }
            }
            if (-not $process.HasExited) { Fail "Isolated process PID $($process.Id) did not exit" }
        } catch {
            Write-Warning ("Could not close isolated verification PID {0}: {1}" -f $process.Id, $_.Exception.Message)
        }
    }
    if (Test-Path -LiteralPath $stagePath) {
        $resolvedStage = Assert-RootLocalPath $stagePath $workspace "stagePath"
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
if (Test-Path -LiteralPath $stagePath) { Fail "Verification stage cleanup did not complete" }

# Postcondition: the original package was never launched or changed.
Assert-ManifestFiles $PackageDirectory $manifest
if (@(Get-ChildItem -LiteralPath $originalData -Force).Count -ne 0) { Fail "Original package data directory changed during verification" }
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($manifestPath)) -ne $manifestBytesBefore) { Fail "Original manifest bytes changed during verification" }
Write-Host "Package verification passed: isolated startup, exact-PID shutdown, stage cleanup, hashes, and untouched original data." -ForegroundColor Green
if ($autoCleanTemporaryPackage -and $tempPackageDir) {
    try {
        $verifiedPkg = Assert-RootLocalPath $tempPackageDir $workspace "tempPackageDir"
        if (Test-Path -LiteralPath $verifiedPkg) {
            Remove-Item -LiteralPath $verifiedPkg -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch {}
}
