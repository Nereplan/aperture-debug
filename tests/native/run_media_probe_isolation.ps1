param([string]$OutputDirectory = "", [string]$HelperExecutable = "")
$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$temporary = [string]::IsNullOrWhiteSpace($OutputDirectory)
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$output = if ($temporary) {
    Join-Path $tempRoot ("Aperture_probe_isolation_" + [Guid]::NewGuid().ToString("N"))
} else { [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory)) }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw "Visual Studio C++ tools not found" }
$vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars64.bat"
$exe = Join-Path $output "media_probe_isolation.exe"
$sources = @("$root/src/MediaProbeIsolation.cpp", "$root/src/MediaFoundation.cpp", "$PSScriptRoot/media_probe_isolation.cpp")
$quotedSources = ($sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
$command = "`"$vcvars`" && cl.exe /nologo /utf-8 /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DAPERTURE_TEST_PROBE_ISOLATION /I `"$root/src`" /Fo`"$output\\`" $quotedSources /Fe:`"$exe`" /link /SUBSYSTEM:CONSOLE mfreadwrite.lib mfplat.lib mfuuid.lib windowscodecs.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib propsys.lib user32.lib gdi32.lib psapi.lib"
try {
    & cmd.exe /c $command
    if ($LASTEXITCODE -ne 0) { throw "Isolation test compilation failed: $LASTEXITCODE" }
    $arguments = @()
    if ($HelperExecutable) {
        $resolvedHelper = [IO.Path]::GetFullPath($HelperExecutable)
        if (-not (Test-Path -LiteralPath $resolvedHelper -PathType Leaf)) { throw "Helper executable not found: $resolvedHelper" }
        $arguments = @('--resource-helper-executable', $resolvedHelper)
    }
    $results = & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Isolation native test failed: $LASTEXITCODE" }
    $results | Set-Content -LiteralPath (Join-Path $output "results.csv") -Encoding utf8
    $results
} finally {
    if ($temporary -and (Test-Path -LiteralPath $output)) {
        $resolvedOutput = [IO.Path]::GetFullPath($output)
        if (-not $resolvedOutput.StartsWith($tempRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing cleanup outside the isolated temporary root: $resolvedOutput"
        }
        Remove-Item -LiteralPath $resolvedOutput -Recurse -Force
    }
}
