# Exercise the complete download/update entry point with only HTTP replaced by a fixture.
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-github-test-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
$installed = Join-Path $root 'installed'
function Write-Fixture([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
try {
    foreach ($name in @('Aperture.exe','libmpv-2.dll','Update.cmd','update.ps1','runtime_update.ps1','fonts/Inter.ttf','locales/en-US.json')) { Write-Fixture (Join-Path $package $name) "new:$name" }
    Write-Fixture (Join-Path $package 'release-source.json') '{"repository":"example/aperture","version":"2.0.1.0"}'
    $files = @(Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
        @{ path=$_.FullName.Substring($package.Length + 1).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash }
    })
    @{ schemaVersion=1; application='Aperture'; architecture='x64'; version='2.0.1.0'; files=$files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
    Write-Fixture (Join-Path $installed 'Aperture.exe') 'old-exe'
    Write-Fixture (Join-Path $installed 'release-source.json') '{"repository":"example/aperture","version":"2.0.0.0"}'
    Write-Fixture (Join-Path $installed 'data/session.json') 'saved playback position'
    # Include both update scripts next to installed runtime for full -PackageZip handoff tests
    Copy-Item -LiteralPath (Join-Path $repoRoot 'scripts/update.ps1') -Destination (Join-Path $installed 'update.ps1') -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot 'scripts/runtime_update.ps1') -Destination (Join-Path $installed 'runtime_update.ps1') -Force
    $zip = Join-Path $root 'release.zip'
    Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip
    $zipSha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    $driver = Join-Path $root 'driver.ps1'
    @'
param($UpdateScript, $Installed, $FixtureZip, $Scenario)
$ErrorActionPreference = 'Stop'
function New-Http404Exception {
    $inner = New-Object Net.WebException('The remote server returned an error: (404) Not Found.')
    Add-Member -InputObject $inner -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{ StatusCode = 404 }) -Force
    return $inner
}
function Invoke-RestMethod {
    param($Uri, $Headers, $TimeoutSec)
    if ($Scenario -eq 'no_releases') {
        if ($Uri -eq 'https://api.github.com/repos/example/aperture/releases/latest') { throw (New-Http404Exception) }
        if ($Uri -like 'https://api.github.com/repos/example/aperture/releases?per_page=*') { return @() }
        throw "Unexpected endpoint: $Uri"
    }
    if ($Scenario -eq 'repo_missing') {
        throw (New-Http404Exception)
    }
    if ($Uri -ne 'https://api.github.com/repos/example/aperture/releases/latest') { throw 'Wrong repository endpoint' }
    $digest = 'sha256:' + (Get-FileHash -LiteralPath $FixtureZip).Hash.ToLowerInvariant()
    if ($Scenario -eq 'tampered') { $digest = 'sha256:' + ('0' * 64) }
    $asset = @{ name='Aperture-2.0.1.0-windows-x64.zip'; size=(Get-Item -LiteralPath $FixtureZip).Length; browser_download_url='https://github.com/example/aperture/releases/download/v2.0.1.0/Aperture-2.0.1.0-windows-x64.zip' }
    $assets = @($asset)
    if ($Scenario -eq 'sidecar') {
        $assets += @{ name='Aperture-2.0.1.0-windows-x64.zip.sha256'; size=64; browser_download_url='https://github.com/example/aperture/releases/download/v2.0.1.0/Aperture-2.0.1.0-windows-x64.zip.sha256' }
    } else {
        $asset.digest = $digest
    }
    return @{ tag_name='v2.0.1.0'; assets=$assets }
}
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, $Uri, $OutFile, $TimeoutSec)
    if ($Scenario -eq 'current') { throw 'An up-to-date copy must not download' }
    if ($Uri -like '*.sha256') {
        $hash = (Get-FileHash -LiteralPath $FixtureZip -Algorithm SHA256).Hash.ToLowerInvariant()
        [IO.File]::WriteAllText($OutFile, "$hash  Aperture-2.0.1.0-windows-x64.zip`n")
        return
    }
    Copy-Item -LiteralPath $FixtureZip -Destination $OutFile
}
& $UpdateScript -InstallDirectory $Installed -NoLaunch
if ($LASTEXITCODE) { exit $LASTEXITCODE }
'@ | Set-Content -LiteralPath $driver -Encoding UTF8
    $updater = Join-Path $repoRoot 'scripts/update.ps1'

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip tampered
    if ($LASTEXITCODE -ne 1) { throw 'Corrupt download was not rejected' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'old-exe') { throw 'Corrupt download changed runtime' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip repo_missing
    if ($LASTEXITCODE -ne 1) { throw 'Missing repository 404 did not fail clearly' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip no_releases
    if ($LASTEXITCODE -ne 0) { throw 'Public repo with zero releases failed unexpectedly' }

    # Pre-downloaded -PackageZip + -StatusFile failure (wrong expected checksum)
    $badStatusFile = Join-Path $root 'status_bad.json'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -InstallDirectory $installed -PackageZip $zip -ExpectedSha256 ('0' * 64) -ExpectedVersion '2.0.1.0' -StatusFile $badStatusFile -NoLaunch
    if ($LASTEXITCODE -ne 1) { throw 'Wrong ExpectedSha256 was accepted in -PackageZip mode' }
    $badStatus = Get-Content -LiteralPath $badStatusFile -Raw | ConvertFrom-Json
    if ($badStatus.phase -ne 'failed') { throw 'Status file did not report failed phase' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'old-exe') { throw 'Failed -PackageZip changed runtime' }

    # Pre-downloaded -PackageZip cancellation via StatusFile = cancelled
    $cancelStatusFile = Join-Path $root 'status_cancel.json'
    [IO.File]::WriteAllText($cancelStatusFile, '{"phase":"cancelled"}')
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -InstallDirectory $installed -PackageZip $zip -ExpectedSha256 $zipSha -ExpectedVersion '2.0.1.0' -StatusFile $cancelStatusFile -NoLaunch
    if ($LASTEXITCODE -ne 1) { throw 'Cancelled -PackageZip did not abort' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'old-exe') { throw 'Cancelled update changed runtime' }

    # Pre-downloaded -PackageZip handoff success & staging cleanup
    $stageDir = Join-Path $installed 'cache/updater-tmp/Aperture_Update_test'
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    $stagedZip = Join-Path $stageDir 'release.zip'
    Copy-Item -LiteralPath $zip -Destination $stagedZip
    $okStatusFile = Join-Path $root 'handoff_status_ok.json'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $updater -InstallDirectory $installed -PackageZip $stagedZip -ExpectedSha256 $zipSha -ExpectedVersion '2.0.1.0' -StatusFile $okStatusFile -NoLaunch
    if ($LASTEXITCODE -ne 0) { throw '-PackageZip handoff update failed' }
    $okStatus = Get-Content -LiteralPath $okStatusFile -Raw | ConvertFrom-Json
    if ($okStatus.phase -ne 'completed') { throw 'Status file did not reach completed phase' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'new:Aperture.exe') { throw 'New executable not installed by -PackageZip' }
    if (Test-Path -LiteralPath $stageDir) { throw 'Staging directory Aperture_Update_test was not cleaned up' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'data/session.json') -Raw) -ne 'saved playback position') { throw 'Session data changed' }

    # Reset installed executable + source to test sidecar fallback & standard flow
    Write-Fixture (Join-Path $installed 'Aperture.exe') 'old-exe'
    Write-Fixture (Join-Path $installed 'release-source.json') '{"repository":"example/aperture","version":"2.0.0.0"}'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip sidecar
    if ($LASTEXITCODE -ne 0) { throw 'Sidecar .sha256 fallback update failed' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'new:Aperture.exe') { throw 'Sidecar update did not install new executable' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip current
    if ($LASTEXITCODE -ne 0) { throw 'Up-to-date check failed' }
    Write-Host 'PASS: GitHub update entry point, -PackageZip handoff, sidecar .sha256 fallback, 404 disambiguation, checksum/cancel rejection, and data retention.' -ForegroundColor Green
} finally {
    $full = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-github-test-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}