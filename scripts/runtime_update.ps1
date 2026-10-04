# Shared package validation and transactional installation for local and downloaded releases.

function Assert-ApertureTree([string]$Root) {
    Set-StrictMode -Version Latest
    $item = Get-Item -LiteralPath $Root -Force
    for ($parent = $item; $null -ne $parent; $parent = $parent.Parent) {
        if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked paths are not supported: $($parent.FullName)" }
    }
    foreach ($entry in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked paths are not supported: $($entry.FullName)" }
    }
}

function Test-ApertureRuntimePath([string]$Path) {
    return $Path -cmatch '^(Aperture(\.Debug)?\.exe|Aperture\.pdb|libmpv-2\.dll|Update\.cmd|update\.ps1|runtime_update\.ps1|release-source\.json|fonts/[A-Za-z0-9_-]+\.ttf|locales/[A-Za-z0-9_-]+\.json)$'
}

function Read-AperturePackage([string]$Root) {
    Set-StrictMode -Version Latest
    Assert-ApertureTree $Root
    $manifest = Get-Content -LiteralPath (Join-Path $Root 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 1 -or $manifest.application -ne 'Aperture' -or $manifest.architecture -ne 'x64') { throw 'Unsupported release package.' }
    [void][version]$manifest.version
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        if (-not (Test-ApertureRuntimePath $file.path) -or -not $seen.Add($file.path)) { throw "Invalid or duplicate package path: $($file.path)" }
        $path = Join-Path $Root $file.path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $($file.path)" }
        if ((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256) { throw "Package verification failed: $($file.path)" }
    }
    foreach ($required in @('Aperture.exe','libmpv-2.dll','Update.cmd','update.ps1','runtime_update.ps1','release-source.json')) {
        if (-not $seen.Contains($required)) { throw "Incomplete package: $required" }
    }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\','/')
        if ($relative -ne 'manifest.json' -and -not $seen.Contains($relative)) { throw "Unlisted package file: $relative" }
    }
    return $manifest
}

function Stop-ApertureRuntime([string]$Root) {
    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + '\'
    $wasRunning = $false
    foreach ($process in Get-Process -Name 'Aperture*' -ErrorAction SilentlyContinue) {
        if ($process.Path -and $process.Path.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            $wasRunning = $true
            if (-not $process.CloseMainWindow()) { throw 'Close Aperture before updating. No files have been replaced.' }
            if (-not $process.WaitForExit(30000)) { throw 'Aperture is still saving or has a dialog open. Close it and try again.' }
        }
    }
    return $wasRunning
}

# Snapshot files remain ordinary, complete recovery trees. Immutable copies
# share storage on file systems with hard links; live runtime files never do.
function New-ApertureBackupLink([string]$Link, [string]$Existing) {
    New-Item -ItemType HardLink -Path $Link -Target $Existing -ErrorAction Stop | Out-Null
}

function Test-ApertureSameBackupFile([string]$Left, [string]$Right) {
    if (-not ('ApertureBackupIdentity' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ApertureBackupIdentity {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetFileInformationByHandle(SafeFileHandle handle, [Out] uint[] info);
}
"@
    }
    $sharing = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $leftStream = [IO.File]::Open($Left, 'Open', 'Read', $sharing)
    try {
        $rightStream = [IO.File]::Open($Right, 'Open', 'Read', $sharing)
        try {
            $a = New-Object uint32[] 13
            $b = New-Object uint32[] 13
            if (-not [ApertureBackupIdentity]::GetFileInformationByHandle($leftStream.SafeFileHandle, $a) -or
                -not [ApertureBackupIdentity]::GetFileInformationByHandle($rightStream.SafeFileHandle, $b)) { throw 'Cannot inspect rollback file identity.' }
            return $a[7] -eq $b[7] -and $a[11] -eq $b[11] -and $a[12] -eq $b[12]
        } finally { $rightStream.Dispose() }
    } finally { $leftStream.Dispose() }
}

function Save-ApertureSnapshotFile([string]$Source, [string]$Saved, [string]$Objects, $Verified, [ref]$LinksAvailable = ([ref]$null)) {
    if ($Source -ieq $Saved -and ([IO.File]::GetAttributes($Saved) -band [IO.FileAttributes]::ReadOnly)) { return }
    if ($LinksAvailable -and $LinksAvailable.Value -eq $false) {
        if ($Source -ine $Saved) { Copy-Item -LiteralPath $Source -Destination $Saved -Force }
        return
    }
    $hash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant()
    $object = Join-Path $Objects ($hash + '.bin')
    $created = $false
    if (-not (Test-Path -LiteralPath $object -PathType Leaf)) {
        $pending = Join-Path $Objects ([guid]::NewGuid().ToString('N') + '.tmp')
        try {
            Copy-Item -LiteralPath $Source -Destination $pending
            if ((Get-FileHash -LiteralPath $pending -Algorithm SHA256).Hash -ine $hash) { throw 'File changed during backup.' }
            [IO.File]::Move($pending, $object)
            $created = $true
            [void]$Verified.Add($hash)
        } finally {
            if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force }
        }
    } elseif (-not $Verified.Contains($hash)) {
        if ((Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash -ine $hash) { throw "Damaged rollback object: $object" }
        [void]$Verified.Add($hash)
    }
    if ($Source -ieq $Saved -and (Test-ApertureSameBackupFile $Saved $object)) { return }
    $link = $Saved + '.dedup-' + [guid]::NewGuid().ToString('N')
    try {
        try {
            New-ApertureBackupLink $link $object
            if ($LinksAvailable) { $LinksAvailable.Value = $true }
        } catch {
            if ($LinksAvailable) { $LinksAvailable.Value = $false }
            # Portable installs on file systems without hard links keep copies.
            if ($Source -ine $Saved) { Copy-Item -LiteralPath $Source -Destination $Saved -Force }
            if ($created) {
                Remove-Item -LiteralPath $object -Force
                [void]$Verified.Remove($hash)
            }
            return
        }
        if (Test-Path -LiteralPath $Saved) { [IO.File]::Replace($link, $Saved, [NullString]::Value) }
        else { [IO.File]::Move($link, $Saved) }
    } finally {
        if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
    }
}

function Optimize-ApertureSnapshots([string]$Backups, [string]$Objects, $Verified, [ref]$LinksAvailable = ([ref]$null)) {
    $referenced = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($snapshot in Get-ChildItem -LiteralPath $Backups -Directory) {
        $files = @()
        foreach ($name in @('runtime','data')) {
            $folder = Join-Path $snapshot.FullName $name
            if (Test-Path -LiteralPath $folder -PathType Container) {
                $files += @(Get-ChildItem -LiteralPath $folder -Recurse -File -Force)
            }
        }
        $config = Join-Path $snapshot.FullName 'mpv.conf'
        if (Test-Path -LiteralPath $config -PathType Leaf) { $files += Get-Item -LiteralPath $config -Force }
        foreach ($file in $files) {
            Save-ApertureSnapshotFile $file.FullName $file.FullName $Objects $Verified $LinksAvailable
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $object = Join-Path $Objects ($hash + '.bin')
            # Copies (including read-only files) do not keep an unused object alive.
            if ((Test-Path -LiteralPath $object -PathType Leaf) -and (Test-ApertureSameBackupFile $file.FullName $object)) {
                [void]$referenced.Add($hash)
            }
        }
    }
    foreach ($object in Get-ChildItem -LiteralPath $Objects -File -Filter '*.bin' -Force) {
        if ($object.BaseName -notmatch '^[a-f0-9]{64}$') { continue }
        if (-not $referenced.Contains($object.BaseName)) { Remove-Item -LiteralPath $object.FullName -Force }
    }
}

function Install-AperturePackage([string]$Package, [string]$Destination, [switch]$RestartIfRunning) {
    Set-StrictMode -Version Latest
    $Package = [IO.Path]::GetFullPath($Package).TrimEnd('\','/')
    $Destination = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
    if ($Package -ieq $Destination -or $Package.StartsWith($Destination + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Package must be staged outside the installed application.' }
    $manifest = Read-AperturePackage $Package
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Assert-ApertureTree $Destination
    $lock = [IO.File]::Open((Join-Path $Destination '.update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        $paths = @($manifest.files | ForEach-Object { $_.path }) + @('manifest.json')
        $expected = @{}
        foreach ($file in $manifest.files) { $expected[$file.path] = $file.sha256 }
        $expected['manifest.json'] = (Get-FileHash -LiteralPath (Join-Path $Package 'manifest.json') -Algorithm SHA256).Hash
        $replacements = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($relative in $paths) {
            $target = Join-Path $Destination $relative
            if (Test-Path -LiteralPath $target -PathType Container) { throw "A folder blocks a runtime file: $relative" }
            if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $expected[$relative]) { [void]$replacements.Add($relative) }
        }
        if (-not $replacements.Count) {
            Write-Host "Aperture $($manifest.version) is already installed; runtime files match the package."
            return
        }
        $wasRunning = Stop-ApertureRuntime $Destination
        $backups = Join-Path $Destination 'update-backups'
        $backup = Join-Path $backups ((Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        $objects = Join-Path $backups '.objects'
        New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $verified = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $linksAvailable = $null
        # Snapshot durable data before the new executable can migrate a schema.
        foreach ($name in @('data','mpv.conf')) {
            $path = Join-Path $Destination $name
            if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination (Join-Path $backup $name) -Recurse -Force }
        }
        $journal = @()
        foreach ($relative in $paths) {
            $target = Join-Path $Destination $relative
            $exists = Test-Path -LiteralPath $target -PathType Leaf
            if (Test-Path -LiteralPath $target -PathType Container) { throw "A folder blocks a runtime file: $relative" }
            if ($exists) {
                $saved = Join-Path (Join-Path $backup 'runtime') $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
                Save-ApertureSnapshotFile $target $saved $objects $verified ([ref]$linksAvailable)
            }
            $journal += [pscustomobject]@{ path = $relative; existed = $exists; replace = $replacements.Contains($relative) }
        }
        $journal | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'journal.json') -Encoding UTF8
        $changed = [Collections.Generic.List[object]]::new()
        try {
            foreach ($entry in $journal) {
                if (-not $entry.replace) { continue }
                $target = Join-Path $Destination $entry.path
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                $pending = $target + '.update-' + [guid]::NewGuid().ToString('N')
                try {
                    Copy-Item -LiteralPath (Join-Path $Package $entry.path) -Destination $pending
                    if ($entry.existed) { [IO.File]::Replace($pending, $target, [NullString]::Value) }
                    else { [IO.File]::Move($pending, $target) }
                    $changed.Add($entry)
                } finally {
                    if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force }
                }
            }
        } catch {
            $failure = $_
            $rollbackErrors = @()
            foreach ($entry in $changed) {
                try {
                    $target = Join-Path $Destination $entry.path
                    if ($entry.existed) { Copy-Item -LiteralPath (Join-Path (Join-Path $backup 'runtime') $entry.path) -Destination $target -Force }
                    elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
                } catch { $rollbackErrors += $_.Exception.Message }
            }
            if ($rollbackErrors.Count) { throw "Update failed; recovery files are at $backup. $($rollbackErrors -join '; ')" }
            throw "Update failed and runtime files were restored. $($failure.Exception.Message)"
        }
        # Retain three complete snapshots. Incomplete attempts are kept for recovery.
        Set-Content -LiteralPath (Join-Path $backup 'complete') -Value $manifest.version
        $completed = @(Get-ChildItem -LiteralPath $backups -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'complete') } | Sort-Object Name -Descending)
        foreach ($old in $completed | Select-Object -Skip 3) {
            $resolved = [IO.Path]::GetFullPath($old.FullName)
            if (-not $resolved.StartsWith([IO.Path]::GetFullPath($backups) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid backup cleanup path.' }
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
        # Compact old copy-based snapshots too, including incomplete recovery trees.
        try { Optimize-ApertureSnapshots $backups $objects $verified ([ref]$linksAvailable) }
        catch { Write-Warning "Runtime installed; backup compaction deferred: $($_.Exception.Message)" }
        Write-Host "Installed Aperture $($manifest.version). Saved data retained. Backup: $backup" -ForegroundColor Green
        if ($RestartIfRunning -and $wasRunning) { Start-Process -FilePath (Join-Path $Destination 'Aperture.exe') -WorkingDirectory $Destination }
    } finally { $lock.Dispose() }
}
