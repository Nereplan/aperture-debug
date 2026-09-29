[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$verifier = Join-Path $repoRoot "scripts\verify_runtime_layout.ps1"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("Aperture-runtime-layout-test-" + [Guid]::NewGuid().ToString("N"))

function Write-TestFile([string]$Path, [string]$Content) {
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

function Invoke-LayoutCheck([string]$Runtime, [string]$Source, [switch]$ShouldFail, [switch]$IncludeDebug) {
    $failed = $false
    try {
        $debugArgs = @{}
        if ($IncludeDebug) {
            $debugArgs.IncludeDebug = $true
            $debugArgs.DebugBinarySource = Join-Path $Source "built\Aperture.Debug.exe"
            $debugArgs.DebugSymbolsSource = Join-Path $Source "built\Aperture.pdb"
        }
        & $verifier -RuntimeDirectory $Runtime -SourceRoot $Source -BinarySource (Join-Path $Source "built\Aperture.exe") @debugArgs *> $null
    } catch {
        $failed = $true
    }
    if ($ShouldFail -and -not $failed) {
        throw "Expected runtime layout verification to fail, but it passed."
    }
    if (-not $ShouldFail -and $failed) {
        throw "Expected runtime layout verification to pass, but it failed."
    }
}

try {
    $source = Join-Path $tempRoot "source"
    $runtime = Join-Path $tempRoot "runtime"
    New-Item -ItemType Directory -Path (Join-Path $source "scripts"), $runtime -Force | Out-Null

    $manifest = @{
        version = 1
        vendoredFiles = @(
            @{ path = "libmpv-2.dll"; category = "required" },
            @{ path = "fonts/Inter.ttf"; category = "required" },
            @{ path = "fonts/JetBrainsMono.ttf"; category = "required" },
            @{ path = "fonts/SpaceGrotesk.ttf"; category = "required" },
            @{ path = "include/mpv/client.h"; category = "required" }
        )
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $source "scripts\dependencies.json") -Encoding UTF8

    Write-TestFile (Join-Path $source "built\Aperture.exe") "release-binary"
    Write-TestFile (Join-Path $source "libmpv-2.dll") "mpv"
    Write-TestFile (Join-Path $source "fonts\Inter.ttf") "inter"
    Write-TestFile (Join-Path $source "fonts\JetBrainsMono.ttf") "mono"
    Write-TestFile (Join-Path $source "fonts\SpaceGrotesk.ttf") "space"
    Write-TestFile (Join-Path $source "locales\en-US.json") "{}"
    Write-TestFile (Join-Path $source "locales\de-DE.json") "{}"

    Write-TestFile (Join-Path $runtime "Aperture.exe") "release-binary"
    Copy-Item -LiteralPath (Join-Path $source "libmpv-2.dll") -Destination (Join-Path $runtime "libmpv-2.dll")
    New-Item -ItemType Directory -Path (Join-Path $runtime "fonts"), (Join-Path $runtime "locales") -Force | Out-Null
    Copy-Item -Path (Join-Path $source "fonts\*.ttf") -Destination (Join-Path $runtime "fonts")
    Copy-Item -Path (Join-Path $source "locales\*.json") -Destination (Join-Path $runtime "locales")

    Invoke-LayoutCheck $runtime $source

    Write-TestFile (Join-Path $runtime "manifest.json") "{}"
    Invoke-LayoutCheck $runtime $source -ShouldFail
    Remove-Item -LiteralPath (Join-Path $runtime "manifest.json") -Force

    New-Item -ItemType Directory -Path (Join-Path $runtime "data") -Force | Out-Null
    Invoke-LayoutCheck $runtime $source -ShouldFail
    Remove-Item -LiteralPath (Join-Path $runtime "data") -Recurse -Force

    Remove-Item -LiteralPath (Join-Path $runtime "fonts\Inter.ttf") -Force
    Invoke-LayoutCheck $runtime $source -ShouldFail
    Copy-Item -LiteralPath (Join-Path $source "fonts\Inter.ttf") -Destination (Join-Path $runtime "fonts\Inter.ttf")

    Set-Content -LiteralPath (Join-Path $runtime "libmpv-2.dll") -Value "wrong" -Encoding UTF8
    Invoke-LayoutCheck $runtime $source -ShouldFail
    Copy-Item -LiteralPath (Join-Path $source "libmpv-2.dll") -Destination (Join-Path $runtime "libmpv-2.dll") -Force

    Write-TestFile (Join-Path $source "built\Aperture.Debug.exe") "debug-binary"
    Write-TestFile (Join-Path $source "built\Aperture.pdb") "debug-symbols"
    Copy-Item -LiteralPath (Join-Path $source "built\Aperture.Debug.exe") -Destination $runtime
    Copy-Item -LiteralPath (Join-Path $source "built\Aperture.pdb") -Destination $runtime
    Invoke-LayoutCheck $runtime $source -ShouldFail
    Invoke-LayoutCheck $runtime $source -IncludeDebug
    Write-TestFile (Join-Path $runtime "Aperture.Debug.exe") "wrong-debug-binary"
    Invoke-LayoutCheck $runtime $source -IncludeDebug -ShouldFail
    Copy-Item -LiteralPath (Join-Path $source "built\Aperture.Debug.exe") -Destination $runtime -Force
    Remove-Item -LiteralPath (Join-Path $runtime "Aperture.pdb")
    Invoke-LayoutCheck $runtime $source -IncludeDebug -ShouldFail

    Write-Host "Runtime layout tests passed." -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
