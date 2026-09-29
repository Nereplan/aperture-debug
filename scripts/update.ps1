[CmdletBinding()]
param([string]$InstallDirectory = $PSScriptRoot, [switch]$NoLaunch, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
. (Join-Path $PSScriptRoot 'runtime_update.ps1')
$temp = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-download-' + [guid]::NewGuid().ToString('N'))
try {
    $source = Get-Content -LiteralPath (Join-Path $InstallDirectory 'release-source.json') -Raw | ConvertFrom-Json
    $repo = $source.repository
    if ($repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'This local build has no GitHub repository yet. Push the project to GitHub and publish its first release using the Release workflow.' }
    Write-Host 'Checking for updates...'
    $headers = @{ 'User-Agent' = 'Aperture-Updater'; 'Accept' = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers $headers -TimeoutSec 30
    $versionText = ([string]$release.tag_name) -replace '^v',''
    if ($versionText -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw 'Release tag must contain a four-part Aperture version.' }
    if ([version]$versionText -le [version]$source.version) {
        Write-Host 'Aperture is up to date.'
        if ($Interactive) { Add-Type -AssemblyName System.Windows.Forms; [void][Windows.Forms.MessageBox]::Show('Aperture is up to date.', 'Aperture Update') }
        return
    }
    $assetName = "Aperture-$versionText-windows-x64.zip"
    $assets = @($release.assets | Where-Object { $_.name -ceq $assetName })
    if ($assets.Count -ne 1) { throw 'The release does not contain a compatible Aperture package.' }
    $asset = $assets[0]
    if ($asset.size -le 0 -or $asset.size -gt 200MB -or $asset.digest -notmatch '^sha256:([a-fA-F0-9]{64})$') { throw 'The release package is missing its GitHub checksum or exceeds the download limit.' }
    $expectedHash = $Matches[1]
    $url = [uri]$asset.browser_download_url
    if ($url.Scheme -ne 'https' -or $url.Host -ne 'github.com' -or -not $url.AbsolutePath.StartsWith("/$repo/releases/download/", [StringComparison]::Ordinal)) { throw 'Unexpected release download location.' }
    New-Item -ItemType Directory -Path $temp | Out-Null
    $zip = Join-Path $temp 'release.zip'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip -TimeoutSec 300
    if ((Get-Item -LiteralPath $zip).Length -ne $asset.size -or (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ine $expectedHash) { throw 'Downloaded release failed checksum verification.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    $package = Join-Path $temp 'package'
    New-Item -ItemType Directory -Path $package | Out-Null
    try {
        $total = 0L
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $archive.Entries) {
            # Windows PowerShell 5.1 writes ZIP entries with backslashes.
            $name = $entry.FullName.Replace('\','/')
            if ($name -in @('fonts/','locales/','data/')) { continue }
            if ($name -ne 'manifest.json' -and -not (Test-ApertureRuntimePath $name)) { throw "Unexpected archive path: $name" }
            if (-not $seen.Add($name)) { throw "Duplicate archive path: $name" }
            $total += $entry.Length
            if ($total -gt 400MB) { throw 'Expanded release exceeds the size limit.' }
            $target = Join-Path $package $name
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
        }
    } finally { $archive.Dispose() }
    $manifest = Read-AperturePackage $package
    $nextSource = Get-Content -LiteralPath (Join-Path $package 'release-source.json') -Raw | ConvertFrom-Json
    if ($manifest.version -ne $versionText -or $nextSource.version -ne $versionText -or $nextSource.repository -cne $repo) { throw 'Package version or repository does not match the release.' }
    Install-AperturePackage -Package $package -Destination $InstallDirectory
    if (-not $NoLaunch) { Start-Process -FilePath (Join-Path $InstallDirectory 'Aperture.exe') -WorkingDirectory $InstallDirectory }
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    if ($Interactive) { Add-Type -AssemblyName System.Windows.Forms; [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Aperture Update') }
    exit 1
} finally {
    $full = [IO.Path]::GetFullPath($temp)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-download-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
