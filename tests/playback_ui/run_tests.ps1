[CmdletBinding()]
param(
    [string]$MediaPath = '',
    [string]$OutputDir = '',
    [switch]$AllowVisibleWindows
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$toolsScratch = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Codex/tools/scratch'
if (-not $MediaPath) { $MediaPath = Join-Path $repo 'scratch/libmpv-tests/media/h264_1080p_60s.mp4' }
$MediaPath = (Resolve-Path -LiteralPath $MediaPath).Path
if (-not $OutputDir) { $OutputDir = Join-Path $toolsScratch ('aperture-playback-ui-' + [Guid]::NewGuid().ToString('N')) }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'Use a new output directory; retained evidence is not overwritten.' }
if (-not $OutputDir.StartsWith([IO.Path]::GetFullPath($toolsScratch).TrimEnd('\','/') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'OutputDir must be below Codex tools scratch.' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$runtime = Join-Path $OutputDir 'runtime'
New-Item -ItemType Directory -Path (Join-Path $runtime 'data') -Force | Out-Null
& (Join-Path $repo 'build.ps1') -BuildOutputPath (Join-Path $runtime 'Aperture.exe') -PdbOutputPath (Join-Path $OutputDir 'Aperture.pdb') *> (Join-Path $OutputDir 'app-build.log')
if ($LASTEXITCODE -ne 0) { throw 'Application build failed; see app-build.log.' }
# Reuse only the objects just built from the current worktree, replacing wWinMain
# with a console runner. Default tests keep every window hidden and mute playback.
# Explicit -AllowVisibleWindows adds owned-window presentation/return checks.
$cmake = Get-Content -Raw -LiteralPath (Join-Path $repo 'CMakeLists.txt')
$sourceBlock = [regex]::Match($cmake, '(?s)set\(APERTURE_SOURCES\s+(.*?)\)')
if (-not $sourceBlock.Success) { throw 'Application source inventory not found.' }
$objects = @([regex]::Matches($sourceBlock.Groups[1].Value, 'src/([^\s)]+\.cpp)') | ForEach-Object {
    if ($_.Groups[1].Value -ne 'main.cpp') {
        $objectPath = Join-Path $repo ('scratch/build/ninja-Release/CMakeFiles/Aperture.dir/src/' + $_.Groups[1].Value + '.obj')
        if (-not (Test-Path -LiteralPath $objectPath)) { throw "Missing production object: $objectPath" }
        '"' + $objectPath + '"'
    }
})
if ($objects.Count -lt 30) { throw 'Expected completed native application objects.' }
$libraryBlock = [regex]::Match($cmake, '(?s)target_link_libraries\(Aperture PRIVATE\s+(.*?)\)')
if (-not $libraryBlock.Success) { throw 'Application library inventory not found.' }
$libraries = @($libraryBlock.Groups[1].Value -split '\s+' | Where-Object { $_ } | ForEach-Object { $_ + '.lib' })
. (Join-Path $repo 'tests/support/native_build.ps1')
$exe = Join-Path $runtime 'playback-ui-tests.exe'
Build-ApertureTestHarness -Sources @((Join-Path $PSScriptRoot 'main.cpp')) -Output $exe -Libraries ($objects + $libraries) -Manifest (Join-Path $repo 'tests/support/native_tests.manifest') *> (Join-Path $OutputDir 'harness-build.log')
Copy-Item -LiteralPath (Join-Path $repo 'libmpv-2.dll') -Destination $runtime
Copy-Item -LiteralPath (Join-Path $repo 'fonts') -Destination $runtime -Recurse
[ordered]@{recordedUtc=[DateTime]::UtcNow.ToString('o');sourceCommit=(& git -C $repo rev-parse HEAD);dirty=$true;mediaPath=$MediaPath;mediaSha256=(Get-FileHash -LiteralPath $MediaPath).Hash;nativeSha256=(Get-FileHash -LiteralPath (Join-Path $runtime 'libmpv-2.dll')).Hash;sourceSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'main.cpp')).Hash;scope=$(if($AllowVisibleWindows){'Native interaction correctness plus explicitly opted-in visible owned-window return checks; forced trim clocks, no performance/cadence claims.'}else{'Hidden native App interaction/render correctness only. No visible windows or VLC launch; no performance claims.'});executableSha256=(Get-FileHash -LiteralPath (Join-Path $runtime 'Aperture.exe')).Hash;harnessSha256=(Get-FileHash -LiteralPath $exe).Hash} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDir 'identity.json') -Encoding utf8
& $exe $MediaPath $OutputDir *> (Join-Path $OutputDir 'checks.log')
$exitCode = $LASTEXITCODE
Get-Content -LiteralPath (Join-Path $OutputDir 'checks.log') -Tail 18
Write-Output $OutputDir
if ($exitCode -ne 0) { throw "Native interaction checks failed ($exitCode); preserve the output directory." }
if ($AllowVisibleWindows) {
    # Production owns one App per process. Keep factory-bound resources from a
    # previous fixture out of each presentation case as well.
    foreach ($mode in @('windowed', 'maximized', 'fullscreen', 'pip', 'minimized')) {
        $log = Join-Path $OutputDir "visible-$mode.log"
        & $exe $MediaPath $OutputDir --visible-return $mode *> $log
        $exitCode = $LASTEXITCODE
        Get-Content -LiteralPath $log -Tail 8
        if ($exitCode -ne 0) { throw "Visible $mode return checks failed ($exitCode); preserve the output directory." }
    }
}
