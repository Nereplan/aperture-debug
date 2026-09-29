<#
.SYNOPSIS
    Packages Aperture for release distribution with portable runtime dependencies.

.DESCRIPTION
    Creates a standalone, portable Aperture release package containing the main
    executable, tuned libmpv-2.dll, private bundled fonts,
    an empty portable data directory, and a cryptographic manifest.json.

.PARAMETER BinaryPath
    Path to the compiled Aperture.exe binary (defaults to debug\Aperture.Release.exe).

.PARAMETER OutputDirectory
    Target directory for the packaged release. Defaults to a timestamped dist\Aperture-release-<timestamp>.

.PARAMETER Version
    Application release version string. If omitted, derives from authoritative version.json.

.PARAMETER AllowVersionOverride
    Explicitly permits specifying a version argument that differs from version.json.

.PARAMETER Sign
    If specified, signs Aperture.exe (and bundled DLLs if -SignBundledDlls is specified)
    using Authenticode before finalizing package manifest hashes.

.PARAMETER CertificatePfxPath
    Path to .pfx or .p12 certificate file for Authenticode signing.

.PARAMETER CertificatePassword
    Password for PFX file. If omitted, read from APERTURE_SIGNING_CERT_PASSWORD,
    SIGNING_CERT_PASSWORD, or CERT_PASSWORD environment variables.

.PARAMETER CertificateThumbprint
    SHA-1 or SHA-256 certificate thumbprint for certificate store mode.

.PARAMETER CertStoreLocation
    Certificate store location when using thumbprint mode (default "Cert:\CurrentUser\My").

.PARAMETER TimestampServer
    RFC 3161 timestamp server URL (default "https://timestamp.digicert.com"). Must use HTTPS.

.PARAMETER SignBundledDlls
    If specified, signs bundled DLLs (libmpv-2.dll) in addition to Aperture.exe.

.PARAMETER DryRun
    Performs dry-run certificate and toolchain validation without writing signatures to disk.

.PARAMETER Build
    If specified, triggers build.ps1 before packaging.

.EXAMPLE
    .\scripts\package_release.ps1 -OutputDirectory .\dist\Aperture-v2.0.0

.EXAMPLE
    .\scripts\package_release.ps1 -Build

.EXAMPLE
    .\scripts\package_release.ps1 -OutputDirectory .\dist\Aperture-v2.0.0 -Sign -CertificatePfxPath C:\temp\release.pfx -TimestampServer https://timestamp.digicert.com
#>

param(
    [string]$BinaryPath = (Join-Path $PSScriptRoot "..\debug\Aperture.Release.exe"),
    [string]$OutputDirectory = "",
    [string]$Version = "",
    [switch]$AllowVersionOverride,
    [string]$Channel = "",
    [string]$Repository = $env:GITHUB_REPOSITORY,
    [switch]$Sign,
    [string]$CertificatePfxPath = "",
    [string]$CertificatePassword = "",
    [string]$CertificateThumbprint = "",
    [string]$CertStoreLocation = "Cert:\CurrentUser\My",
    [string]$TimestampServer = "https://timestamp.digicert.com",
    [switch]$SignBundledDlls,
    [switch]$DryRun,
    [switch]$Build
)

$ErrorActionPreference = "Stop"
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BinaryPath = [IO.Path]::GetFullPath($BinaryPath)
if (-not $Repository -and (Test-Path -LiteralPath (Join-Path $workspace '.git'))) {
    $remote = & git -C $workspace config --get remote.origin.url
    if ($LASTEXITCODE -eq 0 -and $remote -match 'github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$') { $Repository = $Matches[1] }
}
if ($Repository -and $Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Repository must be owner/name.' }

function Fail([string]$Message) { throw "Package failed: $Message" }

# Resolve authoritative version configuration from version.json
$versionJsonPath = Join-Path $workspace "version.json"
if (-not (Test-Path -LiteralPath $versionJsonPath -PathType Leaf)) {
    Fail "Authoritative version source not found: $versionJsonPath"
}
$versionConfig = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
$authoritativeVersion = $versionConfig.versionString

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $authoritativeVersion
} elseif ($Version -ne $authoritativeVersion -and -not $AllowVersionOverride) {
    Fail "Package version '$Version' disagrees with authoritative version '$authoritativeVersion' in version.json. Provide -AllowVersionOverride to force an explicit override."
}

# Load and validate channel policy
$policyScript = Join-Path $PSScriptRoot "validate_channel_policy.ps1"
if (Test-Path -LiteralPath $policyScript) {
    . $policyScript
}
$channelPolicy = if (Get-Command "Get-ApertureChannelPolicy" -ErrorAction SilentlyContinue) {
    Get-ApertureChannelPolicy
} else { $null }

if ([string]::IsNullOrWhiteSpace($Channel)) {
    $Channel = if ($versionConfig.channel) { $versionConfig.channel } elseif ($env:APERTURE_CHANNEL) { $env:APERTURE_CHANNEL } else { "stable" }
}

if ($channelPolicy) {
    if (-not (Test-ApertureChannelValid -ChannelId $Channel -Policy $channelPolicy)) {
        Fail "Specified channel '$Channel' is not a valid channel in channels.json"
    }
    $channelDef = Get-ApertureChannelDefinition -ChannelId $Channel -Policy $channelPolicy
    $Channel = $channelDef.id
    if (-not (Test-ApertureChannelPrerelease -ChannelId $Channel -VersionString $Version -Policy $channelPolicy)) {
        Fail "Version '$Version' violates prerelease rules for channel '$Channel'"
    }
}

function Test-PeX64([string]$Path) {
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $false }
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
        if ($peOffset -lt 0x40 -or $peOffset + 6 -gt $bytes.Length) { return $false }
        if ($bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) { return $false }
        return ([BitConverter]::ToUInt16($bytes, $peOffset + 4) -eq 0x8664)
    } catch { return $false }
}

if ($Build) {
    $buildScript = Join-Path $workspace "build.ps1"
    if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) { Fail "build.ps1 is missing" }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -BuildOutputPath $BinaryPath
    if ($LASTEXITCODE -ne 0) { Fail "Release build failed with exit code $LASTEXITCODE" }
}

if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) { Fail "Release binary not found: $BinaryPath" }
if (-not (Test-PeX64 $BinaryPath)) { Fail "Release binary is not a valid x64 PE image: $BinaryPath" }

$depManifestPath = Join-Path $PSScriptRoot "dependencies.json"
if (-not (Test-Path -LiteralPath $depManifestPath -PathType Leaf)) {
    $depManifestPath = Join-Path $workspace "scripts\dependencies.json"
}
if (-not (Test-Path -LiteralPath $depManifestPath -PathType Leaf)) { Fail "Authoritative dependency manifest not found: $depManifestPath" }
$depManifest = Get-Content -LiteralPath $depManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

$fontSource = Join-Path $workspace "fonts"
$dllSource = Join-Path $workspace "libmpv-2.dll"

# Validate and resolve vendored dependencies from authoritative manifest
$fontNames = @()
foreach ($vendored in @($depManifest.vendoredFiles)) {
    $filePath = Join-Path $workspace ($vendored.path -replace '/', '\')
    if ($vendored.category -eq "required") {
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) { Fail "Required vendored dependency is missing: $($vendored.path)" }
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($vendored.sha256 -and $actualHash -ne $vendored.sha256.ToLowerInvariant()) {
            Fail "SHA-256 mismatch for vendored dependency $($vendored.path): expected $($vendored.sha256), got $actualHash"
        }
        if ($vendored.path -match '^fonts/(.+\.ttf)$') {
            $fontNames += $Matches[1]
        }
    }
}

if (-not (Test-PeX64 $dllSource)) { Fail "libmpv-2.dll is not a valid x64 PE image: $dllSource" }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
    $OutputDirectory = Join-Path $workspace ("dist\Aperture-release-" + $stamp)
} else {
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
}

# Packaging never recursively deletes an arbitrary user path. An existing
# directory must be empty; callers can choose a timestamped output instead.
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Get-Item -LiteralPath $OutputDirectory).PSIsContainer) { Fail "Output path is not a directory: $OutputDirectory" }
    $existing = @(Get-ChildItem -LiteralPath $OutputDirectory -Force)
    if ($existing.Count -ne 0) { Fail "Output directory is not empty; choose a new timestamped path: $OutputDirectory" }
} else {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
}

$fontDestination = Join-Path $OutputDirectory "fonts"
$dataDestination = Join-Path $OutputDirectory "data"
$localesDestination = Join-Path $OutputDirectory "locales"
New-Item -ItemType Directory -Path $fontDestination, $dataDestination, $localesDestination -Force | Out-Null
Copy-Item -LiteralPath $BinaryPath -Destination (Join-Path $OutputDirectory "Aperture.exe")
Copy-Item -LiteralPath $dllSource -Destination (Join-Path $OutputDirectory "libmpv-2.dll")
foreach ($fontName in $fontNames) {
    Copy-Item -LiteralPath (Join-Path $fontSource $fontName) -Destination (Join-Path $fontDestination $fontName)
}
$localesSource = Join-Path $workspace "locales"
if (Test-Path -LiteralPath $localesSource -PathType Container) {
    Get-ChildItem -LiteralPath $localesSource -Filter "*.json" | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $localesDestination $_.Name)
    }
}

# -------------------------------------------------------------------------
# Optional Authenticode Signing (Executed before manifest hashes are finalized)
# -------------------------------------------------------------------------
$isSigningRequested = $Sign -or (-not [string]::IsNullOrWhiteSpace($CertificatePfxPath)) -or (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) -or $DryRun
$signatureInfo = $null

if ($isSigningRequested) {
    if (-not [string]::IsNullOrWhiteSpace($TimestampServer)) {
        if (-not ($TimestampServer.StartsWith("https://", [StringComparison]::OrdinalIgnoreCase))) {
            Fail "Timestamp server URL must use HTTPS: $TimestampServer"
        }
    }

    $signScript = Join-Path $PSScriptRoot "sign_artifacts.ps1"
    if (-not (Test-Path -LiteralPath $signScript -PathType Leaf)) {
        $signScript = Join-Path $workspace "scripts\sign_artifacts.ps1"
    }
    if (-not (Test-Path -LiteralPath $signScript -PathType Leaf)) {
        Fail "Authenticode signing script not found: $signScript"
    }

    $targetsToSign = @((Join-Path $OutputDirectory "Aperture.exe"))
    if ($SignBundledDlls) {
        $targetsToSign += (Join-Path $OutputDirectory "libmpv-2.dll")
    }

    Write-Host ("Signing {0} release artifact(s) with Authenticode..." -f $targetsToSign.Count) -ForegroundColor Cyan

    $signParams = @{
        Path = $targetsToSign
        Mode = "Sign"
        TimestampServer = $TimestampServer
        DigestAlgorithm = "SHA256"
        Description = "Aperture Portable Media Player"
        DescriptionUrl = "https://github.com/aperture-player/aperture"
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificatePfxPath)) {
        $signParams["CertificatePfxPath"] = $CertificatePfxPath
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificatePassword)) {
        $signParams["CertificatePassword"] = $CertificatePassword
    }
    if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
        $signParams["CertificateThumbprint"] = $CertificateThumbprint
        $signParams["CertStoreLocation"] = $CertStoreLocation
    }
    if ($DryRun) {
        $signParams["DryRun"] = $true
    }

    try {
        $signatureResults = & $signScript @signParams
        if ($signatureResults -is [array]) {
            $signatureInfo = $signatureResults[0]
        } else {
            $signatureInfo = $signatureResults
        }
    } catch {
        Fail "Authenticode signing failed: $($_.Exception.Message)"
    }

    if (-not $DryRun) {
        # Strictly verify signature post-signing before generating manifest hashes
        $verifyParams = @{
            Path = @((Join-Path $OutputDirectory "Aperture.exe"))
            Mode = "Verify"
            RequireSigned = $true
        }
        try {
            $postVerify = & $signScript @verifyParams
            if (-not $postVerify.Signed) {
                Fail "Authenticode verification failed: Aperture.exe is not signed"
            }
            $signatureInfo = $postVerify
        } catch {
            Fail "Authenticode post-signing verification failed: $($_.Exception.Message)"
        }
    }
}

foreach ($updateFile in @('Update.cmd', 'update.ps1', 'runtime_update.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $updateFile) -Destination (Join-Path $OutputDirectory $updateFile)
}
$releaseSource = @{ repository = [string]$Repository; version = $Version }
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'release-source.json'), ($releaseSource | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
$binaryInfo = Get-Item -LiteralPath (Join-Path $OutputDirectory "Aperture.exe")
$files = @()
Get-ChildItem -LiteralPath $OutputDirectory -File -Recurse |
    Where-Object { $_.Name -ne "manifest.json" } |
    Sort-Object @{Expression={ $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\','/') }} |
    ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\','/')
        $files += [ordered]@{
            path = $relative
            size = [uint64]$_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }

$manifest = [ordered]@{
    schemaVersion = 1
    application = "Aperture"
    version = $Version
    channel = $Channel
    configuration = "Release"
    compilerStandard = "c++17"
    architecture = "x64"
    binary = "Aperture.exe"
    binarySha256 = (Get-FileHash -LiteralPath $binaryInfo.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    sourceBinaryLastWriteUtc = (Get-Item -LiteralPath $BinaryPath).LastWriteTimeUtc.ToString("o")
    signature = if ($signatureInfo -and $signatureInfo.Signed) {
        [ordered]@{
            signed = $true
            status = if ($signatureInfo.DryRun) { "DryRun" } else { "Signed" }
            digestAlgorithm = $signatureInfo.DigestAlgorithm
            signerSubject = $signatureInfo.SignerSubject
            signerThumbprint = $signatureInfo.SignerThumbprint
            signerIssuer = $signatureInfo.SignerIssuer
            timestampServer = $TimestampServer
        }
    } else {
        [ordered]@{
            signed = $false
            status = "Unsigned"
        }
    }
    runtime = [ordered]@{
        platform = "Windows 10/11 x64"
        graphics = "Direct2D 1.1 / Direct3D 11"
        playback = "libmpv-2.dll"
        writableDirectory = "data"
    }
    files = $files
}
$manifestPath = Join-Path $OutputDirectory "manifest.json"
$manifestJson = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($manifestPath, $manifestJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Host "Package created: $OutputDirectory" -ForegroundColor Green
Write-Host "Manifest: $manifestPath" -ForegroundColor Cyan
if ($manifest.signature.signed) {
    Write-Host ("Authenticode: {0} (SHA-256; Signer: {1}; Thumbprint: {2})" -f $manifest.signature.status, $manifest.signature.signerSubject, $manifest.signature.signerThumbprint) -ForegroundColor Green
} else {
    Write-Host "Authenticode: Unsigned (standard reproducible package)" -ForegroundColor Cyan
}
Write-Host ("Files: {0}; binary SHA-256: {1}" -f $files.Count, $manifest.binarySha256) -ForegroundColor Cyan
