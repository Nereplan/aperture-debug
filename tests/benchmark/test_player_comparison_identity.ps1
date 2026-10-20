$ErrorActionPreference = 'Stop'
$parseErrors = $null; $tokens = $null
$comparison = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'compare_players.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Player comparison parameter parsing failed.' }
# Exercise the actual public parameter block without executing player setup.
$bind = [scriptblock]::Create($comparison.ParamBlock.Extent.Text + '; $SamplingStartMediaSeconds')
$inputs = @{AperturePath='unused';MediaPath='unused';OutputDir='unused'}
foreach ($value in @(0.0,20.5,86400.0)) {
    if ((& $bind @inputs -SamplingStartMediaSeconds $value) -ne $value) { throw 'Media alignment was rounded.' }
}
foreach ($value in @(-0.5,86400.5,[double]::NaN,[double]::PositiveInfinity,[double]::NegativeInfinity)) {
    $rejected = $false
    try { & $bind @inputs -SamplingStartMediaSeconds $value | Out-Null }
    catch [System.Management.Automation.ParameterBindingException] { $rejected = $true }
    if (-not $rejected) { throw 'Invalid media alignment was accepted.' }
}
Write-Output 'PASS fractional alignment binds without rounding and invalid positions are rejected'
. (Join-Path $PSScriptRoot 'player_comparison_identity.ps1')
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('Aperture-runtime-identity-' + [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $fixtureRoot 'original'
$clone = Join-Path $fixtureRoot 'clone'
New-Item -ItemType Directory -Path (Join-Path $fixture 'plugins') -Force | Out-Null
try {
    [IO.File]::WriteAllText((Join-Path $fixture 'player.exe'), 'stub executable')
    [IO.File]::WriteAllText((Join-Path $fixture 'plugins/codec.dll'), 'codec-a')
    [IO.File]::WriteAllText((Join-Path $fixture 'I-İ-ı.txt'), 'unicode')
    $first = Get-PlayerRuntimeManifest $fixture
    Copy-Item -LiteralPath $fixture -Destination $clone -Recurse
    $copy = Get-PlayerRuntimeManifest $clone
    if ($copy.sha256 -cne $first.sha256 -or $copy.files.Count -ne 3) { throw 'A copied runtime changed identity.' }
    Write-Output 'PASS location-independent complete runtime identity'
    $oldCulture = [Threading.Thread]::CurrentThread.CurrentCulture
    try {
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('tr-TR')
        if ((Get-PlayerRuntimeManifest $clone).sha256 -cne $first.sha256) { throw 'Identity depends on sort culture.' }
    } finally { [Threading.Thread]::CurrentThread.CurrentCulture = $oldCulture }
    Write-Output 'PASS ordinal inventory survives a Turkish sort culture'
    [IO.File]::WriteAllText((Join-Path $clone 'plugins/codec.dll'), 'codec-b')
    $changed = Get-PlayerRuntimeManifest $clone
    if ($changed.bytes -ne $first.bytes -or $changed.sha256 -ceq $first.sha256) { throw 'Same-length DLL replacement was not detected.' }
    Write-Output 'PASS same-length dependency replacement changes identity'
    [IO.File]::WriteAllText((Join-Path $clone 'empty'), '')
    $added = Get-PlayerRuntimeManifest $clone
    if ($added.sha256 -ceq $changed.sha256 -or $added.files.Count -ne 4) { throw 'An empty added input was not detected.' }
    Rename-Item -LiteralPath (Join-Path $clone 'empty') -NewName 'renamed'
    if ((Get-PlayerRuntimeManifest $clone).sha256 -ceq $added.sha256) { throw 'A renamed input was not detected.' }
    Write-Output 'PASS added and renamed zero-byte inputs change identity'
    New-Item -ItemType Junction -Path (Join-Path $clone 'linked') -Target $fixture | Out-Null
    $rejected = $false
    try { Get-PlayerRuntimeManifest $clone | Out-Null } catch { if ($_.Exception.Message -match 'Linked runtime') { $rejected = $true } else { throw } }
    if (-not $rejected) { throw 'A linked dependency tree was accepted.' }
    Write-Output 'PASS linked inputs are rejected'
} finally {
    $link = Join-Path $clone 'linked'
    if (Test-Path -LiteralPath $link) { [IO.Directory]::Delete($link) }
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
    if (-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-runtime-identity-[a-f0-9]{32}$') { throw 'Unsafe runtime identity test cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
