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
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 toolchain not found' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $source = if ($SourceDirectory) { (Resolve-Path -LiteralPath $SourceDirectory).Path } else { Join-Path $root 'src' }
    $exe = Join-Path $run 'workflows.exe'
    $response = Join-Path $run 'compile.rsp'
    $main = if ($AppOnly) { 'app_workflows.cpp' } else { 'library_workflows.cpp' }
    $argsList = @('/nologo','/utf-8','/O2','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
        "/I`"$source`"", "/Fo`"$run\\`"", "/Fe:`"$exe`"", "`"$(Join-Path $PSScriptRoot $main)`"")
    $sources = if ($AppOnly) { @('Json') } else {
        @('Json','Settings','ChannelPolicy','Localization','AudioConfig','MediaLibrary','LibraryQuery','GlobalSearch','TextInputModel','FolderWatcher','MediaFoundation')
    }
    foreach ($file in $sources) { $argsList += "`"$(Join-Path $source ($file + '.cpp'))`"" }
    $argsList += @('mfreadwrite.lib','oleaut32.lib','propsys.lib','user32.lib','shell32.lib','advapi32.lib')
    Set-Content -LiteralPath $response -Value $argsList -Encoding UTF8
    & cmd.exe /d /c "`"$vcvars`" && cl.exe @`"$response`"" 2>&1 | Tee-Object -FilePath $log -Append | Write-Host
    if ($LASTEXITCODE -ne 0) { throw 'Workflow harness compilation failed' }
    if ($AppOnly) {
        if (-not $Executable) { $Executable = Join-Path $root 'release/Aperture.exe' }
        $Executable = (Resolve-Path -LiteralPath $Executable).Path
        $appDirectory = Join-Path $run 'app'
        New-Item -ItemType Directory -Path $appDirectory | Out-Null
        Copy-Item -LiteralPath $Executable -Destination (Join-Path $appDirectory 'Aperture.exe')
        Copy-Item -LiteralPath (Join-Path $root 'libmpv-2.dll') -Destination $appDirectory
        foreach ($asset in @('fonts','locales')) { Copy-Item -LiteralPath (Join-Path $root $asset) -Destination $appDirectory -Recurse }
        $runArgs = @((Join-Path $appDirectory 'Aperture.exe'))
        if ($BenchmarkMediaPath) { $runArgs += (Resolve-Path -LiteralPath $BenchmarkMediaPath).Path }
    } else { $runArgs = @($seedValues | ForEach-Object { [string]$_ }) }
    & $exe @runArgs 2>&1 | Tee-Object -FilePath $log -Append | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "Workflow assertions failed; see $log" }
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
