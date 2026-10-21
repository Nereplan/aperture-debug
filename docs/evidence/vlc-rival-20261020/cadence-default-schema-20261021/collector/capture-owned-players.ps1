$ErrorActionPreference='Stop'
$out='C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-rival-20261019\cadence-collector-20261021'
$pm='C:\Users\Alperen\AppData\Local\Codex\tools\PresentMon\2.6.0\PresentMon-2.6.0-x64.exe'
$expected='b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'
if((Get-FileHash -LiteralPath $pm).Hash -ne $expected){throw 'PresentMon does not match its verified official release digest.'}
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Collector did not receive the approved temporary elevation.'}
$session='ApertureCadence-'+[guid]::NewGuid().ToString('N')
$start=[Diagnostics.ProcessStartInfo]::new($pm)
$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
$argsList=@('--process_name','Aperture.exe','--process_name','vlc.exe','--session_name',$session,'--output_file',(Join-Path $out 'presents.csv'),'--timed','1200','--terminate_after_timed','--qpc_time','--write_display_metadata','--no_console_stats','--no_track_gpu','--no_track_input')
foreach($arg in $argsList){$start.ArgumentList.Add($arg)}
$p=[Diagnostics.Process]::Start($start)
$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
$identity=[ordered]@{startedUtc=[DateTime]::UtcNow.ToString('o');scope='One temporarily elevated, bounded PresentMon collector. Only Aperture.exe/vlc.exe names are collected; analysis must retain only the exact coordinator-owned PID/start-time/run windows and reject any foreign player. No resource CPU ranking.';presentMonPid=$p.Id;presentMonSha256=$expected;arguments=$argsList;sessionName=$session;maximumSeconds=1200;status='running'}
$identity|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'started.json') -Encoding utf8
try {
 if(-not $p.WaitForExit(1230000)){$p.Kill();$p.WaitForExit();$identity.status='deadline-killed'}else{$identity.status='completed'}
 $identity.exitCode=$p.ExitCode;$identity.endedUtc=[DateTime]::UtcNow.ToString('o')
 [IO.File]::WriteAllText((Join-Path $out 'stdout.txt'),$stdout.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 [IO.File]::WriteAllText((Join-Path $out 'stderr.txt'),$stderr.GetAwaiter().GetResult(),[Text.UTF8Encoding]::new($false))
 $identity.lossWarning=[bool](Select-String -LiteralPath (Join-Path $out 'stdout.txt'),(Join-Path $out 'stderr.txt') -Pattern 'ETW (buffers|events) were lost|overflowed present events detected' -Quiet)
 $identity.csvExists=Test-Path -LiteralPath (Join-Path $out 'presents.csv')
 $identity.captureQualified=($p.ExitCode -eq 0 -and $identity.csvExists -and -not $identity.lossWarning -and $identity.status -eq 'completed')
} finally {
 $identity|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $out 'finished.json') -Encoding utf8
 $p.Dispose()
}