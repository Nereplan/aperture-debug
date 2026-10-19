[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PrefixPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [ValidateSet('size','balanced','performance')][string]$OptimizationProfile = 'size',
    [string]$MsysRoot = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'
$PrefixPath = (Resolve-Path -LiteralPath $PrefixPath).Path
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ((Test-Path -LiteralPath $OutputPath) -or (Test-Path -LiteralPath ($OutputPath + '.json'))) { throw 'Preserve existing probe output/identity; choose a new path.' }
if (!(Test-Path -LiteralPath (Split-Path -Parent $OutputPath))) { throw 'Output parent must exist.' }
if (!(Test-Path -LiteralPath (Join-Path $PrefixPath 'lib\libavcodec.a'))) { throw 'FFmpeg static prefix required.' }
$source = Join-Path $PSScriptRoot 'ffmpeg_decode_probe.c'
$builder = Join-Path $PSScriptRoot 'build_native_decode_probe.sh'
$drive = $null
foreach ($letter in @('T','Y','X','Z','W','V','U')) {
    if (!(Test-Path -LiteralPath ($letter + ':\'))) { $drive = $letter; break }
}
if (!$drive) { throw 'No free drive alias for the static prefix.' }
$mapped = $false
$previousMsystem = $env:MSYSTEM
try {
    & subst.exe ($drive + ':') $PrefixPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not create prefix alias.' }
    $mapped = $true
    $env:MSYSTEM = 'MINGW64'
    $imports = @(& (Join-Path $MsysRoot 'usr\bin\bash.exe') --noprofile --norc $builder ('/' + $drive.ToLowerInvariant()) $source $OutputPath $OptimizationProfile.ToLowerInvariant())
    if ($LASTEXITCODE -ne 0) { throw 'Decode probe compilation failed.' }
    $names = @($imports | ForEach-Object { if ($_ -match 'DLL Name:\s+(\S+)') { $Matches[1].ToLowerInvariant() } })
    $system = @('bcrypt.dll','crypt32.dll','kernel32.dll','msvcrt.dll','ncrypt.dll','secur32.dll','ws2_32.dll','ole32.dll','user32.dll','advapi32.dll')
    if (!$names.Count -or @($names | Where-Object { $_ -notin $system }).Count) { throw "Unexpected probe runtime imports: $($names -join ', ')" }
    $libraries = [ordered]@{}
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $PrefixPath 'lib') -Filter '*.a' -File) {
        $libraries[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [ordered]@{sourceSha256=(Get-FileHash -LiteralPath $source).Hash;profile=$OptimizationProfile.ToLowerInvariant();prefix=$PrefixPath;probeSha256=(Get-FileHash -LiteralPath $OutputPath).Hash;imports=$names;prefixStaticLibraryHashes=$libraries;scope='CPU-only software demux/decode; no player/renderer/audio output. Static non-system dependencies.'} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($OutputPath + '.json') -Encoding utf8
    Write-Output $OutputPath
} finally {
    $env:MSYSTEM = $previousMsystem
    if ($mapped) { & subst.exe ($drive + ':') /D | Out-Null }
}
