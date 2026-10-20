# No player launch, visible window, desktop capture or GDI pixel read is allowed here.
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'PlayerVideoReadiness.cs')
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ReadinessHiddenFixture {
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr CreateWindowEx(uint extended,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr param);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDpiAwarenessContext();
}
'@
$checkCount = 0
function Check([bool]$Condition,[string]$Name) {
    if (!$Condition) { throw $Name }
    $script:checkCount++
    Write-Output "PASS $Name"
}
for ($phase = 0; $phase -lt 4; $phase++) {
    $rgb = [PlayerVideoReadiness]::ExpectedRgb($phase)
    Check ($rgb.Count -eq 54 -and [PlayerVideoReadiness]::RecognizeRgb($rgb) -eq $phase) "recognize synthetic phase $phase"
}
$noisy = [PlayerVideoReadiness]::ExpectedRgb(2)
for ($i=0; $i -lt $noisy.Count; $i++) {
    $r = [Math]::Min(255,(($noisy[$i] -shr 16) -band 255)+8)
    $g = [Math]::Min(255,(($noisy[$i] -shr 8) -band 255)+8)
    $b = [Math]::Min(255,($noisy[$i] -band 255)+8)
    $noisy[$i] = ($r -shl 16) -bor ($g -shl 8) -bor $b
}
Check ([PlayerVideoReadiness]::RecognizeRgb($noisy) -eq 2) 'bounded RGB conversion error preserves phase'
$broken = [PlayerVideoReadiness]::ExpectedRgb(0)
$broken[53] = 0
Check ([PlayerVideoReadiness]::RecognizeRgb($broken) -eq -1) 'all sentinel pixels must match'
$transition = [PlayerVideoReadiness]::ExpectedRgb(0)
$other = [PlayerVideoReadiness]::ExpectedRgb(1)
for ($i=27; $i -lt 54; $i++) { $transition[$i] = $other[$i] }
Check ([PlayerVideoReadiness]::RecognizeRgb($transition) -eq -1) 'mixed scene phases are ambiguous'
Check ([PlayerVideoReadiness]::RecognizeRgb([int[]]::new(54)) -eq -1) 'black shell is not recognized as video'
$rejected = $false
try { [void][PlayerVideoReadiness]::RecognizeRgb([int[]]::new(53)) } catch { $rejected = $true }
Check $rejected 'incorrect sample count is rejected'
$broken[0] = 0x1000000
$rejected = $false
try { [void][PlayerVideoReadiness]::RecognizeRgb($broken) } catch { $rejected = $true }
Check $rejected 'invalid RGB encoding is rejected'

# A padded top-down BGR32 buffer exercises the same sparse extraction as the
# desktop DIB, without creating a DC or reading any screen pixels.
$width = 11; $height = 8; $stride = $width*4+12
$pixels = [byte[]]::new($stride*$height)
for ($i=0; $i -lt $pixels.Length; $i++) { $pixels[$i] = 0xa5 }
$xy = [int[]]::new(108)
for ($i=0; $i -lt 54; $i++) { $xy[$i*2] = 1+$i%9; $xy[$i*2+1] = 1+[int][Math]::Floor($i/9) }
$buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($pixels.Length)
try {
    for ($phase=0; $phase -lt 4; $phase++) {
        $expected = [PlayerVideoReadiness]::ExpectedRgb($phase)
        for ($i=0; $i -lt 54; $i++) {
            $offset = $xy[$i*2+1]*$stride+$xy[$i*2]*4
            $pixels[$offset] = $expected[$i] -band 255
            $pixels[$offset+1] = ($expected[$i] -shr 8) -band 255
            $pixels[$offset+2] = ($expected[$i] -shr 16) -band 255
            $pixels[$offset+3] = ($i*17+$phase)%256
        }
        [Runtime.InteropServices.Marshal]::Copy($pixels,0,$buffer,$pixels.Length)
        $actual = [PlayerVideoReadiness]::ExtractBgr32($buffer,$width,$height,$stride,$pixels.Length,$xy)
        Check (($actual -join ',') -eq ($expected -join ',') -and [PlayerVideoReadiness]::RecognizeRgb($actual) -eq $phase) "strided BGR extraction preserves phase $phase and ignores alpha/padding"
    }
    $firstRed = $xy[1]*$stride+$xy[0]*4+2
    [Runtime.InteropServices.Marshal]::WriteByte($buffer,$firstRed,[byte]((($expected[0] -shr 16) -band 255)+[PlayerVideoReadiness]::ColorTolerance))
    $actual = [PlayerVideoReadiness]::ExtractBgr32($buffer,$width,$height,$stride,$pixels.Length,$xy)
    Check ([PlayerVideoReadiness]::RecognizeRgb($actual) -eq 3) 'extracted RGB at tolerance boundary retains phase'
    [Runtime.InteropServices.Marshal]::WriteByte($buffer,$firstRed,[byte]((($expected[0] -shr 16) -band 255)+[PlayerVideoReadiness]::ColorTolerance+1))
    $actual = [PlayerVideoReadiness]::ExtractBgr32($buffer,$width,$height,$stride,$pixels.Length,$xy)
    Check ([PlayerVideoReadiness]::RecognizeRgb($actual) -eq -1) 'extracted RGB beyond tolerance boundary rejects phase'
    $invalidX = [int[]]$xy.Clone(); $invalidX[106] = $width
    $invalidY = [int[]]$xy.Clone(); $invalidY[107] = -1
    foreach ($case in @(
        @{name='null BGR pointer'; bits=[IntPtr]::Zero; width=$width; stride=$stride; bytes=$pixels.Length; xy=$xy},
        @{name='short BGR stride'; bits=$buffer; width=$width; stride=$width*4-1; bytes=$pixels.Length; xy=$xy},
        @{name='truncated BGR storage'; bits=$buffer; width=$width; stride=$stride; bytes=$pixels.Length-1; xy=$xy},
        @{name='invalid BGR width'; bits=$buffer; width=0; stride=$stride; bytes=$pixels.Length; xy=$xy},
        @{name='null sentinel coordinates'; bits=$buffer; width=$width; stride=$stride; bytes=$pixels.Length; xy=$null},
        @{name='incorrect sentinel coordinate count'; bits=$buffer; width=$width; stride=$stride; bytes=$pixels.Length; xy=[int[]]::new(106)},
        @{name='sentinel beyond row width'; bits=$buffer; width=$width; stride=$stride; bytes=$pixels.Length; xy=$invalidX},
        @{name='negative sentinel row'; bits=$buffer; width=$width; stride=$stride; bytes=$pixels.Length; xy=$invalidY}
    )) {
        $rejected = $false
        try { [void][PlayerVideoReadiness]::ExtractBgr32($case.bits,$case.width,$height,$case.stride,$case.bytes,$case.xy) } catch { $rejected = $true }
        Check $rejected ($case.name+' is rejected before an invalid memory read')
    }
} finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }

$description = & python (Join-Path $PSScriptRoot 'generate_video_readiness_fixture.py') --describe
if ($LASTEXITCODE -ne 0) { throw 'Fixture description failed.' }
$definition = $description | ConvertFrom-Json
Check (($definition.paletteRgb -join ',') -eq ([PlayerVideoReadiness]::Palette -join ',') -and $definition.phaseCount -eq [PlayerVideoReadiness]::PhaseCount -and $definition.phaseSeconds -eq [PlayerVideoReadiness]::PhaseSeconds) 'fixture and recognizer share phase/color definitions'

$root = [ReadinessHiddenFixture]::CreateWindowEx(0,'STATIC','hidden readiness fixture',2147483648,0,0,640,360,[IntPtr]::Zero,[IntPtr]::Zero,[IntPtr]::Zero,[IntPtr]::Zero)
if ($root -eq [IntPtr]::Zero) { throw 'Hidden fixture creation failed.' }
$canvas = [IntPtr]::Zero
try {
    $canvas = [ReadinessHiddenFixture]::CreateWindowEx(0,'STATIC','hidden canvas',0x50000000,0,0,640,360,$root,[IntPtr]::Zero,[IntPtr]::Zero,[IntPtr]::Zero)
    if ($canvas -eq [IntPtr]::Zero) { throw 'Hidden child creation failed.' }
    Check (![ReadinessHiddenFixture]::IsWindowVisible($root) -and ![ReadinessHiddenFixture]::IsWindowVisible($canvas)) 'fixture stays off screen'
    $dpiBefore = [ReadinessHiddenFixture]::GetThreadDpiAwarenessContext()
    $sample = [PlayerVideoReadiness]::Capture($root,$canvas,[uint32]$PID)
    Check ($sample.status -eq 'rejected' -and $sample.reason -eq 'hidden-or-minimized' -and $sample.qpcReadStart -eq 0 -and $sample.rgb.Count -eq 0) 'hidden window is rejected before any pixel read'
    Check ([ReadinessHiddenFixture]::GetThreadDpiAwarenessContext() -eq $dpiBefore) 'capture restores caller DPI context'
    $sample = [PlayerVideoReadiness]::Capture($root,$canvas,[uint32]($PID+1))
    Check ($sample.status -eq 'rejected' -and $sample.reason -eq 'unowned-or-invalid-canvas' -and $sample.qpcReadStart -eq 0 -and $sample.rgb.Count -eq 0) 'wrong PID is rejected before any pixel read'
    $roundTrip = $sample | ConvertTo-Json -Depth 6 | ConvertFrom-Json
    Check ($roundTrip.status -eq 'rejected' -and $roundTrip.qpcStart -gt 0 -and $roundTrip.qpcEnd -ge $roundTrip.qpcStart) 'rejection retains JSON timing bounds'
    $guard = [PlayerVideoReadiness]::ValidateVisibleCanvas($root,$canvas,[uint32]$PID)
    Check (!$guard.safe -and $guard.reason -eq 'hidden-or-minimized' -and $guard.qpcReadStart -eq 0 -and $guard.rgb.Count -eq 0) 'visibility-only guard rejects hidden canvas without pixel capture'
    $guard = [PlayerVideoReadiness]::ValidateVisibleCanvas($root,$canvas,[uint32]($PID+1))
    Check (!$guard.safe -and $guard.reason -eq 'unowned-or-invalid-canvas' -and $guard.qpcReadStart -eq 0 -and $guard.rgb.Count -eq 0) 'visibility-only guard rejects foreign PID without pixel capture'
} finally {
    if ($canvas -ne [IntPtr]::Zero) { [void][ReadinessHiddenFixture]::DestroyWindow($canvas) }
    [void][ReadinessHiddenFixture]::DestroyWindow($root)
}
$result = [PlayerVideoReadiness]::WaitForPhase([uint32]$PID,'Aperture',[Diagnostics.Stopwatch]::GetTimestamp(),0,5,1)
Check ($result.status -eq 'timeout' -and $result.capturedSamples -eq 0 -and $null -eq $result.upperBoundMs) 'missing canvas never becomes zero-latency readiness'
$result = [PlayerVideoReadiness]::WaitForPhase([uint32]::MaxValue,'Aperture',[Diagnostics.Stopwatch]::GetTimestamp(),0,5,1)
Check ($result.status -eq 'rejected' -and $null -eq $result.upperBoundMs) 'invalid process never becomes readiness'
$guard = [PlayerVideoReadiness]::ValidateVisibleCanvas([uint32]$PID,'Aperture')
Check (!$guard.safe -and $guard.status -eq 'pending' -and $guard.reason -eq 'video-canvas-not-present' -and $guard.qpcReadStart -eq 0) 'visibility-only player discovery does not capture desktop pixels'
Check ($checkCount -eq 35) 'complete readiness development check set'
Write-Output "$checkCount checks passed; visible sentinel, occlusion, startup and seek validation remain deferred to the coordinated desktop session."
