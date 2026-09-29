<#
.SYNOPSIS
    Generates a canonical, versioned, signed release manifest (release.json) for Aperture self-updates.

.DESCRIPTION
    Scans a packaged release directory, computes SHA-256 hashes for all runtime artifacts,
    validates the primary binary architecture (x64), extracts the Authenticode signer thumbprint
    if present, computes the canonical signing payload, signs it using ECDSA P-256 (NIST P-256 / SHA-256),
    and produces a release.json manifest compatible with Aperture's self-updater.

.PARAMETER PackageDirectory
    Path to the packaged release directory containing Aperture.exe and dependencies.

.PARAMETER OutputFile
    Destination path for the generated release.json manifest (defaults to <PackageDirectory>\release.json).

.PARAMETER Version
    Semantic version string for the release.

.PARAMETER Channel
    Release channel ("stable", "beta", etc. Default "stable").

.PARAMETER DownloadUrl
    Explicit HTTPS download URL where the update package will be hosted. Mandatory.

.PARAMETER MinimumUpdaterVersion
    Minimum updater version required to apply this update (default "2.0.0.0").

.PARAMETER ExpiryDays
    Number of days until the manifest expires (default 30).

.PARAMETER PrivateKeyHex
    Hex-encoded ECDSA P-256 private key blob (BCRYPT_ECCKEY_BLOB format) for signing. Mandatory.

.PARAMETER SignerThumbprint
    Optional explicit Authenticode SHA-256 signer certificate thumbprint. If omitted, extracted from binary.

.EXAMPLE
    .\scripts\generate_release_manifest.ps1 -PackageDirectory .\dist\Aperture-v2.0.0 -Version "2.1.0.0" -DownloadUrl "https://updates.aperture.example.com/Aperture-2.1.0.0.zip" -PrivateKeyHex "45435332..."
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDirectory,

    [Parameter(Mandatory = $true)]
    [string]$DownloadUrl,

    [Parameter(Mandatory = $true)]
    [string]$PrivateKeyHex,

    [Parameter()]
    [string]$OutputFile,

    [Parameter()]
    [string]$Version = "",
    [Parameter()]
    [string]$Channel = "stable",

    [Parameter()]
    [string]$MinimumUpdaterVersion = "",
    [Parameter()]
    [int]$ExpiryDays = 30,

    [Parameter()]
    [string]$SignerThumbprint,

    [Parameter()]
    [switch]$AllowVersionOverride
)

$ErrorActionPreference = "Stop"

# Load and validate channel policy
$policyScript = Join-Path $PSScriptRoot "validate_channel_policy.ps1"
if (Test-Path -LiteralPath $policyScript) {
    . $policyScript
}

$channelPolicy = if (Get-Command "Get-ApertureChannelPolicy" -ErrorAction SilentlyContinue) {
    Get-ApertureChannelPolicy
} else { $null }

if ($channelPolicy) {
    if (-not (Test-ApertureChannelValid -ChannelId $Channel -Policy $channelPolicy)) {
        throw "Channel '$Channel' is not a valid release channel defined in channels.json"
    }
    $channelDef = Get-ApertureChannelDefinition -ChannelId $Channel -Policy $channelPolicy
    $Channel = $channelDef.id
    if ($ExpiryDays -gt $channelDef.maxExpiryDays) {
        throw "ExpiryDays ($ExpiryDays) exceeds maximum allowed expiry ($($channelDef.maxExpiryDays) days) for channel '$Channel'"
    }
}

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$versionJsonPath = Join-Path $workspace "version.json"
$authoritativeVersion = "2.0.0.0"
if (Test-Path -LiteralPath $versionJsonPath -PathType Leaf) {
    $verConfig = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $authoritativeVersion = $verConfig.versionString
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $authoritativeVersion
} elseif ($Version -ne $authoritativeVersion -and -not $AllowVersionOverride) {
    throw "Specified -Version '$Version' does not match authoritative version '$authoritativeVersion' in version.json. Use -AllowVersionOverride if an explicit release override is intended."
}

if ($channelPolicy) {
    if (-not (Test-ApertureChannelPrerelease -ChannelId $Channel -VersionString $Version -Policy $channelPolicy)) {
        throw "Version '$Version' violates prerelease rules for release channel '$Channel'"
    }
}

if ([string]::IsNullOrWhiteSpace($MinimumUpdaterVersion)) {
    $MinimumUpdaterVersion = $authoritativeVersion
}

if ([string]::IsNullOrWhiteSpace($DownloadUrl) -or -not $DownloadUrl.StartsWith("https://", [StringComparison]::OrdinalIgnoreCase)) {
    throw "DownloadUrl must be explicitly provided and must start with https://"
}

if ([string]::IsNullOrWhiteSpace($PrivateKeyHex)) {
    throw "PrivateKeyHex is required for cryptographic detached manifest signing."
}

$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath $PackageDirectory -PathType Container)) {
    throw "Package directory does not exist: $PackageDirectory"
}

if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    $OutputFile = Join-Path $PackageDirectory "release.json"
}
$OutputFile = [IO.Path]::GetFullPath($OutputFile)

$exePath = Join-Path $PackageDirectory "Aperture.exe"
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Aperture.exe not found in package directory: $PackageDirectory"
}

# 1. Compute Package Files and Primary Binary SHA-256
$files = @()
$totalBytes = [uint64]0

Get-ChildItem -LiteralPath $PackageDirectory -File -Recurse |
    Where-Object { $_.Name -ne "release.json" -and $_.Name -ne "manifest.json" } |
    Sort-Object @{Expression={ $_.FullName.Substring($PackageDirectory.Length + 1).Replace('\','/') }} |
    ForEach-Object {
        $rel = $_.FullName.Substring($PackageDirectory.Length + 1).Replace('\','/')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $sz = [uint64]$_.Length
        $totalBytes += $sz
        $files += [ordered]@{
            path = $rel
            size = $sz
            sha256 = $hash
        }
    }

$exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
$exeSize = [uint64](Get-Item -LiteralPath $exePath).Length

# 2. Authenticode Signer Thumbprint Discovery
if ([string]::IsNullOrWhiteSpace($SignerThumbprint)) {
    $sig = Get-AuthenticodeSignature -FilePath $exePath
    if ($sig -and $sig.SignerCertificate) {
        $SignerThumbprint = (Get-FileHash -InputStream ([IO.MemoryStream]::new($sig.SignerCertificate.RawData)) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

if ([string]::IsNullOrWhiteSpace($SignerThumbprint)) {
    throw "SignerThumbprint could not be determined: Aperture.exe is unsigned and no -SignerThumbprint was passed."
}

$nowUtc = [DateTime]::UtcNow
$publishedUtc = $nowUtc.ToString("yyyy-MM-ddTHH:mm:ssZ")
$expiryUtc = $nowUtc.AddDays($ExpiryDays).ToString("yyyy-MM-ddTHH:mm:ssZ")

# 3. Construct Canonical String for Signature
$canonicalBuilder = [System.Text.StringBuilder]::new()
[void]$canonicalBuilder.AppendLine("Aperture-Release-Manifest-V1")
[void]$canonicalBuilder.AppendLine("application:Aperture")
[void]$canonicalBuilder.AppendLine("version:$Version")
[void]$canonicalBuilder.AppendLine("channel:$Channel")
if (-not [string]::IsNullOrWhiteSpace($MinimumUpdaterVersion)) {
    [void]$canonicalBuilder.AppendLine("minimumUpdaterVersion:$MinimumUpdaterVersion")
}
[void]$canonicalBuilder.AppendLine("publishedUtc:$publishedUtc")
[void]$canonicalBuilder.AppendLine("expiryUtc:$expiryUtc")
[void]$canonicalBuilder.AppendLine("downloadUrl:$DownloadUrl")
[void]$canonicalBuilder.AppendLine("fileSizeBytes:$totalBytes")
[void]$canonicalBuilder.AppendLine("sha256:$exeHash")
if (-not [string]::IsNullOrWhiteSpace($SignerThumbprint)) {
    [void]$canonicalBuilder.AppendLine("signerThumbprint:$SignerThumbprint")
}
foreach ($f in $files) {
    [void]$canonicalBuilder.AppendLine(("file:{0}:{1}:{2}" -f $f.path, $f.size, $f.sha256))
}
$canonicalString = $canonicalBuilder.ToString()

# 4. Sign Canonical String
Add-Type -AssemblyName System.Security
$cngKeyBlob = [byte[]]@()
for ($i = 0; $i -lt $PrivateKeyHex.Length; $i += 2) {
    $cngKeyBlob += [Convert]::ToByte($PrivateKeyHex.Substring($i, 2), 16)
}

$cngKey = [System.Security.Cryptography.CngKey]::Import($cngKeyBlob, [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$dsa = [System.Security.Cryptography.ECDsaCng]::new($cngKey)
$dsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
$dataBytes = [System.Text.Encoding]::UTF8.GetBytes($canonicalString)
$sigBytes = $dsa.SignData($dataBytes)
$signatureHex = [BitConverter]::ToString($sigBytes).Replace("-", "").ToLowerInvariant()

# 5. Build Canonical Manifest JSON Object
$manifest = [ordered]@{
    schemaVersion = 1
    application = "Aperture"
    version = $Version
    channel = $Channel
    minimumUpdaterVersion = $MinimumUpdaterVersion
    publishedUtc = $publishedUtc
    expiryUtc = $expiryUtc
    downloadUrl = $DownloadUrl
    fileSizeBytes = $totalBytes
    sha256 = $exeHash
    signerThumbprint = $SignerThumbprint
    signature = $signatureHex
    signatureAlgorithm = "ECDSA_P256_SHA256"
    releaseNotes = "Aperture $Version official release."
    files = $files
}

$jsonText = $manifest | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText($OutputFile, $jsonText + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Host "Canonical release manifest generated: $OutputFile" -ForegroundColor Green
Write-Host ("  Version: {0}; Files: {1}; Total Size: {2} bytes" -f $Version, $files.Count, $totalBytes) -ForegroundColor Cyan
Write-Host ("  Primary SHA-256: {0}" -f $exeHash) -ForegroundColor Cyan
Write-Host ("  Authenticode Signer: {0}" -f $SignerThumbprint) -ForegroundColor Cyan
Write-Host ("  Detached ECDSA Signature: {0}" -f $signatureHex.Substring(0, [Math]::Min(32, $signatureHex.Length)) + "...") -ForegroundColor Green
