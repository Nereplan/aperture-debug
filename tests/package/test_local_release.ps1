[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixture = Join-Path $root ('scratch/local-release-test-' + [Guid]::NewGuid().ToString('N'))
$global:ReleaseTestGit = (Get-Command git -CommandType Application | Select-Object -First 1).Source
$global:ReleaseTestState = @{ root = $fixture; calls = @(); failBuild = $false; remoteTag = $false; releases = '[]'; changeSource = $false; missingCommit = $false; failUpload = $false }
$savedActions = $env:GITHUB_ACTIONS

# Intercept only external boundaries; source history uses a real isolated Git repo.
function git {
    if ($args -contains 'ls-remote') {
        $global:LASTEXITCODE = 0
        if ($global:ReleaseTestState.remoteTag) { 'deadbeef refs/tags/v2.0.1.0' }
        return
    }
    & $global:ReleaseTestGit @args
}
function gh {
    $state = $global:ReleaseTestState
    $state.calls += ,@($args)
    $global:LASTEXITCODE = 0
    if (($args[0] -eq 'api' -and $args[1] -match '/commits/' -and $state.missingCommit) -or
        ($args[0] -eq 'release' -and $args[1] -eq 'create' -and $state.failUpload)) {
        $global:LASTEXITCODE = 1
        return
    }
    if ($args[0] -eq 'api') {
        if ($args[1] -match '/commits/') { & $global:ReleaseTestGit -C $state.root rev-parse HEAD }
        else { $state.releases }
    }
}
function powershell.exe {
    $state = $global:ReleaseTestState
    $state.calls += ,@('local-build')
    $global:LASTEXITCODE = if ($state.failBuild) { 1 } else { 0 }
    if (-not $state.failBuild) {
        New-Item -ItemType Directory -Path (Join-Path $state.root 'dist') -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $state.root 'dist/Aperture-2.0.1.0-windows-x64.zip'), 'fixture-package')
        if ($state.changeSource) { Add-Content -LiteralPath (Join-Path $state.root 'source.txt') 'changed during build' }
    }
}
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([string]$Message, [switch]$PrepareOnly, [int]$ReleaseCalls = 0) {
    $global:ReleaseTestState.calls = @()
    $caught = $false
    try { & $publisher -PrepareOnly:$PrepareOnly }
    catch { $caught = $true; Assert ($_.Exception.Message -like "*$Message*") "Unexpected failure: $_" }
    Assert $caught "Expected failure: $Message"
    Assert (@($global:ReleaseTestState.calls | Where-Object { $_[0] -eq 'release' }).Count -eq $ReleaseCalls) 'Failure made unexpected release calls'
}
try {
    $env:GITHUB_ACTIONS = ''
    New-Item -ItemType Directory -Path (Join-Path $fixture 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'scripts/publish_release.ps1') -Destination (Join-Path $fixture 'scripts')
    $publisher = Join-Path $fixture 'scripts/publish_release.ps1'
    Set-Content -LiteralPath (Join-Path $fixture '.gitignore') 'dist/'
    Set-Content -LiteralPath (Join-Path $fixture 'source.txt') 'initial source'
    @{ major = 2; minor = 0; patch = 1; build = 0; versionString = '2.0.1.0'; fileVersion = '2.0.1.0'; productVersion = '2.0.1.0'; channel = 'stable' } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fixture 'version.json')
    & $global:ReleaseTestGit -C $fixture init --quiet
    & $global:ReleaseTestGit -C $fixture remote add origin https://github.com/example/aperture.git
    & $global:ReleaseTestGit -C $fixture add .
    & $global:ReleaseTestGit -C $fixture -c user.name=Fixture -c user.email=fixture@example.com commit --quiet -m 'Initial source'
    & $global:ReleaseTestGit -C $fixture tag v2.0.0.0
    Add-Content -LiteralPath (Join-Path $fixture 'source.txt') 'next change'
    & $global:ReleaseTestGit -C $fixture add .
    & $global:ReleaseTestGit -C $fixture -c user.name=Fixture -c user.email=fixture@example.com commit --quiet -m 'Improve playback'
    Assert ($LASTEXITCODE -eq 0) 'Fixture setup failed'

    & $publisher -PrepareOnly
    Assert (@($global:ReleaseTestState.calls | Where-Object { $_[0] -ne 'local-build' }).Count -eq 0) 'Offline preparation contacted GitHub'
    $notes = Get-Content -LiteralPath (Join-Path $fixture 'dist/Aperture-2.0.1.0-release-notes.md') -Raw
    Assert ($notes.Contains('Improve playback') -and -not $notes.Contains('Initial source')) 'Offline history baseline is wrong'

    $global:ReleaseTestState.calls = @()
    $global:ReleaseTestState.releases = '[{"tag_name":"v2.0.0.0","draft":false,"prerelease":false}]'
    & $publisher
    $calls = $global:ReleaseTestState.calls
    $create = @($calls | Where-Object { $_[0] -eq 'release' -and $_[1] -eq 'create' })
    Assert ($create.Count -eq 1 -and $create[0] -contains '--draft' -and $create[0] -contains '--notes-file') 'Expected one draft with release notes'
    $sha = & $global:ReleaseTestGit -C $fixture rev-parse HEAD
    Assert ($create[0] -contains $sha -and $create[0] -contains 'example/aperture') 'Release provenance is wrong'
    Assert ($calls[-1][1] -eq 'edit' -and $calls[-1] -contains '--draft=false') 'Release was not published after upload'
    $checksumFile = Join-Path $fixture 'dist/Aperture-2.0.1.0-windows-x64.zip.sha256'
    $expectedHash = (Get-FileHash -LiteralPath (Join-Path $fixture 'dist/Aperture-2.0.1.0-windows-x64.zip')).Hash.ToLowerInvariant()
    Assert ((Get-Content -LiteralPath $checksumFile -Raw).StartsWith($expectedHash)) 'Asset checksum is wrong'

    $global:ReleaseTestState.remoteTag = $true
    Expect-Failure 'already exists on GitHub'
    $global:ReleaseTestState.remoteTag = $false
    $global:ReleaseTestState.releases = '[{"tag_name":"v2.0.1.0","draft":true,"prerelease":false}]'
    Expect-Failure 'already exists, including drafts'
    $global:ReleaseTestState.releases = '[{"tag_name":"v2.0.2.0","draft":false,"prerelease":false}]'
    Expect-Failure 'Version must increase'
    $global:ReleaseTestState.releases = '[]'
    $global:ReleaseTestState.missingCommit = $true
    Expect-Failure 'GitHub command failed'
    Assert (@($global:ReleaseTestState.calls | Where-Object { $_[0] -eq 'local-build' }).Count -eq 0) 'Missing remote commit started a build'
    $global:ReleaseTestState.missingCommit = $false
    $global:ReleaseTestState.failUpload = $true
    Expect-Failure 'GitHub command failed' -ReleaseCalls 1
    Assert (@($global:ReleaseTestState.calls | Where-Object { $_[0] -eq 'release' -and $_[1] -eq 'edit' }).Count -eq 0) 'Failed upload published a draft'
    $global:ReleaseTestState.failUpload = $false
    $global:ReleaseTestState.failBuild = $true
    Expect-Failure 'Local build, tests, or packaging failed'
    $global:ReleaseTestState.failBuild = $false
    $global:ReleaseTestState.changeSource = $true
    Expect-Failure 'Source changed during release preparation'
    Expect-Failure 'Commit all source changes' -PrepareOnly
    $env:GITHUB_ACTIONS = 'true'
    Expect-Failure 'outside GitHub Actions'
    Write-Host 'PASS local release: offline preparation, commit history, draft/upload/publish order, checksums, duplicate/version/build/source/runner guards'
} finally {
    $env:GITHUB_ACTIONS = $savedActions
    $resolved = [IO.Path]::GetFullPath($fixture)
    $prefix = [IO.Path]::GetFullPath((Join-Path $root 'scratch')).TrimEnd('\','/') + '\local-release-test-'
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe fixture cleanup: $resolved" }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    Remove-Variable ReleaseTestGit,ReleaseTestState -Scope Global -ErrorAction SilentlyContinue
}
