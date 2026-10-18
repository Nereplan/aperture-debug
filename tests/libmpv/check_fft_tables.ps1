[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BaselineArchive,
    [Parameter(Mandatory)][string]$CandidateArchive,
    [Parameter(Mandatory)][string]$IncludeDirectory,
    [Parameter(Mandatory)][string]$OutputDir,
    [ValidateSet('Experimental','Enabled')][string]$CandidateMode = 'Experimental',
    [string]$Compiler = 'C:\msys64\mingw64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 is required.' }
foreach ($name in @('BaselineArchive','CandidateArchive','IncludeDirectory','Compiler')) {
    Set-Variable -Name $name -Value (Resolve-Path -LiteralPath (Get-Variable -Name $name -ValueOnly)).Path
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) { throw 'OutputDir must be new.' }
New-Item -ItemType Directory -Path $OutputDir | Out-Null
function Invoke-ProbeProcess($Executable, $Arguments, $LogName, $LazyTables) {
    $info = [Diagnostics.ProcessStartInfo]::new($Executable)
    $info.WorkingDirectory = $OutputDir; $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.Environment['PATH'] = (Split-Path -Parent $Compiler) + ';' + $info.Environment['PATH']
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    [void]$info.Environment.Remove('APERTURE_TEST_LAZY_TX_TABLES')
    if ($LazyTables) { $info.Environment['APERTURE_TEST_LAZY_TX_TABLES'] = '1' }
    $process = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(180000)) { $process.Kill(); throw "$LogName timed out." }
        $output = $stdout.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $OutputDir "$LogName.jsonl"), $output)
        [IO.File]::WriteAllText((Join-Path $OutputDir "$LogName.stderr.txt"), $stderr.GetAwaiter().GetResult())
        if ($process.ExitCode -ne 0) { throw "$LogName failed ($($process.ExitCode)); see $OutputDir" }
        return $output
    } finally { $process.Dispose() }
}
$executables = @{}
foreach ($variant in @(@{name='baseline'; archive=$BaselineArchive},@{name='candidate'; archive=$CandidateArchive})) {
    $exe = Join-Path $OutputDir ($variant.name+'.exe')
    $arguments = @('-std=c11','-O2','-Wall','-Wextra',('-I'+$IncludeDirectory),
        (Join-Path $PSScriptRoot 'fft_table_probe.c'),$variant.archive,'-static','-luser32','-lbcrypt','-latomic','-lm','-o',$exe)
    $null = Invoke-ProbeProcess $Compiler $arguments ($variant.name+'-build') $false
    $executables[$variant.name] = $exe
}
$checks = [Collections.Generic.List[object]]::new()
foreach ($mode in @('native','scalar')) {
    $arguments = if ($mode -eq 'scalar') { @('--scalar') } else { @() }
    $baseline = Invoke-ProbeProcess $executables.baseline $arguments ("baseline-$mode") $false
    $baselineRecords = @($baseline -split '\r?\n' | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
    if ($baselineRecords.Count -ne 78 -or @($baselineRecords | Where-Object error -LT 0).Count) { throw "Incomplete baseline $mode." }
    $states = if ($CandidateMode -eq 'Enabled') { @($false) } else { @($false,$true) }
    foreach ($lazy in $states) {
        $name = if ($CandidateMode -eq 'Enabled') { "candidate-enabled-$mode" } elseif ($lazy) { "candidate-lazy-$mode" } else { "candidate-writable-$mode" }
        $candidate = Invoke-ProbeProcess $executables.candidate $arguments $name $lazy
        if ($candidate -cne $baseline) { throw "Transform output differs: $name" }
        $checks.Add([ordered]@{mode=$mode; lazyTables=($CandidateMode -eq 'Enabled' -or $lazy); featureFlagRequested=$lazy; transforms=$baselineRecords.Count; bitExact=$true})
    }
}
$failure = Invoke-ProbeProcess $executables.candidate @('--fail-protect') 'candidate-protection-failure' ($CandidateMode -eq 'Experimental') | ConvertFrom-Json
if (-not $failure.protection_failure -or $failure.error -ge 0 -or $failure.blocked_transitions -lt 1) { throw 'Protection failure was not exercised.' }
[ordered]@{
    baselineArchive=$BaselineArchive; candidateArchive=$CandidateArchive
    baselineSha256=(Get-FileHash -LiteralPath $BaselineArchive).Hash
    candidateSha256=(Get-FileHash -LiteralPath $CandidateArchive).Hash
    probeSha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'fft_table_probe.c')).Hash
    candidateMode=$CandidateMode
    limits='FFT output equivalence and concurrent initialization checks, not an exhaustive codec or player test.'
    checks=@($checks.ToArray())
    protectionFailure=$failure
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir 'checks.json') -Encoding utf8
Write-Output 'FFT checks passed: native/scalar output equivalence, concurrent initialization, large transforms and allocation failure.'
