[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BeforeSourceDirectory,
    [string]$AfterSourceDirectory = '',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1, 30)][int]$Samples = 7
)
$ErrorActionPreference = 'Stop'
if (-not $AfterSourceDirectory) { $AfterSourceDirectory = Join-Path $PSScriptRoot '../../src' }
$sources = @{
    before = (Resolve-Path -LiteralPath $BeforeSourceDirectory).Path
    after = (Resolve-Path -LiteralPath $AfterSourceDirectory).Path
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
$runRoot = Join-Path $output ('run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot -Force | Out-Null
. (Join-Path $PSScriptRoot '../support/native_build.ps1')
$executables = @{}
foreach ($version in @('before', 'after')) {
    $build = Join-Path $runRoot $version
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    $exe = Join-Path $build 'thumbnail-purge-benchmark.exe'
    $source = $sources[$version]
    $units = @('MpvPlayer', 'Json', 'Settings', 'Localization', 'AudioConfig', 'ChannelPolicy') |
        ForEach-Object { Join-Path $source ($_ + '.cpp') }
    Build-ApertureTestHarness -SourceDirectory $source -Output $exe -Sources @(
        $units + (Join-Path $PSScriptRoot 'thumbnail_purge_benchmark.cpp')
    ) -Libraries @('oleaut32.lib', 'propsys.lib')
    $executables[$version] = $exe
}
# Finish both builds first, then alternate process order. Sample zero is warmup.
$csv = [Collections.Generic.List[string]]::new()
$csv.Add('version,scenario,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,retained_cpp_bytes,removed')
for ($sample = 0; $sample -le $Samples; ++$sample) {
    $order = if ($sample % 2) { @('after', 'before') } else { @('before', 'after') }
    foreach ($version in $order) {
        $profile = Join-Path (Split-Path -Parent $executables[$version]) 'profiles'
        $rows = & $executables[$version] $profile $sample
        if ($LASTEXITCODE -ne 0) { throw "Thumbnail purge benchmark failed: $version sample $sample" }
        foreach ($row in $rows) { $csv.Add("$version,$row") }
        $csv | Set-Content -LiteralPath (Join-Path $output 'results.csv') -Encoding utf8
        Write-Host "Completed $version sample $sample of $Samples."
    }
}
$csv | Set-Content -LiteralPath (Join-Path $output 'results.csv') -Encoding utf8
@{
    beforeSource = $sources.before; afterSource = $sources.after
    runRoot = $runRoot; samples = $Samples; warmupSample = 0
    beforeExecutableSha256 = (Get-FileHash -LiteralPath $executables.before).Hash
    afterExecutableSha256 = (Get-FileHash -LiteralPath $executables.after).Hash
    limits = 'Warm filesystem metadata; single available logical processor; empty cache payloads; C++ requested heap excludes OS directory buffers, process RSS and GPU memory. Fixtures and builds are retained below runRoot.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'run.json') -Encoding utf8
Write-Output "Benchmark results: $(Join-Path $output 'results.csv')"
