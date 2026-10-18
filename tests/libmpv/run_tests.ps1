<#
.SYNOPSIS
    Compiles and runs isolated playback checks against a specific libmpv DLL.
#>
[CmdletBinding()]
param(
    [string]$DllPath = '',
    [string]$MediaDir = '',
    [ValidateSet('all','functional','version','basics','controls','subs','screenshot','geometry','swrender','audio','hdr','bench','network','startup','url','resources','framecheck','transitioncheck')]
    [string]$Scenario = 'functional',
    [string]$Url = '',
    [ValidateSet('gpu','gpu-next')][string]$Renderer = 'gpu',
    [string]$ResourceFile = '',
    [ValidateSet('','1','2','4')][string]$HardwareDecoderThreads = '',
    [ValidateSet('','yes','no')][string]$GpuTiming = '',
    [ValidateSet('','0','1','4')][string]$VertexCacheEntries = '',
    [ValidateSet('default','balanced','linear')][string]$ScalingProfile = 'default',
    [ValidateSet('','yes','no')][string]$SdrFboPrecision = '',
    [ValidateSet('','yes','no')][string]$DirectNv12Filtering = '',
    [ValidateSet('','yes','no')][string]$ZeroCopy = '',
    [switch]$FramebufferDiagnostics,
    [switch]$VisiblePlayback,
    [ValidateSet('d3d11va','no')][string]$HardwareDecodeMode = 'd3d11va',
    [ValidateSet('','auto','rgba16hf','rgb10_a2','rgba8')][string]$FramebufferFormat = '',
    [string]$FrameCapture = '',
    [ValidateSet('','fruit','ordered','error-diffusion','no')][string]$FrameDither = '',
    [ValidateSet(0,90,180,270)][int]$FrameRotation = 0,
    [ValidateRange(-3,3)][double]$FrameZoom = 0,
    [ValidateSet('yes','no')][string]$TlsVerify = 'yes',
    [string]$Python = 'python',
    [int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
if (!$VisiblePlayback -and $Scenario -in @('all','resources','bench','startup')) { throw 'This scenario requires visible playback; it cannot run as a hidden performance measurement.' }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$DllPath) { $DllPath = Join-Path $repo 'libmpv-2.dll' }
if (!$MediaDir) { $MediaDir = Join-Path $repo 'scratch\libmpv-tests\media' }
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
$MediaDir = [IO.Path]::GetFullPath($MediaDir)
if ($Scenario -eq 'url' -and !$Url) { throw '-Scenario url requires -Url.' }
if ($Url -and $Url -match '["\r\n]') { throw 'URL contains invalid command-line characters.' }
if ($Scenario -ne 'version' -and -not (Test-Path -LiteralPath $MediaDir -PathType Container)) {
    & (Join-Path $PSScriptRoot 'generate_media.ps1') -OutDir $MediaDir
}
$run = Join-Path $repo ('scratch\libmpv-tests\run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'mpv_playback_test.cpp'
$exe = Join-Path $run 'mpv_playback_test.exe'
$buildCmd = 'call "{0}" >nul && cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS "{1}" /I "{2}" /Fo"{3}\mpv_playback_test.obj" /Fe:"{4}" /link ole32.lib user32.lib shlwapi.lib /SUBSYSTEM:CONSOLE' -f $vcvars,$source,(Join-Path $repo 'include'),$run,$exe
& cmd.exe /d /s /c $buildCmd
if ($LASTEXITCODE -ne 0) { throw 'Playback harness compilation failed.' }
$stagedDll = Join-Path $run 'libmpv-2.dll'
Copy-Item -LiteralPath $DllPath -Destination $stagedDll
$sha = (Get-FileHash -LiteralPath $DllPath).Hash
if ((Get-FileHash -LiteralPath $stagedDll).Hash -ne $sha) { throw 'Staged DLL hash mismatch.' }
$server = $null
$port = 0
try {
    if ($Scenario -in @('all','functional','network')) {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
        $listener.Start()
        $port = $listener.LocalEndpoint.Port
        $listener.Stop()
        $server = Start-Process -FilePath $Python -ArgumentList @('-m','http.server',"$port",'--bind','127.0.0.1') -WorkingDirectory $MediaDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'http-out.txt') -RedirectStandardError (Join-Path $run 'http-error.txt')
        $ready = $false
        for ($i=0; $i -lt 40; $i++) {
            $server.Refresh()
            if ($server.HasExited) { throw 'Local HTTP fixture exited.' }
            try {
                $client = [Net.Sockets.TcpClient]::new('127.0.0.1',$port)
                $client.Dispose(); $ready = $true; break
            } catch { Start-Sleep -Milliseconds 100 }
        }
        if (!$ready) { throw 'Local HTTP fixture did not become ready.' }
    }
    $arguments = '--dll "{0}" --media "{1}" --scenario {2} --port {3}' -f $stagedDll,$MediaDir,$Scenario,$port
    $arguments += $(if ($VisiblePlayback) { ' --visible' } else { ' --headless' })
    if ($FrameDither) { $arguments += ' --frame-dither ' + $FrameDither }
    if ($FrameRotation) { $arguments += ' --frame-rotate ' + $FrameRotation }
    if ($FrameZoom) { $arguments += ' --frame-zoom ' + $FrameZoom.ToString([Globalization.CultureInfo]::InvariantCulture) }
    if ($FramebufferFormat) { $arguments += ' --fbo-format ' + $FramebufferFormat }
    $arguments += ' --resource-hwdec ' + $HardwareDecodeMode
    if ($FrameCapture) {
        if ($FrameCapture -match '["\r\n]') { throw 'Invalid capture path.' }
        $arguments += ' --capture-window "' + [IO.Path]::GetFullPath($FrameCapture) + '"'
    }
    if ($Url) { $arguments += ' --url "' + $Url + '"' }
    $arguments += ' --tls-verify ' + $TlsVerify
    $arguments += ' --renderer ' + $Renderer
    if ($HardwareDecoderThreads) { $arguments += ' --hwdec-threads ' + $HardwareDecoderThreads }
    if ($GpuTiming) { $arguments += ' --gpu-timing ' + $GpuTiming }
    if ($VertexCacheEntries -ne '') { $arguments += ' --vertex-cache-entries ' + $VertexCacheEntries }
    if ($ScalingProfile -ne 'default') { $arguments += ' --scaling ' + $ScalingProfile }
    if ($SdrFboPrecision) { $arguments += ' --fbo-sdr10 ' + $SdrFboPrecision }
    if ($DirectNv12Filtering) { $arguments += ' --scale-direct-nv12 ' + $DirectNv12Filtering }
    if ($ZeroCopy) { $arguments += ' --zero-copy ' + $ZeroCopy }
    if ($FramebufferDiagnostics) { $arguments += ' --fbo-diagnostics' }
    if ($ResourceFile) {
        if ($ResourceFile -match '["\r\n/\\]' -or $ResourceFile -eq '..') { throw 'ResourceFile must be a filename.' }
        $arguments += ' --resource-file "' + $ResourceFile + '"'
    }
    $out = Join-Path $run 'playback.txt'
    $err = Join-Path $run 'playback-error.txt'
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $process.Handle
    if (!$process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Playback tests timed out; logs: $run"
    }
    $process.WaitForExit()
    $process.Refresh()
    $log = Get-Content -LiteralPath $out -Raw
    Write-Host $log
    if ($log -notmatch 'Loaded module path matches requested: YES') { throw 'Test loaded an unexpected DLL.' }
    if ((Get-FileHash -LiteralPath $stagedDll).Hash -ne $sha) { throw 'Staged DLL changed during the test.' }
    $summary = [ordered]@{scenario=$Scenario;sourceDll=$DllPath;loadedDll=$stagedDll;sha256=$sha;bytes=(Get-Item -LiteralPath $stagedDll).Length;exitCode=$process.ExitCode;visibility=$(if($VisiblePlayback){'visible-playback'}else{'hidden-correctness-only'});logs=$run}
    $summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding UTF8
    Write-Host "Results: $run"
    if ($process.ExitCode -ne 0) { throw "Playback tests failed ($($process.ExitCode))." }
} finally {
    if ($server) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
}
