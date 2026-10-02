param([Parameter(Mandatory=$true)][string]$VideoPath, [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$video = (Resolve-Path -LiteralPath $VideoPath).Path
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')
$output = Join-Path $tempRoot ('Aperture-preview-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $exe = Join-Path $output 'preview.exe'
    $response = Join-Path $output 'compile.rsp'
    $compile = @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE','/guard:cf','/GS','/sdl',
        "/I`"$root/src`"", "/I`"$root`"", "/Fo`"$output\\`"", "/Fe:`"$exe`"", "`"$PSScriptRoot/preview_pipeline.cpp`"")
    foreach ($source in @('MpvPlayer','Json','Settings','Localization','AudioConfig','ChannelPolicy')) {
        $compile += "`"$root/src/$source.cpp`""
    }
    $compile += @('oleaut32.lib','propsys.lib','/link /guard:cf /SUBSYSTEM:CONSOLE')
    Set-Content -LiteralPath $response -Value $compile -Encoding utf8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`""
    if ($LASTEXITCODE -ne 0) { throw 'Preview native probe compilation failed' }
    Copy-Item -LiteralPath (Join-Path $root 'libmpv-2.dll') -Destination $output
    $result = & $exe $video
    if ($LASTEXITCODE -ne 0) { throw 'Preview native probe failed' }
    $result
    if ($OutputDirectory) {
        New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
        $result | Set-Content -LiteralPath (Join-Path $OutputDirectory 'preview-result.txt') -Encoding utf8
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($output)
    if (-not $resolved.StartsWith($tempRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-preview-*') { throw 'Unsafe preview probe cleanup target' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
