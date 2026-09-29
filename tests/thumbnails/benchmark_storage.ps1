[CmdletBinding()]
param([Parameter(Mandatory)][string]$Corpus, [Parameter(Mandatory)][string]$BaselineHeaderDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempBase ('Aperture-thumb-storage-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $source = (Resolve-Path -LiteralPath $Corpus).Path
    $baseline = (Resolve-Path -LiteralPath $BaselineHeaderDirectory).Path
    $results = @()
    foreach ($mode in @('before','after')) {
        $headers = if ($mode -eq 'before') { $baseline } else { Join-Path $root 'src' }
        $exe = Join-Path $run "$mode.exe"
        $response = Join-Path $run "$mode.rsp"
        @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
          "/I`"$headers`"", "/I`"$root/src`"", "/Fo`"$run\\`"", "/Fe:`"$exe`"", "`"$PSScriptRoot/storage_benchmark.cpp`"") |
            Set-Content -LiteralPath $response -Encoding UTF8
        & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
        if ($LASTEXITCODE -ne 0) { throw 'Storage benchmark compilation failed' }
    }
    # Alternate order to reduce filesystem warming and run-order bias.
    foreach ($iteration in 1..3) {
        $order = if ($iteration % 2) { @('before','after') } else { @('after','before') }
        foreach ($mode in $order) {
            $result = & (Join-Path $run "$mode.exe") $source (Join-Path $run "$mode-$iteration")
            if ($LASTEXITCODE -ne 0) { throw 'Storage benchmark failed verification' }
            $value = $result | ConvertFrom-Json
            $value | Add-Member -NotePropertyName mode -NotePropertyValue $mode
            $value | Add-Member -NotePropertyName iteration -NotePropertyValue $iteration
            $results += $value
        }
    }
    $report = Join-Path $root 'diagnostics/thumbnail-storage-benchmark.json'
    $results | ConvertTo-Json | Set-Content -LiteralPath $report -Encoding UTF8
    $results | Format-Table
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-thumb-storage-*') { throw 'Unsafe temporary cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
