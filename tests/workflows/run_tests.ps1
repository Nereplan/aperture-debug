[CmdletBinding()]
param(
    [string]$Seed = '',
    [switch]$AppOnly,
    [string]$Executable = '',
    [string]$SourceDirectory = '',
    [string]$BenchmarkMediaPath = '',
    [switch]$KeepArtifacts
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + '\'
$run = Join-Path $tempBase ('Aperture-workflows-' + [Guid]::NewGuid().ToString('N'))
$reportDirectory = Join-Path $root 'diagnostics/workflows'
New-Item -ItemType Directory -Path $run,$reportDirectory -Force | Out-Null
$mode = if ($AppOnly) { 'app' } else { 'library' }
$log = Join-Path $reportDirectory ("latest-$mode.log")
$report = Join-Path $reportDirectory ("latest-$mode.json")
$seedValues = if ($AppOnly) { @() } elseif ($Seed) { @($Seed.Split(',') | ForEach-Object { [uint32]::Parse($_) }) } else { @(20260918,731,42,(Get-Random -Minimum 1 -Maximum 2147483647)) }
$result = [ordered]@{ mode=$mode; status='FAIL'; seeds=@(); startedUtc=[DateTime]::UtcNow.ToString('o'); artifactDirectory=$null; failure=$null }
if (-not $AppOnly) { $result.seeds = @($seedValues) }
$exitCode = 1
try {
    Set-Content -LiteralPath $log -Value ("Mode=$mode Seeds=" + ($seedValues -join ','))
    Write-Host ("Workflow suite: $mode | seeds=" + ($seedValues -join ','))
    . (Join-Path $PSScriptRoot '../support/native_build.ps1')
    $source = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { Join-Path $root 'src' }
    $exe = Join-Path $run 'workflows.exe'
    $main = if ($AppOnly) { 'app_workflows.cpp' } else { 'library_workflows.cpp' }
    $sources = if ($AppOnly) { @('Json') } else {
        @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery','GlobalSearch','TextInputModel','FolderWatcher','MediaFoundation','MediaProbeIsolation')
    }
    $sourceFiles = @((Join-Path $PSScriptRoot $main)) + @($sources | ForEach-Object { Join-Path $source ($_ + '.cpp') })
    Build-ApertureTestHarness -Sources $sourceFiles -Output $exe -SourceDirectory $source -Libraries @('mfreadwrite.lib','oleaut32.lib','propsys.lib','user32.lib','shell32.lib','advapi32.lib') 2>&1 |
        Tee-Object -FilePath $log -Append | Write-Host
    if ($AppOnly) {
        if (-not $Executable) { $Executable = Join-Path $root 'release/Aperture.exe' }
        $Executable = (Resolve-Path -LiteralPath $Executable).Path
        $appDirectory = Join-Path $run 'app'
        New-Item -ItemType Directory -Path $appDirectory | Out-Null
        Copy-Item -LiteralPath $Executable -Destination (Join-Path $appDirectory 'Aperture.exe')
        $diagnosticPdb = Join-Path $root 'scratch/build/ninja-Release/Aperture.pdb'
        if (Test-Path -LiteralPath $diagnosticPdb) { Copy-Item -LiteralPath $diagnosticPdb -Destination $appDirectory }
        Copy-Item -LiteralPath (Join-Path $root 'libmpv-2.dll') -Destination $appDirectory
        foreach ($asset in @('fonts','locales')) { Copy-Item -LiteralPath (Join-Path $root $asset) -Destination $appDirectory -Recurse }
        $runArgs = @((Join-Path $appDirectory 'Aperture.exe'))
        if ($BenchmarkMediaPath) { $runArgs += (Resolve-Path -LiteralPath $BenchmarkMediaPath).Path }
    } else { $runArgs = @($seedValues | ForEach-Object { [string]$_ }) }
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $exe @runArgs 2>&1 | Tee-Object -FilePath $log -Append | Write-Host
        $workflowExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousErrorAction }
    if ($workflowExit -ne 0) { throw "Workflow assertions failed; see $log" }
    $result.status = 'PASS'
    $exitCode = 0
} catch {
    $result.failure = $_.Exception.Message
    $_ | Out-String | Add-Content -LiteralPath $log
    Write-Host $result.failure -ForegroundColor Red
} finally {
    $result.finishedUtc = [DateTime]::UtcNow.ToString('o')
    if ($KeepArtifacts) { $result.artifactDirectory = $run }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $report -Encoding UTF8
    if (-not $KeepArtifacts) {
        $resolved = [IO.Path]::GetFullPath($run)
        if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notlike 'Aperture-workflows-*') { throw "Unsafe test cleanup: $resolved" }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
exit $exitCode
