<#
.SYNOPSIS
    Tests encrypted loopback HTTPS, RTSP/TCP, and RTMP playback.
.DESCRIPTION
    Requires a user-supplied MediaMTX executable, FFmpeg, OpenSSL, Python, and
    the generated media fixtures. No tools are downloaded or globally installed.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$MediaMtxPath,
    [string]$DllPath = '',
    [string]$MediaDir = '',
    [string]$Ffmpeg = 'ffmpeg',
    [string]$OpenSsl = 'openssl',
    [string]$Python = 'python',
    [string]$TrustedHttpsUrl = ''
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$DllPath) { $DllPath = Join-Path $repo 'libmpv-2.dll' }
if (!$MediaDir) { $MediaDir = Join-Path $repo 'scratch\libmpv-tests\media' }
$MediaDir = (Resolve-Path -LiteralPath $MediaDir).Path
$MediaMtxPath = (Resolve-Path -LiteralPath $MediaMtxPath).Path
$run = Join-Path $repo ('scratch\libmpv-tests\network-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run -Force | Out-Null
function New-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    return $port
}
function Wait-Port([int]$Port, $Process) {
    for ($i=0; $i -lt 100; $i++) {
        $Process.Refresh()
        if ($Process.HasExited) { throw "Fixture exited: $run" }
        try { $tcp = [Net.Sockets.TcpClient]::new('127.0.0.1',$Port); $tcp.Dispose(); return } catch { Start-Sleep -Milliseconds 100 }
    }
    throw "Fixture port $Port did not become ready."
}
$httpsPort=New-Port; $rtspPort=New-Port; $rtmpPort=New-Port
$processes = @()
try {
    $cert = Join-Path $run 'test.crt'; $key = Join-Path $run 'test.key'
    $certArgs = 'req -x509 -newkey rsa:2048 -nodes -keyout "{0}" -out "{1}" -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"' -f $key,$cert
    $certificate = Start-Process -FilePath $OpenSsl -ArgumentList $certArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'certificate-out.txt') -RedirectStandardError (Join-Path $run 'certificate-error.txt')
    $null = $certificate.Handle
    $certificate.WaitForExit(); $certificate.Refresh()
    if ($certificate.ExitCode -ne 0) { throw 'TLS fixture certificate generation failed.' }
    $args = '"{0}" --port {1} --cert "{2}" --key "{3}"' -f (Join-Path $PSScriptRoot 'https_fixture.py'),$httpsPort,$cert,$key
    $https = Start-Process -FilePath $Python -ArgumentList $args -WorkingDirectory $MediaDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'https-out.txt') -RedirectStandardError (Join-Path $run 'https-error.txt')
    $processes += $https
    Wait-Port $httpsPort $https
    $config = Join-Path $run 'mediamtx.yml'
    @"
logLevel: info
rtsp: true
rtspAddress: 127.0.0.1:$rtspPort
rtspTransports: [tcp]
rtmp: true
rtmpAddress: 127.0.0.1:$rtmpPort
hls: false
webrtc: false
srt: false
api: false
metrics: false
pprof: false
playback: false
paths:
  stream:
    source: publisher
"@ | Set-Content -LiteralPath $config -Encoding ASCII
    $server = Start-Process -FilePath $MediaMtxPath -ArgumentList ('"' + $config + '"') -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'mediamtx-out.txt') -RedirectStandardError (Join-Path $run 'mediamtx-error.txt')
    $processes += $server
    Wait-Port $rtspPort $server
    $inputFile = Join-Path $MediaDir 'h264_aac_720p.mp4'
    $publish = '-hide_banner -loglevel error -re -stream_loop -1 -i "{0}" -map 0:v -map 0:a -c copy -f flv rtmp://127.0.0.1:{1}/stream' -f $inputFile,$rtmpPort
    $publisher = Start-Process -FilePath $Ffmpeg -ArgumentList $publish -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $run 'publisher-out.txt') -RedirectStandardError (Join-Path $run 'publisher-error.txt')
    $processes += $publisher
    $ready=$false
    for ($i=0; $i -lt 100; $i++) {
        if ((Get-Content -LiteralPath (Join-Path $run 'mediamtx-out.txt') -Raw) -match 'is publishing to path') { $ready=$true; break }
        $publisher.Refresh()
        if ($publisher.HasExited) { throw "Publisher exited: $run" }
        Start-Sleep -Milliseconds 100
    }
    if (!$ready) { throw "Publisher did not become ready: $run" }
    foreach ($url in @("https://127.0.0.1:$httpsPort/h264_aac_faststart.mp4", "rtsp://127.0.0.1:$rtspPort/stream", "rtmp://127.0.0.1:$rtmpPort/stream")) {
        # FFmpeg's Schannel backend uses the Windows trust store. Avoid changing
        # that store: the self-signed loopback fixture alone bypasses verification.
        $verify = if ($url.StartsWith('https://127.0.0.1:')) { 'no' } else { 'yes' }
        & (Join-Path $PSScriptRoot 'run_tests.ps1') -DllPath $DllPath -MediaDir $MediaDir -Scenario url -Url $url -TlsVerify $verify -Python $Python
    }
    if ($TrustedHttpsUrl) {
        if (!$TrustedHttpsUrl.StartsWith('https://')) { throw 'TrustedHttpsUrl must use HTTPS.' }
        & (Join-Path $PSScriptRoot 'run_tests.ps1') -DllPath $DllPath -MediaDir $MediaDir -Scenario url -Url $TrustedHttpsUrl -TlsVerify yes -Python $Python
    }
    Write-Host "HTTPS, RTSP, RTMP passed. Fixture logs: $run"
} finally {
    foreach ($process in $processes) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
}
