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
    $readOnlyData = Join-Path $target 'data/readonly.json'
    Write-Fixture $readOnlyData 'keep-readonly'
    [IO.File]::SetAttributes($readOnlyData, [IO.FileAttributes]::ReadOnly)
    $before = @{}
    Get-ChildItem -LiteralPath $target -Recurse -File | Where-Object Extension -notin @('.exe','.dll') | ForEach-Object { $before[$_.FullName] = (Get-FileHash -LiteralPath $_.FullName).Hash }
    Install-AperturePackage $package $target
    Assert ((Get-Content -LiteralPath (Join-Path $target 'Aperture.exe') -Raw) -eq 'new:Aperture.exe') 'Executable not installed'
    foreach ($file in $before.Keys) { Assert ((Get-FileHash -LiteralPath $file).Hash -eq $before[$file]) "Saved file changed: $file" }
    $backup = Get-ChildItem -LiteralPath (Join-Path $target 'update-backups') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'complete') } | Select-Object -First 1
    Assert ((Get-Content -LiteralPath (Join-Path $backup.FullName 'runtime/Aperture.exe') -Raw) -eq 'old-exe') 'Previous runtime not backed up'
    Assert (Test-Path -LiteralPath (Join-Path $backup.FullName 'data/session.json')) 'Playback session not backed up'
    Install-AperturePackage $package $target
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $target 'update-backups') -Directory | Where-Object Name -ne '.objects').Count -eq 1) 'Identical reinstall created a redundant snapshot'
    # An unchanged dependency does not need replacement, even if another reader holds it.
    $lock = [IO.File]::Open((Join-Path $target 'libmpv-2.dll'), 'Open', 'Read', 'Read')
    try {
        Write-Fixture (Join-Path $package 'Aperture.exe') 'revision-1'
        Write-Manifest
        Install-AperturePackage $package $target
    } finally { $lock.Dispose() }
    2..3 | ForEach-Object {
        Write-Fixture (Join-Path $package 'Aperture.exe') "revision-$_"
        Write-Manifest
        Install-AperturePackage $package $target
    }
    $backups = Join-Path $target 'update-backups'
    $snapshots = @(Get-ChildItem -LiteralPath $backups -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'complete') })
    Assert ($snapshots.Count -eq 3) 'Backup retention failed'
    $objects = Join-Path $backups '.objects'
    $dllHash = (Get-FileHash -LiteralPath (Join-Path $target 'libmpv-2.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $blob = Join-Path $objects ($dllHash + '.bin')
    $dllBytes = [IO.File]::ReadAllBytes($blob)
    # Verify actual shared storage and that it never aliases the installed runtime.
    [IO.File]::WriteAllText($blob, 'shared-storage-probe')
    foreach ($snapshot in $snapshots) {
        Assert ((Get-Content -LiteralPath (Join-Path $snapshot.FullName 'runtime/libmpv-2.dll') -Raw) -eq 'shared-storage-probe') 'Snapshot dependencies were copied instead of sharing storage'
        Assert (Test-Path -LiteralPath (Join-Path $snapshot.FullName 'data/session.json')) 'Complete snapshot lost durable data'
        Assert (([IO.File]::GetAttributes((Join-Path $snapshot.FullName 'data/readonly.json')) -band [IO.FileAttributes]::ReadOnly) -ne 0) 'Read-only recovery file lost its protection'
    }
    Assert ((Get-Content -LiteralPath (Join-Path $target 'libmpv-2.dll') -Raw) -eq 'new:libmpv-2.dll') 'Live runtime shares mutable backup storage'
    $session = Join-Path $target 'data/session.json'
    $sessionHash = (Get-FileHash -LiteralPath $session -Algorithm SHA256).Hash.ToLowerInvariant()
    $sessionBlob = Join-Path $objects ($sessionHash + '.bin')
    $sessionBytes = [IO.File]::ReadAllBytes($sessionBlob)
    [IO.File]::WriteAllText($sessionBlob, 'shared-data-probe')
    foreach ($snapshot in $snapshots) {
        Assert ((Get-Content -LiteralPath (Join-Path $snapshot.FullName 'data/session.json') -Raw) -eq 'shared-data-probe') 'Unchanged saved data was duplicated across snapshots'
    }
    Assert ((Get-Content -LiteralPath $session -Raw) -eq 'keep:data/session.json') 'Live saved data shares backup storage'
    [IO.File]::WriteAllBytes($sessionBlob, $sessionBytes)
    Write-Fixture (Join-Path $package 'Aperture.exe') 'must-not-install'
    Write-Manifest
    $rejected = $false
    try { Install-AperturePackage $package $target } catch { $rejected = $_.Exception.Message -like '*Damaged rollback object*' }
    Assert $rejected 'Corrupt shared rollback object was accepted'
    Assert ((Get-Content -LiteralPath (Join-Path $target 'Aperture.exe') -Raw) -eq 'revision-3') 'Corrupt backup changed the installed executable'
    [IO.File]::WriteAllBytes($blob, $dllBytes)
    foreach ($oldText in @('old-exe','old-dll')) {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $oldHash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($oldText))).Replace('-','').ToLowerInvariant() }
        finally { $sha.Dispose() }
        Assert (-not (Test-Path -LiteralPath (Join-Path $objects ($oldHash + '.bin')))) 'Expired snapshot object was retained'
    }
    # Exercise the real portable fallback without requiring a second file system.
    $linkImplementation = (Get-Command New-ApertureBackupLink).ScriptBlock
    try {
        function New-ApertureBackupLink([string]$Link, [string]$Existing) { throw [PlatformNotSupportedException]::new('Fixture file system has no hard links') }
        $fallbackObjects = Join-Path $root 'fallback-objects'
        New-Item -ItemType Directory -Path $fallbackObjects | Out-Null
        $source = Join-Path $root 'fallback-source'
        $saved = Join-Path $root 'fallback-saved'
        Write-Fixture $source 'portable-copy'
        $verified = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $linkSupport = $null
        Save-ApertureSnapshotFile $source $saved $fallbackObjects $verified ([ref]$linkSupport)
        Assert ($linkSupport -eq $false) 'Unsupported hard links were not remembered during installation'
        Save-ApertureSnapshotFile $saved $saved $fallbackObjects $verified ([ref]$linkSupport)
        Assert ((Get-Content -LiteralPath $saved -Raw) -eq 'portable-copy') 'Portable snapshot fallback lost recovery bytes'
        Assert (@(Get-ChildItem -LiteralPath $fallbackObjects -File).Count -eq 0) 'Portable fallback retained a redundant object copy'
    } finally { Set-Item -LiteralPath Function:\New-ApertureBackupLink -Value $linkImplementation }
    # A real file lock rejects replacement after the first file changed.
    Write-Fixture (Join-Path $package 'Aperture.exe') 'next-exe'
    Write-Fixture (Join-Path $package 'libmpv-2.dll') 'next-dll'
    Write-Manifest
    $lock = [IO.File]::Open((Join-Path $target 'libmpv-2.dll'), 'Open', 'Read', 'Read')
    $failed = $false
    try { Install-AperturePackage $package $target } catch { $failed = $true } finally { $lock.Dispose() }
    Assert $failed 'Locked runtime should fail'
    Assert ((Get-Content -LiteralPath (Join-Path $target 'Aperture.exe') -Raw) -eq 'revision-3') 'Failed update did not restore executable'
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
    Write-Host 'PASS: real installation, data retention, deduplicated backups, no-op reinstall, portable fallback, locked-file rollback, tampering, and protected paths.' -ForegroundColor Green
} finally {
    $full = [IO.Path]::GetFullPath($root)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\Aperture-update-test-'
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force }
}
