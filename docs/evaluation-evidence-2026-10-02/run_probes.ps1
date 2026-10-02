$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../src')).Path
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$probeBuild = Join-Path $tempBase ('Aperture-evaluation-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $probeBuild | Out-Null
try {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $exe = Join-Path $probeBuild 'probes.exe'
    $rsp = Join-Path $probeBuild 'compile.rsp'
    $argsList = @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
        "/I`"$source`"", "/Fo`"$probeBuild\\`"", "/Fe:`"$exe`"", "`"$(Join-Path $PSScriptRoot 'probes.cpp')`"")
    foreach ($file in @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery','GlobalSearch','TextInputModel','FolderWatcher','MediaFoundation')) {
        $argsList += "`"$(Join-Path $source ($file + '.cpp'))`""
    }
    $argsList += @('mfreadwrite.lib','oleaut32.lib','propsys.lib','user32.lib','shell32.lib','advapi32.lib')
    Set-Content -LiteralPath $rsp -Value $argsList -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$rsp`""
    if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed' }
    & $exe | Tee-Object -FilePath (Join-Path $PSScriptRoot 'probes-results.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Probe execution failed' }
} finally {
    $resolved = [IO.Path]::GetFullPath($probeBuild)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-evaluation-*') { throw "Unsafe cleanup: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
