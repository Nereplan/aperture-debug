param([ValidateSet('h264','hevc','av1')][string]$Workload='h264', [string]$OutputName='h264-hub', [switch]$IncludeBefore, [switch]$Pilot, [switch]$Sentinel)
$ErrorActionPreference='Stop'
$root='C:\Users\Alperen\Desktop\Antigravity\Aperture Final'
$work=Join-Path $env:LOCALAPPDATA 'Codex\tools\scratch\aperture-rival-20261019'
$fixtures=Join-Path $env:LOCALAPPDATA 'Codex\tools\scratch\aperture-vlc-performance-20261017'
$media=@{h264='h264-1080p30-audio.mp4';hevc='hevc-2160p60-audio.mp4';av1='av1-1080p30-audio.mp4'}[$Workload]
$output=Join-Path $work $OutputName
if($Sentinel){$fixtures=Join-Path $work 'readiness-fixture';$media='video-readiness-h264-aac.mp4'}
if(Test-Path -LiteralPath $output) { throw 'Choose a fresh benchmark output name.' }
$extra=@{}
if($Sentinel){$extra.SentinelFixtureManifest=Join-Path $fixtures 'manifest.json'}
$iterations=1; $warmups=0; $seconds=0; $alignment=0
if(-not $Sentinel){throw 'This launcher is only for the controlled sentinel color check.'}
$extra.SentinelColorCheck=$true
if($IncludeBefore) {
    $variants=Join-Path $work ($OutputName+'-variants.json')
    if(Test-Path -LiteralPath $variants) { throw 'Variants already exist.' }
    @(@{id='Aperture-before';runtime=(Join-Path $work 'baseline-runtime')},@{id='Aperture-current';runtime=(Join-Path $work 'candidate-runtime-qualified')}) | ConvertTo-Json | Set-Content -LiteralPath $variants -Encoding utf8
    $extra.ApertureVariantsFile=$variants
}
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class BenchmarkCursor {
 public struct Point { public int x,y; }
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
}
"@
$prior=[BenchmarkCursor+Point]::new()
if(-not [BenchmarkCursor]::GetCursorPos([ref]$prior)) { throw 'Cannot read the cursor for visibility preflight.' }
$cursorWasMoved=$false
# Leave an already-safe cursor alone. The actual sample guards remain authoritative.
if($prior.x -ge 32 -and $prior.x -le 1360 -and $prior.y -ge 32 -and $prior.y -le 832) {
 if(-not [BenchmarkCursor]::SetCursorPos(0,0)) { throw 'Cursor overlaps the planned canvas and Windows rejected moving it; move it outside x32..1360,y32..832.' }
 $cursorWasMoved=$true
}
try {
 & (Join-Path $root 'tests\benchmark\compare_players.ps1') -AllowVisiblePlayback -RefreshVlcBaseline -FailFast -PlayerNames Aperture,VLC -AperturePath (Join-Path $work 'candidate-runtime-qualified') -VlcPath (Join-Path $work 'vlc-3.0.24\vlc-3.0.24') -MediaPath (Join-Path $fixtures $media) -OutputDir $output -LaunchMode HubThenOpen -Iterations $iterations -WarmupIterations $warmups -VideoWidth 1280 -VideoHeight 720 -SamplingStartMediaSeconds $alignment -SamplingSeconds $seconds @extra
} finally {
 $current=[BenchmarkCursor+Point]::new()
 if($cursorWasMoved -and [BenchmarkCursor]::GetCursorPos([ref]$current) -and $current.x -eq 0 -and $current.y -eq 0) { [void][BenchmarkCursor]::SetCursorPos($prior.x,$prior.y) }
}
