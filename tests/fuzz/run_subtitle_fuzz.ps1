param(
    [int64]$Seed = 0,
    [int64]$Iterations = 0,
    [switch]$Verbose,
    [switch]$Help
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Resolve-Path (Join-Path $scriptDir "..\..")

. (Join-Path $rootDir 'tests/support/native_build.ps1')

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Aperture_subtitle_fuzz_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$exeOutput = Join-Path $tempDir "subtitle_fuzz.exe"

$libs = "d2d1.lib dwrite.lib d3d11.lib dxgi.lib windowscodecs.lib mfreadwrite.lib mfplat.lib mfuuid.lib dwmapi.lib shell32.lib shlwapi.lib comctl32.lib comdlg32.lib uxtheme.lib user32.lib gdi32.lib ole32.lib oleaut32.lib oleacc.lib advapi32.lib propsys.lib dxguid.lib imm32.lib"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "  Building Aperture Subtitle Fuzz Tests" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan

$srcFiles = @(
    (Join-Path $scriptDir 'subtitle_fuzz.cpp'),
    (Join-Path $rootDir 'src/Subtitles.cpp'),
    (Join-Path $rootDir 'src/SubtitleDiscovery.cpp'),
    (Join-Path $rootDir 'src/Json.cpp')
)

$testExitCode = 1
try {
    Build-ApertureTestHarness -Sources $srcFiles -Output $exeOutput -Libraries ($libs -split ' ')

    $argsList = @()
    if ($Seed -gt 0) { $argsList += "--seed", $Seed }
    if ($Iterations -gt 0) { $argsList += "--iterations", $Iterations }
    if ($Verbose) { $argsList += "--verbose" }
    if ($Help) { $argsList += "--help" }

    Write-Host "`nRunning subtitle fuzz test runner...`n" -ForegroundColor Green
    & $exeOutput $argsList
    $testExitCode = $LASTEXITCODE
}
finally {
    if ($tempDir -and (Test-Path -LiteralPath $tempDir)) {
        $resolvedTemp = [IO.Path]::GetFullPath($tempDir)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + '\'
        if (-not $resolvedTemp.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolvedTemp) -notmatch '^Aperture_subtitle_fuzz_[a-f0-9]{32}$') {
            throw 'Unsafe subtitle fuzz cleanup path.'
        }
        if ((Get-Item -LiteralPath $resolvedTemp -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Linked subtitle fuzz directories are unsupported.'
        }
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}

exit $testExitCode
