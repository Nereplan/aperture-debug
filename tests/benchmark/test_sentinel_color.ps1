# Pure parser/metric and mocked owned-control contracts. No player, window,
# desktop readback, socket, compilation or runtime staging occurs here.
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'compare_players.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw ($errors.Message -join '; ') }
$names=@('Vlc-ReplyValues','Vlc-Time','Vlc-ColorState','Color-Status','Assert-ColorPaused','Wait-ColorPaused','Resume-ColorVlc','Color-Difference')
foreach($name in $names) {
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$false)
    if(-not $definition) { throw "Missing tested function: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$count=0
function Check([bool]$Condition,[string]$Name) {
    if(-not $Condition) { throw $Name }
    $script:count++; Write-Output "PASS $Name"
}
function Rejects([scriptblock]$Action) {
    try { & $Action | Out-Null; return $false } catch { return $true }
}
$values=@(Vlc-ReplyValues "VLC media player3.0.24`r`n> 1`r`n6`r`n> " 2)
Check ($values.Count -eq 2 -and $values[0] -eq 1 -and $values[1] -eq 6) 'RC state/time batch parses complete numeric lines'
$values=@(Vlc-ReplyValues "1`n6" 2)
Check ($values.Count -eq 1) 'partial TCP numeric line is not accepted'
$values=@(Vlc-ReplyValues "1`n63`n" 2)
Check ($values.Count -eq 2 -and $values[1] -eq 63) 'completed multi-digit value is preserved'
Check (Rejects { Vlc-ReplyValues "1`n6`n7`n" 2 }) 'extra RC numeric responses are rejected'
Check (Rejects { Vlc-ReplyValues ('x'*16385) 1 }) 'oversized RC transcript is rejected'
$black=[int[]]::new(54); $white=[int[]]::new(54)
for($i=0;$i -lt 54;$i++) { $white[$i]=0xffffff }
$metric=Color-Difference $black $black
Check ($metric.meanAbsoluteComponentDifference -eq 0 -and $metric.maximumAbsoluteComponentDifference -eq 0 -and $metric.componentCount -eq 162) 'identical RGB samples have zero pairwise difference'
$metric=Color-Difference $black $white
Check ($metric.meanAbsoluteComponentDifference -eq 255 -and $metric.maximumAbsoluteComponentDifference -eq 255) 'channel-wise difference reaches full 8-bit range'
$one=[int[]]::new(54); $one[0]=0x030000
$metric=Color-Difference $black $one
Check ([Math]::Abs($metric.meanAbsoluteComponentDifference-3.0/162) -lt 1e-12 -and $metric.maximumAbsoluteComponentDifference -eq 3) 'packed RGB integers are split into independent components'
$reverse=Color-Difference $one $black
Check ($reverse.meanAbsoluteComponentDifference -eq $metric.meanAbsoluteComponentDifference) 'pairwise metric treats neither player as reference'
Check (Rejects { Color-Difference ([int[]]::new(53)) $black }) 'incomplete RGB samples are rejected'
$invalid=[int[]]::new(54); $invalid[53]=0x1000000
Check (Rejects { Color-Difference $invalid $black }) 'out-of-range RGB is rejected'
Check (Rejects { Assert-ColorPaused @{paused=$false;positionSec=6} 6 }) 'playing status cannot validate paused color'
Check (Rejects { Assert-ColorPaused @{paused=$true;positionSec=$null} 6 }) 'missing position cannot validate paused color'
Check (Rejects { Assert-ColorPaused @{paused=$true;positionSec=[double]::NaN} 6 }) 'nonfinite position cannot validate paused color'
Check (Rejects { Assert-ColorPaused @{paused=$true;positionSec=18} 6 }) 'wrong phase midpoint is rejected'
Check (Rejects { Assert-ColorPaused @{paused=$true;positionSec=6.2} 6 @{positionSec=6} }) 'motion during paused sampling is rejected'
Assert-ColorPaused @{paused=$true;positionSec=6.001} 6 @{positionSec=6}
Check $true 'stable near-midpoint paused position is accepted'

$script:replies=[Collections.Generic.Queue[object]]::new()
$script:requests=[Collections.Generic.List[string]]::new()
$script:expectedCounts=[Collections.Generic.List[int]]::new()
function Vlc-Numbers([int]$Port,[int]$OwnedPid,[string]$Request,[int]$ExpectedCount=1) {
    if($OwnedPid -ne 123 -or $Port -ne 456) { throw 'Control lost explicit owned PID/port.' }
    $script:requests.Add($Request)
    $script:expectedCounts.Add($ExpectedCount)
    if(-not $script:replies.Count) { throw 'Unexpected extra control request.' }
    $reply=$script:replies.Dequeue()
    $reply.request=$Request
    $reply.qpcStart=[Diagnostics.Stopwatch]::GetTimestamp()
    $reply.qpcEnd=[Diagnostics.Stopwatch]::GetTimestamp()
    $reply
}
$script:replies.Enqueue(@{values=@(1,6);raw="status change: ( pause state: 4 )`n"})
$status=Color-Status VLC 123 456 @{} -Pause
Assert-ColorPaused $status 6
Check ($requests.Count -eq 1 -and $requests[0] -notmatch 'pause') 'already-paused VLC is never toggled into playing'
$script:requests.Clear(); $script:replies.Enqueue(@{values=@(1,18);raw="status change: ( play state: 3 )`n"}); $script:replies.Enqueue(@{values=@(1,18);raw="status change: ( pause state: 4 )`n"})
$status=Color-Status VLC 123 456 @{} -Pause
Assert-ColorPaused $status 18
Check ($requests.Count -eq 2 -and $requests[1] -eq "pause`nstatus`nis_playing`nget_time`n" -and $status.raw.Count -eq 2) 'playing VLC toggles once and retains both raw state/time responses'
$script:replies.Enqueue(@{values=@(0,18);raw="status change: ( stop state: 5 )`n"})
$statusStopped=Color-Status VLC 123 456 @{}
Check (Rejects { Assert-ColorPaused $statusStopped 18 }) 'VLC stopped state cannot masquerade as paused'
$script:replies.Enqueue(@{values=@(0,18);raw='no authoritative state'})
$statusUnknown=Color-Status VLC 123 456 @{}
Check (Rejects { Assert-ColorPaused $statusUnknown 18 }) 'VLC missing status cannot masquerade as paused'
Check ($status.qpcStart -gt 0 -and $status.qpcEnd -ge $status.qpcStart) 'control observation retains conservative QPC bounds'
$script:requests.Clear(); $script:replies.Enqueue(@{values=@(1,30);raw="status change: ( play state: 3 )`n"}); $script:replies.Enqueue(@{values=@(1,30);raw="status change: ( play state: 3 )`n"})
$status=Color-Status VLC 123 456 @{} -Pause
Check (Rejects { Assert-ColorPaused $status 30 }) 'unconfirmed VLC pause is rejected'
$script:replies.Enqueue(@{values=@(9,42)})
Check (Rejects { Color-Status VLC 123 456 @{} -Pause }) 'invalid VLC state is rejected'
$script:requests.Clear(); $script:replies.Enqueue(@{values=@(30)})
Check ((Vlc-Time 456 123 '' 30) -eq 30 -and $requests[0] -eq "seek 30`nget_time`n") 'ordinary seek/time control remains compatible'
$script:lastAction=''
function Playback([string]$Name,[int]$OwnedPid,[int]$Port,$Environment,[string]$Action='') {
    $script:lastAction=$Action
    @{state=3;positionSec=42;reason=$null}
}
$status=Color-Status Aperture 123 0 @{} -Pause
Assert-ColorPaused $status 42
Check ($lastAction -eq 'Pause' -and $status.raw.state -eq 3) 'Aperture uses its explicit pause control and retained status'
$status=Color-Status Aperture 123 0 @{}
Check ($lastAction -eq '' -and $status.paused) 'Aperture sampled status does not issue another control'
$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,6);raw="status change: ( play state: 3 )`n"})
$script:replies.Enqueue(@{values=@(1,6);raw="status change: ( play state: 3 )`n"})
$initial=Color-Status VLC 123 456 @{} -Pause
$script:replies.Enqueue(@{values=@(1,6);raw="Type 'pause' to continue.`r`nstatus: returned 0 (no error)`r`n1`r`n6`r`n"})
$trace=@{}
$settled=Wait-ColorPaused VLC 123 456 @{} $initial 6 $trace
Check ($settled.paused -and $trace.observations.Count -eq 2 -and $trace.status -eq 'settled') 'asynchronous pause requires a later consistent status'
Check ($requests.Count -eq 3 -and @($requests | Where-Object { $_ -match '(?m)^pause$' }).Count -eq 1) 'pause confirmation never repeats the toggle'
Check ($trace.qpcStart -gt 0 -and $trace.qpcEnd -ge $trace.qpcStart -and $trace.observations[0].raw.Count -eq 2) 'settling retains initial replies and conservative QPC bounds'
$trace=@{}
$timedOut=Rejects { Wait-ColorPaused VLC 123 456 @{} $initial 6 $trace -TimeoutMs 0 }
Check ($timedOut -and $trace.status -eq 'failed' -and $trace.observations.Count -eq 1) 'unconfirmed pause times out with its observations retained'
Check (Rejects { Wait-ColorPaused VLC 123 456 @{} @{paused=$true;positionSec=18} 6 @{} }) 'settled pause still rejects the wrong midpoint'
$trace=@{}; $before=$requests.Count
$settled=Wait-ColorPaused VLC 123 456 @{} @{paused=$true;positionSec=6} 6 $trace
Check ($settled.paused -and $requests.Count -eq $before -and $trace.observations.Count -eq 1) 'already-settled pause needs no further control request'
$state=Vlc-ColorState @{values=@(1,6);raw="Type 'pause' to continue.`r`n"}
Check ($state.paused -and $state.evidence -eq 'input-pause-guard' -and $null -eq $state.state) 'old RC blocked status proves paused input without inventing a playlist state'
$state=Vlc-ColorState @{values=@(0,6);raw="Type 'pause' to continue.`r`nstatus change: ( pause state: 4 )`r`n"}
Check (-not $state.paused) 'stale pause text cannot validate an absent input'

$script:requests.Clear(); $script:expectedCounts.Clear()
$script:replies.Enqueue(@{values=@(1,6);raw="Type 'pause' to continue.`r`n"})
$script:replies.Enqueue(@{values=@(6);raw="6`r`n"})
$script:replies.Enqueue(@{values=@(1,6);raw="status change: ( play state: 3 )`r`n"})
$trace=@{}
$resumed=Resume-ColorVlc 123 456 @{} $trace
Check ($trace.status -eq 'settled' -and $trace.observations.Count -eq 2 -and $resumed.hasInput -and -not $resumed.paused -and $resumed.state -eq 3) 'guard-only paused VLC resumes with authoritative playing confirmation'
Check ($requests.Count -eq 3 -and $requests[1] -eq "pause`nget_time`n" -and $expectedCounts[1] -eq 1) 'resume uses one owned toggle with one expected numeric reply'
Check ($trace.toggle.raw -eq "6`r`n" -and $trace.toggle.request -eq $requests[1] -and $trace.qpcStart -le $trace.observations[0].qpcStart -and $trace.toggle.qpcStart -ge $trace.qpcStart -and $trace.qpcEnd -ge $trace.observations[1].qpcEnd) 'resume retains pre-control, toggle and confirmation raw/QPC evidence'

$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,18);raw="status change: ( pause state: 4 )`r`n"})
$script:replies.Enqueue(@{values=@(18);raw="18`r`n"})
$script:replies.Enqueue(@{values=@(1,18);raw="Type 'pause' to continue.`r`nstatus change: ( play state: 3 )`r`n"})
$script:replies.Enqueue(@{values=@(1,18);raw="status change: ( play state: 3 )`r`n"})
$trace=@{}
$resumed=Resume-ColorVlc 123 456 @{} $trace
Check ($trace.observations.Count -eq 3 -and $trace.observations[1].paused -and -not $resumed.paused -and $resumed.state -eq 3) 'resume waits past stale playing text while the input pause guard remains'
Check ($requests.Count -eq 4 -and @($requests | Where-Object { $_ -match '(?m)^pause$' }).Count -eq 1) 'asynchronous resume confirmations never repeat the toggle'

$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,30);raw="status change: ( play state: 3 )`r`n"})
$trace=@{}
$resumed=Resume-ColorVlc 123 456 @{} $trace -TimeoutMs 0
Check ($requests.Count -eq 1 -and -not $trace.ContainsKey('toggle') -and $trace.status -eq 'settled' -and $resumed.state -eq 3) 'already-playing VLC needs no toggle or further confirmation request'
foreach($invalid in @(
    @{values=@(0,6);raw="Type 'pause' to continue.`r`nstatus change: ( pause state: 4 )`r`n"},
    @{values=@(1,6);raw='no authoritative state'},
    @{values=@(1,6);raw="status change: ( stop state: 5 )`r`n"}
)) {
    $script:requests.Clear(); $script:replies.Enqueue($invalid); $trace=@{}
    $rejected=Rejects { Resume-ColorVlc 123 456 @{} $trace }
    Check ($rejected -and $trace.status -eq 'failed' -and $requests.Count -eq 1 -and -not $trace.ContainsKey('toggle') -and $trace.observations.Count -eq 1) 'absent, unknown or stopped input fails resume without toggling'
}

$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,42);raw="Type 'pause' to continue.`r`n"})
$script:replies.Enqueue(@{values=@(42);raw="42`r`n"})
$trace=@{}
$timedOut=Rejects { Resume-ColorVlc 123 456 @{} $trace -TimeoutMs 0 }
Check ($timedOut -and $trace.status -eq 'failed' -and $trace.reason -like 'Timed out*' -and $requests.Count -eq 2 -and $trace.observations.Count -eq 1) 'unconfirmed resume reaches its deadline without another request'
Check ($trace.toggle.raw -eq "42`r`n" -and $trace.qpcStart -gt 0 -and $trace.qpcEnd -ge $trace.toggle.qpcEnd -and @($requests | Where-Object { $_ -match '(?m)^pause$' }).Count -eq 1) 'resume timeout retains the one toggle and conservative QPC bounds'

$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,6);raw="Type 'pause' to continue.`r`n"})
$script:replies.Enqueue(@{values=@(6);raw="6`r`n"})
$script:replies.Enqueue(@{values=@(0,6);raw="status change: ( stop state: 5 )`r`n"})
$trace=@{}
$rejected=Rejects { Resume-ColorVlc 123 456 @{} $trace }
Check ($rejected -and $trace.status -eq 'failed' -and $requests.Count -eq 3 -and $trace.observations.Count -eq 2 -and $trace.toggle) 'input loss during resume fails with its observations and toggle retained'
$script:requests.Clear()
$script:replies.Enqueue(@{values=@(1,6);raw="Type 'pause' to continue.`r`n"})
$trace=@{}
$rejected=Rejects { Resume-ColorVlc 123 456 @{} $trace }
Check ($rejected -and $trace.status -eq 'failed' -and $trace.reason -and $requests.Count -eq 2 -and $trace.observations.Count -eq 1 -and $trace.qpcEnd -ge $trace.qpcStart) 'failed resume transport is retained and never retried as another toggle'
Check ($count -eq 48) 'complete coarse color contract set'
Write-Output "$count checks passed; no player or desktop capture was used."
