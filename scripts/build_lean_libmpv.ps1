<#
.SYNOPSIS
    Builds Aperture's size-optimized, self-contained x64 libmpv DLL.
.DESCRIPTION
    Uses hash-pinned sources and existing MSYS2 MINGW64 packages. Never updates
    MSYS2, replaces the bundled DLL, or changes the dependency inventory.
    Build output and a provenance manifest are written below BuildRoot.
#>
[CmdletBinding()]
param(
    [string]$MsysRoot = 'C:\msys64',
    [string]$BuildRoot = '',
    [string]$OutputDir = '',
    [ValidateRange(0, 256)][int]$Jobs = 0,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\','/')
$scratch = Join-Path $repo 'scratch'
if (!$BuildRoot) { $BuildRoot = Join-Path $scratch 'libmpv-build' }
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot).TrimEnd('\','/')
if (-not $BuildRoot.StartsWith($scratch + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildRoot must be a strict descendant of this repository''s scratch directory.'
}
if (!$OutputDir) { $OutputDir = Join-Path $BuildRoot 'output' }
$OutputDir = [IO.Path]::GetFullPath($OutputDir).TrimEnd('\','/')
if (-not $OutputDir.StartsWith($BuildRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDir must be a strict descendant of BuildRoot.'
}
foreach ($path in @($BuildRoot, $OutputDir)) {
    $node = $path
    while ($node -and $node -ne $repo) {
        if ((Test-Path -LiteralPath $node) -and ((Get-Item -LiteralPath $node -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a reparse point in the build path: $node"
        }
        $node = Split-Path $node -Parent
    }
}
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) { throw "MSYS2 bash missing: $bash" }
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$lock = $null
$drive = $null
$mapped = $false
$previousMsystem = $env:MSYSTEM
$previousNoBytecode = $env:PYTHONDONTWRITEBYTECODE
try {
    $lock = [IO.File]::Open((Join-Path $BuildRoot '.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    # Clean only owned build products; retain verified downloads and extracted sources.
    if ($Clean) {
        foreach ($name in @('ffmpeg-build','placebo-build','mpv-build','build-shaderc','build-spirv-cross','prefix','stage','output')) {
            $target = Join-Path $BuildRoot $name
            if (Test-Path -LiteralPath $target) {
                $entries = @(Get-ChildItem -LiteralPath $target -Recurse -Force) + @(Get-Item -LiteralPath $target -Force)
                if (@($entries | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw "Reparse point below $target" }
                foreach ($file in $entries | Where-Object { -not $_.PSIsContainer }) { Remove-Item -LiteralPath $file.FullName }
                foreach ($dir in $entries | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending) { Remove-Item -LiteralPath $dir.FullName }
            }
        }
    }
    $recipe = Join-Path $BuildRoot 'recipe'
    New-Item -ItemType Directory -Path $recipe -Force | Out-Null
    # Imports in provenance must not create directories among hashed recipe files.
    # Remove only ordinary Python cache files left by an earlier invocation.
    $recipeCache = [IO.Path]::GetFullPath((Join-Path $recipe '__pycache__'))
    if (-not $recipeCache.StartsWith($BuildRoot + '\recipe\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe recipe cache path.' }
    if (Test-Path -LiteralPath $recipeCache) {
        if (((Get-Item -LiteralPath $recipe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            ((Get-Item -LiteralPath $recipeCache -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse point in recipe cache.' }
        $cacheFiles = @(Get-ChildItem -LiteralPath $recipeCache -Force)
        if (@($cacheFiles | Where-Object { $_.PSIsContainer -or $_.Extension -ne '.pyc' -or ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }).Count) { throw 'Unexpected entry in recipe cache.' }
        foreach ($file in $cacheFiles) { Remove-Item -LiteralPath $file.FullName }
        Remove-Item -LiteralPath $recipeCache
    }
    $utf8 = New-Object Text.UTF8Encoding($false)
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'libmpv') -File) {
        [IO.File]::WriteAllText((Join-Path $recipe $file.Name), ([IO.File]::ReadAllText($file.FullName).Replace([string][char]13 + [char]10, [string][char]10)), $utf8)
    }
    $driveRecord = Join-Path $BuildRoot '.build-drive.txt'
    if (Test-Path -LiteralPath $driveRecord) {
        $cachedDrive = [IO.File]::ReadAllText($driveRecord).Trim()
        if ($cachedDrive -notmatch '^[QTYXZWVU]$') { throw 'Invalid cached build drive.' }
        if (Test-Path -LiteralPath ($cachedDrive + ':\')) { throw "Cached build drive $cachedDrive is busy; free it or use a fresh BuildRoot." }
        $drive = $cachedDrive
    } else {
        foreach ($letter in @('Q','T','Y','X','Z','W','V','U')) {
            if (-not (Test-Path -LiteralPath ($letter + ':\'))) { $drive = $letter; break }
        }
    }
    if (!$drive) { throw 'No free drive letter for the space-free build alias.' }
    & subst.exe ($drive + ':') $BuildRoot
    if ($LASTEXITCODE -ne 0) { throw 'Could not create build drive alias.' }
    $mapped = $true
    [IO.File]::WriteAllText($driveRecord, $drive, $utf8)
    if ($Jobs -eq 0) { $Jobs = [Environment]::ProcessorCount }
    $env:MSYSTEM = 'MINGW64'
    $env:PYTHONDONTWRITEBYTECODE = '1'
    $msys = '/' + $drive.ToLowerInvariant()
    & $bash -lc "cd '$msys' && JOBS=$Jobs bash recipe/build.sh 2>&1" | Tee-Object -FilePath (Join-Path $BuildRoot 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "libmpv build failed ($LASTEXITCODE); see $BuildRoot\build.log" }
    if ($OutputDir -ne (Join-Path $BuildRoot 'output')) {
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $BuildRoot 'output\libmpv-2.dll'), (Join-Path $BuildRoot 'output\build-provenance.json') -Destination $OutputDir
    }
    $dll = Join-Path $OutputDir 'libmpv-2.dll'
    Write-Host ("Built {0}: {1:N0} bytes; SHA-256 {2}" -f $dll, (Get-Item -LiteralPath $dll).Length, (Get-FileHash -LiteralPath $dll).Hash)
} finally {
    $env:MSYSTEM = $previousMsystem
    $env:PYTHONDONTWRITEBYTECODE = $previousNoBytecode
    if ($mapped) { & subst.exe ($drive + ':') /D | Out-Null }
    if ($lock) { $lock.Dispose() }
}
