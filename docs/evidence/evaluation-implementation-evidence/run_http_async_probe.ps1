$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$work = Join-Path $root 'scratch/evaluation-fixes/http-async-probe'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$exe = Join-Path $work 'http_async_probe.exe'
$compile = "`"$vcvars`" && cl.exe /nologo /utf-8 /EHsc /std:c++17 /DUNICODE /D_UNICODE /I `"$root\src`" /Fo`"$work\\`" `"$PSScriptRoot\http_async_probe.cpp`" `"$root\src\HttpTransport.cpp`" winhttp.lib shlwapi.lib shell32.lib ole32.lib /Fe:`"$exe`""
cmd.exe /c $compile
if ($LASTEXITCODE -ne 0) { throw 'Asynchronous transport probe compilation failed' }
$results = & $exe
$probeExitCode = $LASTEXITCODE
$results | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'http-async-results.csv')
$results | Write-Output
exit $probeExitCode
