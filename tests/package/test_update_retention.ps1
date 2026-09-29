$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../scripts/runtime_update.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-update-test-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
$target = Join-Path $root 'installed'
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Write-Fixture([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
function Write-Manifest {
    $files = @('Aperture.exe','libmpv-2.dll','Update.cmd','update.ps1','runtime_update.ps1','release-source.json','fonts/Inter.ttf','locales/en-US.json') | ForEach-Object {
        $file = Get-Item -LiteralPath (Join-Path $package $_)
        @{ path = $_; size = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash }
    }
    @{ schemaVersion = 1; application = 'Aperture'; architecture = 'x64'; version = '2.0.1.0'; files = @($files) } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
}
try {
    foreach ($name in @('Aperture.exe','libmpv-2.dll','Update.cmd','update.ps1','runtime_update.ps1','release-source.json','fonts/Inter.ttf','locales/en-US.json')) { Write-Fixture (Join-Path $package $name) "new:$name" }
    Write-Manifest
    Write-Fixture (Join-Path $target 'Aperture.exe') 'old-exe'
    Write-Fixture (Join-Path $target 'libmpv-2.dll') 'old-dll'
    foreach ($name in @('data/settings.json','data/library.json','data/playlists.json','data/history.json','data/session.json','data/backups/settings.json.bak.1','mpv.conf','cache/keep.bin','custom.txt')) { Write-Fixture (Join-Path $target $name) "keep:$name" }
    $before = @{}
    Get-ChildItem -LiteralPath $target -Recurse -File | Where-Object Extension -notin @('.exe','.dll') | ForEach-Object { $before[$_.FullName] = (Get-FileHash -LiteralPath $_.FullName).Hash }
    Install-AperturePackage $package $target
    Assert ((Get-Content -LiteralPath (Join-Path $target 'Aperture.exe') -Raw) -eq 'new:Aperture.exe') 'Executable not installed'
    foreach ($file in $before.Keys) { Assert ((Get-FileHash -LiteralPath $file).Hash -eq $before[$file]) "Saved file changed: $file" }
    $backup = Get-ChildItem -LiteralPath (Join-Path $target 'update-backups') -Directory | Select-Object -First 1
    Assert ((Get-Content -LiteralPath (Join-Path $backup.FullName 'runtime/Aperture.exe') -Raw) -eq 'old-exe') 'Previous runtime not backed up'
    Assert (Test-Path -LiteralPath (Join-Path $backup.FullName 'data/session.json')) 'Playback session not backed up'
    1..3 | ForEach-Object { Install-AperturePackage $package $target }
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $target 'update-backups') -Directory).Count -eq 3) 'Backup retention failed'
    # A real file lock rejects replacement after the first file changed.
    Write-Fixture (Join-Path $package 'Aperture.exe') 'next-exe'
    Write-Manifest
    $lock = [IO.File]::Open((Join-Path $target 'libmpv-2.dll'), 'Open', 'Read', 'Read')
    $failed = $false
    try { Install-AperturePackage $package $target } catch { $failed = $true } finally { $lock.Dispose() }
    Assert $failed 'Locked runtime should fail'
    Assert ((Get-Content -LiteralPath (Join-Path $target 'Aperture.exe') -Raw) -eq 'new:Aperture.exe') 'Failed update did not restore executable'
    foreach ($file in $before.Keys) { Assert ((Get-FileHash -LiteralPath $file).Hash -eq $before[$file]) 'Failed update changed saved data' }
    Write-Fixture (Join-Path $package 'Aperture.exe') 'tampered'
    $failed = $false
    try { Install-AperturePackage $package $target } catch { $failed = $true }
    Assert $failed 'Tampered package accepted'
    Write-Manifest
    foreach ($bad in @('data/settings.json','../outside.exe','fonts/../../outside.ttf','fonts\Inter.ttf','mpv.conf','C:/outside.exe')) {
        Assert (-not (Test-ApertureRuntimePath $bad)) "Unsafe runtime path accepted: $bad"
    }
    Write-Fixture (Join-Path $package 'data/settings.json') 'unlisted private data'
    $failed = $false
    try { Read-AperturePackage $package } catch { $failed = $true }
    Assert $failed 'Package containing profile data accepted'
    Write-Host 'PASS: real installation, data retention, backups, locked-file rollback, tampering, and protected paths.' -ForegroundColor Green
} finally {
    $full = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-update-test-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
