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
    $zip = Join-Path $root 'release.zip'
    Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip
    $driver = Join-Path $root 'driver.ps1'
    @'
param($UpdateScript, $Installed, $FixtureZip, $Scenario)
$ErrorActionPreference = 'Stop'
function Invoke-RestMethod {
    param($Uri, $Headers, $TimeoutSec)
    if ($Uri -ne 'https://api.github.com/repos/example/aperture/releases/latest') { throw 'Wrong repository endpoint' }
    $digest = 'sha256:' + (Get-FileHash -LiteralPath $FixtureZip).Hash.ToLowerInvariant()
    if ($Scenario -eq 'tampered') { $digest = 'sha256:' + ('0' * 64) }
    return @{ tag_name='v2.0.1.0'; assets=@(@{ name='Aperture-2.0.1.0-windows-x64.zip'; size=(Get-Item -LiteralPath $FixtureZip).Length; digest=$digest; browser_download_url='https://github.com/example/aperture/releases/download/v2.0.1.0/Aperture-2.0.1.0-windows-x64.zip' }) }
}
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, $Uri, $OutFile, $TimeoutSec)
    if ($Scenario -eq 'current') { throw 'An up-to-date copy must not download' }
    Copy-Item -LiteralPath $FixtureZip -Destination $OutFile
}
& $UpdateScript -InstallDirectory $Installed -NoLaunch
if ($LASTEXITCODE) { exit $LASTEXITCODE }
'@ | Set-Content -LiteralPath $driver -Encoding UTF8
    $updater = Join-Path $repoRoot 'scripts/update.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip tampered
    if ($LASTEXITCODE -ne 1) { throw 'Corrupt download was not rejected' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'old-exe') { throw 'Corrupt download changed runtime' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip success
    if ($LASTEXITCODE -ne 0) { throw 'Full GitHub update failed' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'Aperture.exe') -Raw) -ne 'new:Aperture.exe') { throw 'New executable not installed' }
    if ((Get-Content -LiteralPath (Join-Path $installed 'data/session.json') -Raw) -ne 'saved playback position') { throw 'Session data changed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driver $updater $installed $zip current
    if ($LASTEXITCODE -ne 0) { throw 'Up-to-date check failed' }
    Write-Host 'PASS: GitHub update entry point, full ZIP deployment, checksum rejection, data retention, and no-op when current.' -ForegroundColor Green
} finally {
    $full = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-github-test-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
