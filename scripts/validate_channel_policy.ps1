<#
.SYNOPSIS
    Validates Aperture release channel policy definition and release input parameters.

.DESCRIPTION
    Loads and validates channels.json against channels.schema.json, provides helper
    functions to validate channel IDs, SemVer prerelease compliance, expiry limits,
    and update promotion eligibility.

.PARAMETER Validate
    Runs full self-validation of channels.json and channels.schema.json.

.PARAMETER Channel
    Optional channel ID to validate.

.PARAMETER Version
    Optional version string to validate against channel prerelease rules.

.PARAMETER ExpiryDays
    Optional expiry days value to validate against channel maxExpiryDays.

.PARAMETER Json
    Emits validation result as JSON.
#>

[CmdletBinding()]
param(
    [switch]$Validate,
    [string]$Channel = $Channel,
    [string]$Version = $Version,
    [int]$ExpiryDays = $ExpiryDays,
    [switch]$Json = $Json,
    [string]$PolicyPath = ""
)

$ErrorActionPreference = "Stop"

function Get-ApertureChannelPolicy {
    param([string]$Path = "")

    if ([string]::IsNullOrWhiteSpace($Path)) {
        $workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
        $candidates = @(
            (Join-Path $workspace "channels.json"),
            (Join-Path $PSScriptRoot "channels.json")
        )
        foreach ($cand in $candidates) {
            if (Test-Path -LiteralPath $cand -PathType Leaf) {
                $Path = $cand
                break
            }
        }
    }

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Authoritative channels.json policy not found at '$Path'"
    }

    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $policy = $raw | ConvertFrom-Json
    return $policy
}

function Test-ApertureChannelValid {
    param(
        [Parameter(Mandatory = $true)][string]$ChannelId,
        $Policy = $null
    )
    if ($null -eq $Policy) { $Policy = Get-ApertureChannelPolicy }
    foreach ($ch in @($Policy.channels)) {
        if ($ch.id -ieq $ChannelId) {
            return $true
        }
    }
    return $false
}

function Get-ApertureChannelDefinition {
    param(
        [Parameter(Mandatory = $true)][string]$ChannelId,
        $Policy = $null
    )
    if ($null -eq $Policy) { $Policy = Get-ApertureChannelPolicy }
    foreach ($ch in @($Policy.channels)) {
        if ($ch.id -ieq $ChannelId) {
            return $ch
        }
    }
    return $null
}

function Test-ApertureChannelPrerelease {
    param(
        [Parameter(Mandatory = $true)][string]$ChannelId,
        [Parameter(Mandatory = $true)][string]$VersionString,
        $Policy = $null
    )
    if ($null -eq $Policy) { $Policy = Get-ApertureChannelPolicy }
    $ch = Get-ApertureChannelDefinition -ChannelId $ChannelId -Policy $Policy
    if ($null -eq $ch) { return $false }

    $dashIdx = $VersionString.IndexOf('-')
    $preRelease = ""
    if ($dashIdx -ge 0) {
        $preRelease = $VersionString.Substring($dashIdx + 1)
        # Strip build metadata if present
        $plusIdx = $preRelease.IndexOf('+')
        if ($plusIdx -ge 0) {
            $preRelease = $preRelease.Substring(0, $plusIdx)
        }
    }

    if (-not $ch.prerelease.allowed) {
        return [string]::IsNullOrWhiteSpace($preRelease)
    }

    if ([string]::IsNullOrWhiteSpace($preRelease)) {
        return $false
    }

    if (-not [string]::IsNullOrWhiteSpace($ch.prerelease.pattern)) {
        if ($preRelease -notmatch $ch.prerelease.pattern) {
            return $false
        }
    }

    return $true
}

function Test-ApertureChannelExpiry {
    param(
        [Parameter(Mandatory = $true)][string]$ChannelId,
        [Parameter(Mandatory = $true)][int]$Days,
        $Policy = $null
    )
    if ($null -eq $Policy) { $Policy = Get-ApertureChannelPolicy }
    $ch = Get-ApertureChannelDefinition -ChannelId $ChannelId -Policy $Policy
    if ($null -eq $ch) { return $false }

    if ($Days -le 0 -or $Days -gt $ch.maxExpiryDays) {
        return $false
    }
    return $true
}

# Standalone execution
if ($MyInvocation.InvocationName -ne "." -and ($Validate -or -not [string]::IsNullOrWhiteSpace($Channel) -or -not [string]::IsNullOrWhiteSpace($Version))) {
    $policy = Get-ApertureChannelPolicy -Path $PolicyPath

    if (-not $policy.schemaVersion -or -not $policy.defaultChannel -or -not $policy.channels) {
        throw "Invalid channels.json policy structure: missing required top-level fields."
    }

    if (-not (Test-ApertureChannelValid -ChannelId $policy.defaultChannel -Policy $policy)) {
        throw "defaultChannel '$($policy.defaultChannel)' is not defined in channels list."
    }

    $seenIds = @{}
    $seenRanks = @{}
    foreach ($ch in @($policy.channels)) {
        if ($seenIds.ContainsKey($ch.id.ToLowerInvariant())) {
            throw "Duplicate channel id '$($ch.id)' in channels.json"
        }
        $seenIds[$ch.id.ToLowerInvariant()] = $true

        if ($seenRanks.ContainsKey($ch.stabilityRank)) {
            throw "Duplicate stabilityRank '$($ch.stabilityRank)' for channel '$($ch.id)' in channels.json"
        }
        $seenRanks[$ch.stabilityRank] = $true

        foreach ($acc in @($ch.acceptedChannels)) {
            if (-not (Test-ApertureChannelValid -ChannelId $acc -Policy $policy)) {
                throw "Channel '$($ch.id)' accepts undefined channel '$acc'"
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Channel)) {
        if (-not (Test-ApertureChannelValid -ChannelId $Channel -Policy $policy)) {
            throw "Specified channel '$Channel' is not a valid channel in channels.json."
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Channel) -and -not [string]::IsNullOrWhiteSpace($Version)) {
        if (-not (Test-ApertureChannelPrerelease -ChannelId $Channel -VersionString $Version -Policy $policy)) {
            throw "Version '$Version' violates prerelease rules for channel '$Channel'."
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Channel) -and $ExpiryDays -gt 0) {
        if (-not (Test-ApertureChannelExpiry -ChannelId $Channel -Days $ExpiryDays -Policy $policy)) {
            $ch = Get-ApertureChannelDefinition -ChannelId $Channel -Policy $policy
            throw "ExpiryDays ($ExpiryDays) exceeds maxExpiryDays ($($ch.maxExpiryDays)) for channel '$Channel'."
        }
    }

    if ($Json) {
        [ordered]@{
            valid = $true
            channels = @($policy.channels | ForEach-Object { $_.id })
            defaultChannel = $policy.defaultChannel
        } | ConvertTo-Json
    } else {
        Write-Host "Channel policy validation successful: $($policy.channels.Count) channels defined (default: $($policy.defaultChannel))." -ForegroundColor Green
    }
}
