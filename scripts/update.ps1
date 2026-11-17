[CmdletBinding()]
param(
    [string]$InstallDirectory = $PSScriptRoot,
    [switch]$NoLaunch,
    [switch]$Interactive,
    [string]$PackageZip = '',
    [string]$ExpectedSha256 = '',
    [string]$ExpectedVersion = '',
    [int]$ParentPid = 0,
    [string]$StatusFile = '',
    [ValidateSet('', 'stable', 'beta', 'nightly')]
    [string]$Channel = '',
    [switch]$PrivateMode
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor 3072 }
catch { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 }
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
. (Join-Path $PSScriptRoot 'runtime_update.ps1')
$temp = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-download-' + [guid]::NewGuid().ToString('N'))

function Write-UpdateStatus([string]$Path, [string]$Phase, [string]$Message = '', [string]$VersionText = '') {
    if (-not $Path) { return }
    if ($Phase -ne 'failed' -and (Read-UpdateStatusPhase $Path) -eq 'cancelled') { return }
    $tmp = "$Path.tmp.$([guid]::NewGuid().ToString('N'))"
    try {
        $parent = Split-Path -Parent $Path
        if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        $payload = [ordered]@{
            phase = $Phase
            version = $VersionText
            error = $Message
            updatedUtc = ([DateTime]::UtcNow.ToString('o'))
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText($tmp, $payload, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($tmp, $Path, [NullString]::Value) }
        else { [IO.File]::Move($tmp, $Path) }
    } catch {
        Write-Warning "Could not write update status file '$Path': $($_.Exception.Message)"
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
}

function Read-UpdateStatusPhase([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    try {
        $obj = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        if ($obj -and $obj.PSObject.Properties['phase']) { return [string]$obj.phase }
    } catch {}
    return ''
}

function Show-InteractiveMessage([string]$Message, [bool]$IsError) {
    if (-not $Interactive) { return }
    Add-Type -AssemblyName System.Windows.Forms
    $icon = if ($IsError) { [Windows.Forms.MessageBoxIcon]::Error } else { [Windows.Forms.MessageBoxIcon]::Information }
    [void][Windows.Forms.MessageBox]::Show($Message, 'Aperture Update', [Windows.Forms.MessageBoxButtons]::OK, $icon, [Windows.Forms.MessageBoxDefaultButton]::Button1, [Windows.Forms.MessageBoxOptions]::DefaultDesktopOnly)
}

function Expand-ApertureZipSafely([string]$ZipPath, [string]$PackageDir) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    New-Item -ItemType Directory -Path $PackageDir | Out-Null
    $buffer = New-Object byte[] 65536
    try {
        $total = 0L
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            # Windows PowerShell 5.1 writes ZIP entries with backslashes.
            $name = $entry.FullName.Replace('\','/')
            if ($name -in @('fonts/','locales/','data/')) { continue }
            if ($name -ne 'manifest.json' -and -not (Test-ApertureRuntimePath $name)) { throw "Unexpected archive path: $name" }
            if (-not $seen.Add($name)) { throw "Duplicate archive path: $name" }
            if ($entry.Length -gt 400MB) { throw 'Expanded release exceeds the size limit.' }
            $target = Join-Path $PackageDir $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            $inStream = $entry.Open()
            $outStream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                while (($read = $inStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $total += $read
                    if ($total -gt 400MB) { throw 'Expanded release exceeds the size limit.' }
                    $outStream.Write($buffer, 0, $read)
                }
            } finally {
                $outStream.Dispose()
                $inStream.Dispose()
            }
        }
    } finally { $archive.Dispose() }
}

function Get-HttpStatusCodeFromException($ErrorRecord) {
    try {
        $ex = $ErrorRecord.Exception
        if ($ex.Response -and $ex.Response.StatusCode) { return [int]$ex.Response.StatusCode }
        if ($ex.InnerException -and $ex.InnerException.Response -and $ex.InnerException.Response.StatusCode) { return [int]$ex.InnerException.Response.StatusCode }
    } catch {}
    return 0
}

function Resolve-HexSha256Token([string]$Text) {
    if (-not $Text) { return '' }
    if ($Text -match '(?i)\b([a-f0-9]{64})\b') { return $Matches[1].ToLowerInvariant() }
    return ''
}

$parentClosed = $false
$installedVersion = ''
$resolvedZipStageDir = ''
try {
    $sourcePath = Join-Path $InstallDirectory 'release-source.json'
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw 'Missing release-source.json in the Aperture folder. Reinstall a portable release before updating.'
    }
    $source = Get-Content -LiteralPath $sourcePath -Raw | ConvertFrom-Json
    $repo = [string]$source.repository
    if ($repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'This local build has no GitHub repository yet. Push the project to GitHub and publish its first release using the Release workflow.' }

    $package = Join-Path $temp 'package'
    if ($PackageZip) {
        if ((Read-UpdateStatusPhase $StatusFile) -eq 'cancelled') { throw 'Update cancelled before installation.' }
        Write-UpdateStatus $StatusFile 'verifying' '' $ExpectedVersion
        $zipPath = [IO.Path]::GetFullPath($PackageZip)
        if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) { throw "Staged update archive was not found: $zipPath" }
        if ($ExpectedSha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Missing expected SHA-256 checksum for staged update.' }
        $zipSize = (Get-Item -LiteralPath $zipPath).Length
        if ($zipSize -le 0 -or $zipSize -gt 200MB) { throw 'Staged update archive size is outside allowed bounds.' }
        $actualSha = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
        if ($actualSha -ine $ExpectedSha256) { throw 'Staged update package failed SHA-256 verification.' }

        $stageParent = Split-Path -Parent $zipPath
        if ((Split-Path -Leaf $stageParent) -like 'Aperture_Update_*') { $resolvedZipStageDir = $stageParent }

        if ((Read-UpdateStatusPhase $StatusFile) -eq 'cancelled') { throw 'Update cancelled before installation.' }
        Expand-ApertureZipSafely $zipPath $package
        $manifest = Read-AperturePackage $package
        $nextSource = Get-Content -LiteralPath (Join-Path $package 'release-source.json') -Raw | ConvertFrom-Json
        if ($nextSource.repository -cne $repo) { throw 'Package repository does not match release-source.json.' }
        if ($ExpectedVersion) {
            $expectedNumeric = ($ExpectedVersion -split '[-+]')[0]
            if ($manifest.version -ne $expectedNumeric -or $nextSource.version -ne $expectedNumeric) {
                throw 'Package version does not match the verified release.'
            }
        }
        if ($source.PSObject.Properties['version'] -and $source.version -and ([version]$manifest.version -le [version]$source.version)) {
            throw 'Package version is not newer than the installed Aperture version.'
        }
        $installedVersion = [string]$manifest.version

        if ((Read-UpdateStatusPhase $StatusFile) -eq 'cancelled') { throw 'Update cancelled before closing Aperture.' }
        Write-UpdateStatus $StatusFile 'ready_to_install' '' $installedVersion

        if ($ParentPid -gt 0) {
            $deadline = [DateTime]::UtcNow.AddSeconds(30)
            $parentProc = $null
            try { $parentProc = Get-Process -Id $ParentPid -ErrorAction SilentlyContinue } catch {}
            while ($parentProc -and -not $parentProc.HasExited) {
                if ((Read-UpdateStatusPhase $StatusFile) -eq 'cancelled') { throw 'Update was cancelled by the user.' }
                if ([DateTime]::UtcNow -ge $deadline) { throw 'Aperture did not finish closing within 30 seconds.' }
                Start-Sleep -Milliseconds 100
                try { $parentProc.Refresh() } catch {}
            }
            $parentClosed = $true
        }
        Install-AperturePackage -Package $package -Destination $InstallDirectory
        Write-UpdateStatus $StatusFile 'completed' '' $installedVersion
        if ($resolvedZipStageDir -and (Test-Path -LiteralPath $resolvedZipStageDir)) {
            Remove-Item -LiteralPath $resolvedZipStageDir -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (-not $NoLaunch) { Start-Process -FilePath (Join-Path $InstallDirectory 'Aperture.exe') -WorkingDirectory $InstallDirectory }
        return
    }

    Write-Host 'Checking for updates...'
    $headers = @{ 'User-Agent' = 'Aperture-Updater'; 'Accept' = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $release = $null
    $useListEndpoint = ($Channel -eq 'beta' -or $Channel -eq 'nightly')
    if (-not $useListEndpoint) {
        try {
            $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers $headers -TimeoutSec 30
        } catch {
            $code = Get-HttpStatusCodeFromException $_
            if ($code -eq 404) {
                $list = @()
                try { $list = @(Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases?per_page=10" -Headers $headers -TimeoutSec 30) }
                catch {
                    $listCode = Get-HttpStatusCodeFromException $_
                    if ($listCode -eq 404) { throw "Update repository '$repo' was not found or is private (HTTP 404)." }
                    throw
                }
                if ($list.Count -eq 0) {
                    Write-Host "No releases have been published for '$repo' yet."
                    Show-InteractiveMessage "No releases have been published for '$repo' yet." $false
                    return
                }
                Write-Host 'No stable release is published yet (only prerelease or draft releases were found).'
                Show-InteractiveMessage 'No stable release is published yet (only prerelease or draft releases were found).' $false
                return
            }
            if ($code -eq 403 -or $code -eq 429) { throw "GitHub API rate limit exceeded (HTTP $code). Please try again later." }
            throw
        }
    } else {
        try {
            $releases = @(Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases?per_page=15" -Headers $headers -TimeoutSec 30)
        } catch {
            $code = Get-HttpStatusCodeFromException $_
            if ($code -eq 404) { throw "Update repository '$repo' was not found or is private (HTTP 404)." }
            if ($code -eq 403 -or $code -eq 429) { throw "GitHub API rate limit exceeded (HTTP $code). Please try again later." }
            throw
        }
        $release = $releases | Where-Object { -not $_.draft -and ($Channel -eq 'nightly' -or $_.tag_name -notmatch 'nightly|alpha|dev') } | Select-Object -First 1
        if (-not $release) {
            Write-Host "No eligible releases found for channel '$Channel'."
            Show-InteractiveMessage "No eligible releases found for channel '$Channel'." $false
            return
        }
    }

    $tagNoPrefix = ([string]$release.tag_name) -replace '^v',''
    $numericTag = ($tagNoPrefix -split '[-+]')[0]
    if ($numericTag -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Release tag must contain a four-part Aperture version.' }
    $versionText = $numericTag
    if ([version]$versionText -le [version]$source.version) {
        Write-Host 'Aperture is up to date.'
        Show-InteractiveMessage 'Aperture is up to date.' $false
        return
    }

    $assetName = "Aperture-$versionText-windows-x64.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $assetName })
    if ($assets.Count -ne 1) {
        $assets = @($release.assets | Where-Object { $_.name -like 'Aperture-*-win*.zip' -or $_.name -like 'Aperture-*.zip' } | Select-Object -First 1)
    }
    if ($assets.Count -ne 1) { throw 'The release does not contain a compatible Aperture package.' }
    $asset = $assets[0]
    if ($asset.size -le 0 -or $asset.size -gt 200MB) { throw 'The release package exceeds the download size limit.' }

    $expectedHash = ''
    if ($asset.PSObject.Properties['digest'] -and $asset.digest -match '^sha256:([a-fA-F0-9]{64})$') {
        $expectedHash = $Matches[1].ToLowerInvariant()
    } else {
        $sidecar = @($release.assets | Where-Object { $_.name -ceq "$($asset.name).sha256" } | Select-Object -First 1)
        if ($sidecar.Count -eq 1) {
            $sidecarUri = [uri]$sidecar[0].browser_download_url
            if ($sidecarUri.Scheme -eq 'https' -and $sidecarUri.Host -eq 'github.com' -and $sidecarUri.AbsolutePath.StartsWith("/$repo/releases/download/", [StringComparison]::Ordinal)) {
                $sidecarPath = Join-Path $temp 'package.sha256'
                New-Item -ItemType Directory -Path $temp -Force | Out-Null
                Invoke-WebRequest -UseBasicParsing -Uri $sidecarUri -OutFile $sidecarPath -TimeoutSec 30
                $expectedHash = Resolve-HexSha256Token (Get-Content -LiteralPath $sidecarPath -Raw)
            }
        }
    }
    if ($expectedHash -notmatch '^[a-fA-F0-9]{64}$') { throw 'The release package is missing its SHA-256 checksum.' }

    $url = [uri]$asset.browser_download_url
    if ($url.Scheme -ne 'https' -or $url.Host -ne 'github.com' -or -not $url.AbsolutePath.StartsWith("/$repo/releases/download/", [StringComparison]::Ordinal)) { throw 'Unexpected release download location.' }
    if (-not (Test-Path -LiteralPath $temp)) { New-Item -ItemType Directory -Path $temp | Out-Null }
    $zip = Join-Path $temp 'release.zip'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -TimeoutSec 300
    if ((Get-Item -LiteralPath $zip).Length -ne $asset.size -or (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ine $expectedHash) { throw 'Downloaded release failed checksum verification.' }
    Expand-ApertureZipSafely $zip $package
    $manifest = Read-AperturePackage $package
    $nextSource = Get-Content -LiteralPath (Join-Path $package 'release-source.json') -Raw | ConvertFrom-Json
    if ($manifest.version -ne $versionText -or $nextSource.version -ne $versionText -or $nextSource.repository -cne $repo) { throw 'Package version or repository does not match the release.' }
    Install-AperturePackage -Package $package -Destination $InstallDirectory
    if (-not $NoLaunch) { Start-Process -FilePath (Join-Path $InstallDirectory 'Aperture.exe') -WorkingDirectory $InstallDirectory }
} catch {
    $errMsg = $_.Exception.Message
    Write-Host $errMsg -ForegroundColor Red
    Write-UpdateStatus $StatusFile 'failed' $errMsg $installedVersion
    if ($resolvedZipStageDir -and (Test-Path -LiteralPath $resolvedZipStageDir)) {
        Remove-Item -LiteralPath $resolvedZipStageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($parentClosed -and $PackageZip) {
        if (-not $PrivateMode) {
            try {
                $errDir = Join-Path (Join-Path $InstallDirectory 'cache') 'updater-tmp'
                New-Item -ItemType Directory -Path $errDir -Force | Out-Null
                $errFile = Join-Path $errDir 'last_update_error.json'
                $errPayload = [ordered]@{ version = $installedVersion; error = $errMsg; timestampUtc = ([DateTime]::UtcNow.ToString('o')) } | ConvertTo-Json -Compress
                [IO.File]::WriteAllText($errFile, $errPayload, [Text.UTF8Encoding]::new($false))
            } catch {}
        }
        if (-not $NoLaunch -and (Test-Path -LiteralPath (Join-Path $InstallDirectory 'Aperture.exe'))) {
            Start-Process -FilePath (Join-Path $InstallDirectory 'Aperture.exe') -WorkingDirectory $InstallDirectory
        }
    }
    Show-InteractiveMessage $errMsg $true
    exit 1
} finally {
    $full = [IO.Path]::GetFullPath($temp)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-download-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}