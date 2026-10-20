$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'MSVC not found' }
$version = (Get-Content -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$compiler = Join-Path $vs "VC/Tools/MSVC/$version/bin/Hostx64/x64/cl.exe"
$linker = Join-Path $vs "VC/Tools/MSVC/$version/bin/Hostx64/x64/link.exe"
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$shared = Join-Path $taskRoot 'baseline-src'
$common = Join-Path $taskRoot 'common-objects'
$baseline = Join-Path $taskRoot 'baseline-build'
$candidate = Join-Path $taskRoot 'candidate-build'
foreach ($directory in @($common,$baseline,$candidate)) { New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null }
$flags = @('/nologo','/utf-8','/O2','/MT','/bigobj','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE', ('/I"'+$shared+'"'))
$commonNames = @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','GlobalSearch','TextInputModel','FolderWatcher','MediaFoundation','MediaProbeIsolation')
$commonSources = @($commonNames | ForEach-Object { Join-Path $shared ($_+'.cpp') }) + @(Join-Path $taskRoot 'flat_sift_probe.cpp')
$commonArguments = $flags + @('/c',('/Fo"'+$common+'\\"')) + @($commonSources | ForEach-Object { '"'+$_+'"' })
$commonArguments | Set-Content -LiteralPath (Join-Path $taskRoot 'compile-common.rsp') -Encoding utf8
foreach ($variant in @('baseline','candidate')) {
    $directory = if ($variant -eq 'baseline') {$baseline} else {$candidate}
    $source = Join-Path $taskRoot ($variant+'-src/LibraryQuery.cpp')
    ($flags + @('/c',('/Fo"'+(Join-Path $directory 'LibraryQuery.obj')+'"'),('"'+$source+'"'))) |
        Set-Content -LiteralPath (Join-Path $taskRoot ('compile-'+$variant+'.rsp')) -Encoding utf8
    $objects = @($commonNames | ForEach-Object {Join-Path $common ($_+'.obj')}) + @(Join-Path $common 'flat_sift_probe.obj') + @(Join-Path $directory 'LibraryQuery.obj')
    $linkArguments = @('/NOLOGO','/SUBSYSTEM:CONSOLE','/INCREMENTAL:NO','/OPT:REF','/OPT:ICF',('/OUT:"'+(Join-Path $directory 'flat-sift-probe.exe')+'"')) +
        @($objects | ForEach-Object {'"'+$_+'"'}) + @('mfreadwrite.lib','oleaut32.lib','propsys.lib')
    $linkArguments | Set-Content -LiteralPath (Join-Path $taskRoot ('link-'+$variant+'.rsp')) -Encoding utf8
}
$batch = @('@echo off',('call "'+$vcvars+'" >nul'),'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$compiler+'" @"'+(Join-Path $taskRoot 'compile-common.rsp')+'"'),'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$compiler+'" @"'+(Join-Path $taskRoot 'compile-baseline.rsp')+'"'),'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$compiler+'" @"'+(Join-Path $taskRoot 'compile-candidate.rsp')+'"'),'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$linker+'" @"'+(Join-Path $taskRoot 'link-baseline.rsp')+'"'),'if errorlevel 1 exit /b %errorlevel%',
    ('"'+$linker+'" @"'+(Join-Path $taskRoot 'link-candidate.rsp')+'"'),'exit /b %errorlevel%')
$batch | Set-Content -LiteralPath (Join-Path $taskRoot 'build.cmd') -Encoding ascii
$identity = [ordered]@{compiler=$compiler;compilerVersion=(Get-Item -LiteralPath $compiler).VersionInfo.FileVersion;compilerSha256=(Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant();linker=$linker;linkerSha256=(Get-FileHash -LiteralPath $linker -Algorithm SHA256).Hash.ToLowerInvariant();commonCodeGenerationFlags=$flags;variantDifference='Only LibraryQuery.cpp is compiled separately. Both links reuse the identical common11production objects and identical probe object.';buildStartedUtc=[DateTime]::UtcNow.ToString('o')}
$identity | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskRoot 'toolchain.json') -Encoding utf8
& cmd.exe /d /c ('"'+(Join-Path $taskRoot 'build.cmd')+'"') *> (Join-Path $taskRoot 'build.log')
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath (Join-Path $taskRoot 'build.log') -Tail 35; throw "Build failed: $LASTEXITCODE" }
Write-Output 'Both frozen variants built; no timings run.'
