$ErrorActionPreference = 'Stop'
$nativeBuild = 'C:\Users\Alperen\Desktop\Antigravity\Aperture Final\scratch\libmpv-build'
$nativeEvidence = 'C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-vlc-performance-20261017\direct-nv12-experiment'
$nativeOutput = Join-Path $nativeBuild 'direct-nv12-experiment'
if (Test-Path -LiteralPath $nativeOutput) { throw 'Preserve the existing native output.' }
New-Item -ItemType Directory -Path $nativeOutput | Out-Null
$nativeLock = $null
$nativeMapped = $false
$nativePreviousMsystem = $env:MSYSTEM
try {
    $nativeLock = [IO.File]::Open((Join-Path $nativeBuild '.build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    if (Test-Path -LiteralPath 'Q:\') { throw 'Q: already in use.' }
    & subst.exe 'Q:' $nativeBuild
    if ($LASTEXITCODE -ne 0) { throw 'subst failed' }
    $nativeMapped = $true
    $env:MSYSTEM = 'MINGW64'
    & 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/mingw64/bin:/usr/bin:$PATH; ninja -C /q/mpv-build -j6' 2>&1 | Tee-Object -FilePath (Join-Path $nativeEvidence 'native-build.log')
    if ($LASTEXITCODE -ne 0) { throw "Native build failed ($LASTEXITCODE)" }
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'mpv-build\libmpv-2.dll') -Destination (Join-Path $nativeOutput 'libmpv-2.actual-unstripped.dll')
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'mpv-build\libmpv-2.dll') -Destination (Join-Path $nativeOutput 'libmpv-2.dll')
    & 'C:\msys64\mingw64\bin\strip.exe' --strip-unneeded (Join-Path $nativeOutput 'libmpv-2.dll')
    if ($LASTEXITCODE -ne 0) { throw 'strip failed' }
    Get-FileHash -LiteralPath (Join-Path $nativeOutput 'libmpv-2.dll'),(Join-Path $nativeOutput 'libmpv-2.actual-unstripped.dll') | Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $nativeEvidence 'artifact-hashes.json')
} finally {
    $env:MSYSTEM = $nativePreviousMsystem
    if ($nativeMapped) { & subst.exe 'Q:' /D | Out-Null }
    if ($nativeLock) { $nativeLock.Dispose() }
}
