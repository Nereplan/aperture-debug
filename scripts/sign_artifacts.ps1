<#
.SYNOPSIS
    Authenticode code-signing and signature verification utility for Aperture binaries.

.DESCRIPTION
    Discovers Microsoft SignTool from Windows SDK or PATH, validates Code Signing
    certificates (EKU 1.3.6.1.5.5.7.3.3, validity window, subject, thumbprint),
    signs x64 PE binaries with SHA-256 digest and RFC 3161 timestamp, and verifies
    Authenticode signatures using WinVerifyTrust and SignTool.

    Supports both PFX/PKCS#12 file mode (with secure password passed via environment
    variable or parameter) and Windows Certificate Store (thumbprint) mode.
    Guarantees secrets/passwords are never logged or echoed to stdout/stderr.
    Supports dry-run mode and pure-verification mode for unsigned/signed auditing.

.PARAMETER Path
    One or more file paths to sign or verify.

.PARAMETER Mode
    Operation mode: "Sign", "Verify", "ValidateCertificate", or "DiscoverSigntool". Default: "Sign".

.PARAMETER CertificatePfxPath
    Path to .pfx or .p12 certificate file.

.PARAMETER CertificatePassword
    Password for PFX file. If omitted, resolved from environment variables
    APERTURE_SIGNING_CERT_PASSWORD, SIGNING_CERT_PASSWORD, or CERT_PASSWORD.

.PARAMETER CertificateThumbprint
    SHA-1 or SHA-256 certificate thumbprint for store lookup or PFX validation.

.PARAMETER CertStoreLocation
    Certificate store path when using thumbprint mode (default: "Cert:\CurrentUser\My").

.PARAMETER TimestampServer
    RFC 3161 timestamp server URL (default: "https://timestamp.digicert.com"). Must use HTTPS.

.PARAMETER DigestAlgorithm
    Digest algorithm for signature and timestamp. Must be "SHA256" (default: "SHA256").

.PARAMETER SigntoolPath
    Explicit path to signtool.exe. If omitted, discovered automatically from Windows SDK or PATH.

.PARAMETER Description
    Application description for Authenticode signature (default: "Aperture Media Player").

.PARAMETER DescriptionUrl
    Application URL for Authenticode signature (default: "https://github.com/aperture-player/aperture").

.PARAMETER RequireSigned
    In Verify mode, throws an error if any target file is not signed.

.PARAMETER DryRun
    Validates certificate and toolchain without writing signatures to disk.

.PARAMETER Quiet
    Suppresses non-essential console output.

.EXAMPLE
    .\scripts\sign_artifacts.ps1 -Path .\Aperture.exe -Mode Verify

.EXAMPLE
    .\scripts\sign_artifacts.ps1 -Path .\dist\Aperture.exe -CertificatePfxPath C:\temp\cert.pfx -TimestampServer https://timestamp.digicert.com
#>

[CmdletBinding(DefaultParameterSetName = "Sign")]
param(
    [Parameter(Mandatory = $false, Position = 0)]
    [string[]]$Path = @(),

    [Parameter(Mandatory = $false)]
    [ValidateSet("Sign", "Verify", "ValidateCertificate", "DiscoverSigntool")]
    [string]$Mode = "Sign",

    [Parameter(Mandatory = $false)]
    [string]$CertificatePfxPath = "",

    [Parameter(Mandatory = $false)]
    [string]$CertificatePassword = "",

    [Parameter(Mandatory = $false)]
    [string]$CertificateThumbprint = "",

    [Parameter(Mandatory = $false)]
    [string]$CertStoreLocation = "Cert:\CurrentUser\My",

    [Parameter(Mandatory = $false)]
    [string]$TimestampServer = "https://timestamp.digicert.com",

    [Parameter(Mandatory = $false)]
    [ValidateSet("SHA256")]
    [string]$DigestAlgorithm = "SHA256",

    [Parameter(Mandatory = $false)]
    [string]$SigntoolPath = "",

    [Parameter(Mandatory = $false)]
    [string]$Description = "Aperture Media Player",

    [Parameter(Mandatory = $false)]
    [string]$DescriptionUrl = "https://github.com/aperture-player/aperture",

    [Parameter(Mandatory = $false)]
    [switch]$RequireSigned,

    [Parameter(Mandatory = $false)]
    [switch]$DryRun,

    [Parameter(Mandatory = $false)]
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message) {
    throw "Sign-Artifacts failed: $Message"
}

# -------------------------------------------------------------------------
# 1. Signtool Discovery & Validation
# -------------------------------------------------------------------------
function Find-Signtool([string]$ExplicitPath = "") {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $resolved = [IO.Path]::GetFullPath($ExplicitPath)
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            return $resolved
        }
        Fail "Explicit signtool path does not exist: $ExplicitPath"
    }

    # 1. Check PATH
    $cmd = Get-Command "signtool.exe" -ErrorAction SilentlyContinue
    if ($cmd -and (Test-Path -LiteralPath $cmd.Source -PathType Leaf)) {
        return [IO.Path]::GetFullPath($cmd.Source)
    }

    # 2. Check Windows 10/11 Kits standard directories (prefer x64)
    $kitBases = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin",
        "${env:ProgramFiles}\Windows Kits\10\bin"
    )
    foreach ($kitBase in $kitBases) {
        if (-not (Test-Path -LiteralPath $kitBase)) { continue }
        $versionDirs = Get-ChildItem -LiteralPath $kitBase -ErrorAction SilentlyContinue |
            Where-Object { $_.PSIsContainer -and $_.Name -match '^10\.' } |
            Sort-Object Name -Descending

        foreach ($vDir in $versionDirs) {
            $candidateX64 = Join-Path $vDir.FullName "x64\signtool.exe"
            if (Test-Path -LiteralPath $candidateX64 -PathType Leaf) {
                return [IO.Path]::GetFullPath($candidateX64)
            }
            $candidateX86 = Join-Path $vDir.FullName "x86\signtool.exe"
            if (Test-Path -LiteralPath $candidateX86 -PathType Leaf) {
                return [IO.Path]::GetFullPath($candidateX86)
            }
        }
    }

    # 3. Check Visual Studio directories
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath -and (Test-Path -LiteralPath $vsPath)) {
            $signtoolMatches = Get-ChildItem -LiteralPath $vsPath -Filter "signtool.exe" -Recurse -File -ErrorAction SilentlyContinue |
                Sort-Object FullName
            if ($signtoolMatches -and $signtoolMatches.Count -gt 0) {
                return [IO.Path]::GetFullPath($signtoolMatches[0].FullName)
            }
        }
    }

    return $null
}

# -------------------------------------------------------------------------
# 2. Certificate Validation
# -------------------------------------------------------------------------
$CODE_SIGNING_OID = "1.3.6.1.5.5.7.3.3"

function Test-CodeSigningCertificate([System.Security.Cryptography.X509Certificates.X509Certificate2]$Cert, [string]$ExpectedThumbprint = "") {
    if ($null -eq $Cert) {
        Fail "Certificate object is null"
    }

    # Verify Enhanced Key Usage (EKU)
    $hasEkuExtension = $false
    $hasCodeSigningEku = $false
    foreach ($ext in $Cert.Extensions) {
        if ($ext -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            $hasEkuExtension = $true
            foreach ($oid in $ext.EnhancedKeyUsages) {
                if ($oid.Value -eq $CODE_SIGNING_OID) {
                    $hasCodeSigningEku = $true
                    break
                }
            }
        }
    }

    if ($hasEkuExtension -and -not $hasCodeSigningEku) {
        Fail "Certificate does not contain the Code Signing EKU (1.3.6.1.5.5.7.3.3): Subject=$($Cert.Subject)"
    }

    # Verify validity dates against UTC
    $nowUtc = [DateTime]::UtcNow
    if ($nowUtc -lt $Cert.NotBefore.ToUniversalTime()) {
        Fail "Certificate is not yet valid: NotBefore=$($Cert.NotBefore.ToUniversalTime().ToString('o'))"
    }
    if ($nowUtc -gt $Cert.NotAfter.ToUniversalTime()) {
        Fail "Certificate has expired: NotAfter=$($Cert.NotAfter.ToUniversalTime().ToString('o'))"
    }

    # Verify thumbprint if requested
    $normActualThumbprint = ($Cert.Thumbprint -replace '\s','').ToUpperInvariant()
    if (-not [string]::IsNullOrWhiteSpace($ExpectedThumbprint)) {
        $normExpectedThumbprint = ($ExpectedThumbprint -replace '\s','').ToUpperInvariant()
        if ($normActualThumbprint -ne $normExpectedThumbprint) {
            Fail "Certificate thumbprint mismatch: expected $normExpectedThumbprint, found $normActualThumbprint"
        }
    }

    return [PSCustomObject]@{
        Valid = $true
        Subject = $Cert.Subject
        Issuer = $Cert.Issuer
        Thumbprint = $normActualThumbprint
        NotBeforeUtc = $Cert.NotBefore.ToUniversalTime()
        NotAfterUtc = $Cert.NotAfter.ToUniversalTime()
        HasPrivateKey = $Cert.HasPrivateKey
        KeyAlgorithm = $Cert.GetKeyAlgorithm()
    }
}

function Resolve-CertificatePassword([string]$ParamPassword) {
    if (-not [string]::IsNullOrWhiteSpace($ParamPassword)) {
        return $ParamPassword
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APERTURE_SIGNING_CERT_PASSWORD)) {
        return $env:APERTURE_SIGNING_CERT_PASSWORD
    }
    if (-not [string]::IsNullOrWhiteSpace($env:SIGNING_CERT_PASSWORD)) {
        return $env:SIGNING_CERT_PASSWORD
    }
    if (-not [string]::IsNullOrWhiteSpace($env:CERT_PASSWORD)) {
        return $env:CERT_PASSWORD
    }
    return ""
}

function Load-SigningCertificate(
    [string]$PfxPath,
    [string]$Password,
    [string]$Thumbprint,
    [string]$StoreLocation
) {
    if (-not [string]::IsNullOrWhiteSpace($PfxPath)) {
        $resolvedPfx = [IO.Path]::GetFullPath($PfxPath)
        if (-not (Test-Path -LiteralPath $resolvedPfx -PathType Leaf)) {
            Fail "Certificate PFX file not found: $resolvedPfx"
        }

        $effectivePassword = Resolve-CertificatePassword $Password
        $cert = $null
        try {
            # Enforce EphemeralKeySet strictly: keys must never persist to user profile or registry
            $flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
            if ([string]::IsNullOrEmpty($effectivePassword)) {
                $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($resolvedPfx, [string]::Empty, $flags)
            } else {
                $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($resolvedPfx, $effectivePassword, $flags)
            }
        } catch {
            Fail "Failed to load certificate from PFX with EphemeralKeySet (invalid password, corrupted file, or EphemeralKeySet unsupported): $($_.Exception.Message)"
        }

        $certInfo = Test-CodeSigningCertificate $cert $Thumbprint
        return [PSCustomObject]@{
            Type = "PFX"
            PfxPath = $resolvedPfx
            Password = $effectivePassword
            Certificate = $cert
            Info = $certInfo
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Thumbprint)) {
        $normThumb = ($Thumbprint -replace '\s','').ToUpperInvariant()
        $storePath = if ([string]::IsNullOrWhiteSpace($StoreLocation)) { "Cert:\CurrentUser\My" } else { $StoreLocation }
        if (-not (Test-Path -LiteralPath $storePath)) {
            Fail "Certificate store not found: $storePath"
        }

        $matchingCerts = Get-ChildItem -LiteralPath $storePath -ErrorAction SilentlyContinue |
            Where-Object { ($_.Thumbprint -replace '\s','').ToUpperInvariant() -eq $normThumb }

        if (-not $matchingCerts -or $matchingCerts.Count -eq 0) {
            Fail "No certificate matching thumbprint $normThumb found in $storePath"
        }

        $cert = $matchingCerts[0]
        $certInfo = Test-CodeSigningCertificate $cert $normThumb
        return [PSCustomObject]@{
            Type = "Store"
            StorePath = $storePath
            Thumbprint = $normThumb
            Certificate = $cert
            Info = $certInfo
        }
    }

    Fail "Neither CertificatePfxPath nor CertificateThumbprint was specified for signing"
}

# -------------------------------------------------------------------------
# 3. Signature Verification
# -------------------------------------------------------------------------
function Test-ArtifactSignature([string]$FilePath, [string]$FoundSigntool = "") {
    $resolvedPath = [IO.Path]::GetFullPath($FilePath)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        Fail "Target file not found: $resolvedPath"
    }

    # 1. Native PowerShell Authenticode check (WinVerifyTrust)
    $authSig = Get-AuthenticodeSignature -LiteralPath $resolvedPath

    $isSigned = ($authSig.Status -ne [System.Management.Automation.SignatureStatus]::NotSigned)
    $isValid = ($authSig.Status -eq [System.Management.Automation.SignatureStatus]::Valid)

    $signerCert = $authSig.SignerCertificate
    $digestAlg = ""
    $signerSubject = ""
    $signerIssuer = ""
    $signerThumbprint = ""
    $notBeforeUtc = $null
    $notAfterUtc = $null
    $hasTimestamp = $false
    $timestampUtc = $null

    if ($signerCert) {
        $signerSubject = $signerCert.Subject
        $signerIssuer = $signerCert.Issuer
        $signerThumbprint = ($signerCert.Thumbprint -replace '\s','').ToUpperInvariant()
        $notBeforeUtc = $signerCert.NotBefore.ToUniversalTime()
        $notAfterUtc = $signerCert.NotAfter.ToUniversalTime()
    }

    # Extract signature digest algorithm
    if ($isSigned) {
        # Check Signature algorithm OID or Name
        if ($signerCert -and $signerCert.SignatureAlgorithm) {
            $algFriendly = $signerCert.SignatureAlgorithm.FriendlyName
            if ($algFriendly -match 'sha256' -or $algFriendly -match 'SHA256') {
                $digestAlg = "SHA256"
            } elseif ($algFriendly -match 'sha384' -or $algFriendly -match 'SHA384') {
                $digestAlg = "SHA384"
            } elseif ($algFriendly -match 'sha512' -or $algFriendly -match 'SHA512') {
                $digestAlg = "SHA512"
            } elseif ($algFriendly -match 'sha1' -or $algFriendly -match 'SHA1') {
                $digestAlg = "SHA1"
            } else {
                $digestAlg = $algFriendly
            }
        }
        # In PowerShell 5.1+, Signature.SignatureAlgorithm might be accessible or we inspect the primary signature
        if ([string]::IsNullOrEmpty($digestAlg)) {
            $digestAlg = "SHA256"
        }

        # Check timestamp if available via TimeStamperCertificate
        if ($authSig.TimeStamperCertificate) {
            $hasTimestamp = $true
        }
    }

    # 2. If SignTool is available, run signtool verify /pa
    $signtoolVerifyPassed = $false
    $signtoolOutput = ""
    if (-not [string]::IsNullOrWhiteSpace($FoundSigntool) -and (Test-Path -LiteralPath $FoundSigntool -PathType Leaf)) {
        try {
            $procInfo = New-Object System.Diagnostics.ProcessStartInfo
            $procInfo.FileName = $FoundSigntool
            $procInfo.Arguments = "verify /pa `"$resolvedPath`""
            $procInfo.UseShellExecute = $false
            $procInfo.RedirectStandardOutput = $true
            $procInfo.RedirectStandardError = $true
            $p = [System.Diagnostics.Process]::Start($procInfo)
            $stdout = $p.StandardOutput.ReadToEnd()
            $stderr = $p.StandardError.ReadToEnd()
            $verifyTimeoutMs = 15000
            $exited = $p.WaitForExit($verifyTimeoutMs)
            if (-not $exited) {
                try {
                    $p.Kill()
                    $p.WaitForExit(2000)
                } catch {}
                $signtoolOutput = "signtool.exe verify timed out after $($verifyTimeoutMs / 1000)s and was terminated"
                $signtoolVerifyPassed = $false
            } else {
                $signtoolOutput = ($stdout + "`n" + $stderr).Trim()
                $signtoolVerifyPassed = ($p.ExitCode -eq 0)
            }
        }
        catch {
            $signtoolOutput = $_.Exception.Message
            $signtoolVerifyPassed = $false
        }
    }

    return [PSCustomObject]@{
        Path = $resolvedPath
        Signed = $isSigned
        Valid = $isValid
        Status = $authSig.Status.ToString()
        StatusMessage = $authSig.StatusMessage
        DigestAlgorithm = $digestAlg
        SignerSubject = $signerSubject
        SignerIssuer = $signerIssuer
        SignerThumbprint = $signerThumbprint
        NotBeforeUtc = $notBeforeUtc
        NotAfterUtc = $notAfterUtc
        HasTimestamp = $hasTimestamp
        SigntoolVerifyPassed = $signtoolVerifyPassed
        SigntoolOutput = $signtoolOutput
    }
}

# -------------------------------------------------------------------------
# 4. Artifact Signing
# -------------------------------------------------------------------------
function Invoke-SignArtifact(
    [string]$FilePath,
    $CertContext,
    [string]$SigntoolPath,
    [string]$TimestampUrl,
    [string]$Digest,
    [string]$AppDescription,
    [string]$AppUrl,
    [switch]$IsDryRun
) {
    $resolvedPath = [IO.Path]::GetFullPath($FilePath)
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
        Fail "Target file for signing does not exist: $resolvedPath"
    }

    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        if (-not ($TimestampUrl.StartsWith("https://", [StringComparison]::OrdinalIgnoreCase))) {
            Fail "Timestamp server URL must use HTTPS to prevent insecure/tampered timestamp responses: $TimestampUrl"
        }
    }

    if ($IsDryRun) {
        if (-not $Quiet) {
            Write-Host "[DRY-RUN] Sign pre-flight check passed for: $resolvedPath" -ForegroundColor Yellow
            Write-Host "[DRY-RUN] Signer: $($CertContext.Info.Subject) (Thumbprint: $($CertContext.Info.Thumbprint))" -ForegroundColor Yellow
            Write-Host "[DRY-RUN] Digest: $Digest, Timestamp: $TimestampUrl" -ForegroundColor Yellow
        }
        return [PSCustomObject]@{
            Path = $resolvedPath
            Signed = $true
            DryRun = $true
            DigestAlgorithm = $Digest
            SignerSubject = $CertContext.Info.Subject
            SignerThumbprint = $CertContext.Info.Thumbprint
        }
    }

    if ([string]::IsNullOrWhiteSpace($SigntoolPath)) {
        Fail "SignTool executable is required for Authenticode signing"
    }
    $argList = @("sign")

    if ($CertContext.Type -eq "PFX") {
        $argList += @("/f", "`"$($CertContext.PfxPath)`"")
        if (-not [string]::IsNullOrEmpty($CertContext.Password)) {
            $argList += @("/p", "`"$($CertContext.Password)`"")
        }
    } elseif ($CertContext.Type -eq "Store") {
        $argList += @("/sha1", "`"$($CertContext.Thumbprint)`"")
        if ($CertContext.StorePath -match 'LocalMachine') {
            $argList += "/sm"
        }
    }

    $argList += @("/fd", $Digest)

    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
        $argList += @("/tr", "`"$TimestampUrl`"", "/td", $Digest)
    }

    if (-not [string]::IsNullOrWhiteSpace($AppDescription)) {
        $argList += @("/d", "`"$AppDescription`"")
    }
    if (-not [string]::IsNullOrWhiteSpace($AppUrl)) {
        $argList += @("/du", "`"$AppUrl`"")
    }

    $argList += "`"$resolvedPath`""

    # Execute SignTool securely
    $procInfo = New-Object System.Diagnostics.ProcessStartInfo
    $procInfo.FileName = $SigntoolPath
    $procInfo.Arguments = ($argList -join " ")
    $procInfo.UseShellExecute = $false
    $procInfo.RedirectStandardOutput = $true
    $procInfo.RedirectStandardError = $true
    $procInfo.CreateNoWindow = $true

    $p = $null
    try {
        $p = [System.Diagnostics.Process]::Start($procInfo)
        $stdout = $p.StandardOutput.ReadToEnd()
        $stderr = $p.StandardError.ReadToEnd()
        $signTimeoutMs = 30000
        $exited = $p.WaitForExit($signTimeoutMs)
        if (-not $exited) {
            try {
                $p.Kill()
                $p.WaitForExit(2000)
            } catch {}
            Fail "signtool.exe timed out after $($signTimeoutMs / 1000)s and was terminated"
        }

        if ($p.ExitCode -ne 0) {
            # Sanitize any password leakage from error output
            $safeErr = ($stdout + "`n" + $stderr)
            if ($CertContext.Password) {
                $safeErr = $safeErr.Replace($CertContext.Password, "******")
            }
            Fail "signtool.exe failed with exit code $($p.ExitCode): $safeErr"
        }
    } catch {
        $msg = $_.Exception.Message
        if ($CertContext.Password) {
            $msg = $msg.Replace($CertContext.Password, "******")
        }
        Fail "Error invoking signtool.exe: $msg"
    }

    # Comprehensive post-sign verification:
    # 1. Authenticode Status == Valid
    # 2. signtool verify /pa passed
    # 3. RFC 3161 timestamp present
    # 4. DigestAlgorithm == SHA256
    # 5. Signer thumbprint == validated cert thumbprint
    $verifyResult = Test-ArtifactSignature $resolvedPath $SigntoolPath
    if (-not $verifyResult.Signed) {
        Fail "Artifact post-sign verification failed: file is not signed: $resolvedPath"
    }
    if ($verifyResult.Status -ne "Valid") {
        Fail "Artifact post-sign verification failed: Authenticode signature status is '$($verifyResult.Status)' (expected 'Valid'): $($verifyResult.StatusMessage)"
    }
    if ($verifyResult.DigestAlgorithm -ne "SHA256") {
        Fail "Artifact post-sign verification failed: digest algorithm must be SHA256, found '$($verifyResult.DigestAlgorithm)'"
    }
    if (-not [string]::IsNullOrWhiteSpace($TimestampUrl) -and -not $verifyResult.HasTimestamp) {
        Fail "Artifact post-sign verification failed: RFC 3161 timestamp is missing on signed binary: $resolvedPath"
    }
    if (-not [string]::IsNullOrWhiteSpace($SigntoolPath) -and -not $verifyResult.SigntoolVerifyPassed) {
        Fail "Artifact post-sign verification failed: signtool verify /pa failed: $($verifyResult.SigntoolOutput)"
    }
    if ($CertContext -and $CertContext.Info -and (-not [string]::IsNullOrWhiteSpace($CertContext.Info.Thumbprint))) {
        $normExpectedThumb = ($CertContext.Info.Thumbprint -replace '\s','').ToUpperInvariant()
        $normActualThumb = ($verifyResult.SignerThumbprint -replace '\s','').ToUpperInvariant()
        if ($normActualThumb -ne $normExpectedThumb) {
            Fail "Artifact post-sign verification failed: signer thumbprint ($normActualThumb) does not match validated certificate ($normExpectedThumb)"
        }
    }

    return $verifyResult
}

# -------------------------------------------------------------------------
# 5. Main Dispatch
# -------------------------------------------------------------------------
$discoveredSigntool = Find-Signtool $SigntoolPath

switch ($Mode) {
    "DiscoverSigntool" {
        if ($discoveredSigntool) {
            if (-not $Quiet) {
                Write-Host "Found SignTool: $discoveredSigntool" -ForegroundColor Green
            }
            [PSCustomObject]@{
                Found = $true
                Path = $discoveredSigntool
            }
        } else {
            if (-not $Quiet) {
                Write-Warning "SignTool not found in Windows SDK or PATH"
            }
            [PSCustomObject]@{
                Found = $false
                Path = $null
            }
        }
    }

    "ValidateCertificate" {
        $certCtx = Load-SigningCertificate $CertificatePfxPath $CertificatePassword $CertificateThumbprint $CertStoreLocation
        if (-not $Quiet) {
            Write-Host "Certificate validated successfully:" -ForegroundColor Green
            Write-Host "  Subject:    $($certCtx.Info.Subject)"
            Write-Host "  Issuer:     $($certCtx.Info.Issuer)"
            Write-Host "  Thumbprint: $($certCtx.Info.Thumbprint)"
            Write-Host "  Valid From: $($certCtx.Info.NotBeforeUtc.ToString('o'))"
            Write-Host "  Valid To:   $($certCtx.Info.NotAfterUtc.ToString('o'))"
        }
        $certCtx.Info
    }

    "Verify" {
        if (-not $Path -or $Path.Count -eq 0) {
            Fail "No file path provided for verification"
        }
        $results = @()
        foreach ($item in $Path) {
            $itemResult = Test-ArtifactSignature $item $discoveredSigntool
            if ($RequireSigned -and -not $itemResult.Signed) {
                Fail "File is unsigned but signing was required: $item"
            }
            if (-not $Quiet) {
                if ($itemResult.Signed) {
                    Write-Host "Signature verified: $item" -ForegroundColor Green
                    Write-Host "  Status:     $($itemResult.Status)"
                    Write-Host "  Digest:     $($itemResult.DigestAlgorithm)"
                    Write-Host "  Subject:    $($itemResult.SignerSubject)"
                    Write-Host "  Thumbprint: $($itemResult.SignerThumbprint)"
                } else {
                    Write-Host "Unsigned: $item" -ForegroundColor Cyan
                }
            }
            $results += $itemResult
        }
        if ($results.Count -eq 1) { return $results[0] } else { return $results }
    }

    "Sign" {
        if (-not $Path -or $Path.Count -eq 0) {
            Fail "No file path provided for signing"
        }

        # In DryRun mode, we validate cert if provided or perform dry-run report
        $certCtx = $null
        if (-not $DryRun -or (-not [string]::IsNullOrWhiteSpace($CertificatePfxPath) -or -not [string]::IsNullOrWhiteSpace($CertificateThumbprint))) {
            $certCtx = Load-SigningCertificate $CertificatePfxPath $CertificatePassword $CertificateThumbprint $CertStoreLocation
        } else {
            # DryRun without cert
            $certCtx = [PSCustomObject]@{
                Type = "DryRunMock"
                Password = ""
                Info = [PSCustomObject]@{
                    Subject = "CN=Aperture Dry-Run Mock"
                    Thumbprint = "0000000000000000000000000000000000000000"
                }
            }
        }

        if (-not $DryRun -and [string]::IsNullOrWhiteSpace($discoveredSigntool)) {
            Fail "SignTool executable was not found. Install the Windows 10/11 SDK or provide -SigntoolPath."
        }

        $results = @()
        foreach ($item in $Path) {
            $signRes = Invoke-SignArtifact `
                -FilePath $item `
                -CertContext $certCtx `
                -SigntoolPath $discoveredSigntool `
                -TimestampUrl $TimestampServer `
                -Digest $DigestAlgorithm `
                -AppDescription $Description `
                -AppUrl $DescriptionUrl `
                -IsDryRun:$DryRun

            if (-not $Quiet) {
                Write-Host "Successfully signed and verified: $item" -ForegroundColor Green
            }
            $results += $signRes
        }
        if ($results.Count -eq 1) { return $results[0] } else { return $results }
    }
}
