<#
.SYNOPSIS
    Build, test, and package locally, then upload a versioned GitHub release.
.DESCRIPTION
    Requires a clean, committed checkout. -PrepareOnly performs local preparation
    without contacting GitHub. Publication requires HEAD to exist on origin and
    refuses existing release tags. Never invokes GitHub Actions or pushes source.
#>
[CmdletBinding()]
param([switch]$PrepareOnly)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ($env:GITHUB_ACTIONS -eq 'true') { throw 'Releases must be built and published locally, outside GitHub Actions.' }

function Invoke-Git([string[]]$Arguments) {
    $result = & git -C $root @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed." }
    return $result
}
function Invoke-Gh([string[]]$Arguments) {
    $result = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "GitHub command failed: gh $($Arguments -join ' '). No existing release was overwritten." }
    return $result
}
function Assert-SourceUnchanged {
    if ((Invoke-Git @('rev-parse','HEAD')) -ne $script:commit -or (Invoke-Git @('status','--porcelain'))) {
        throw 'Source changed during release preparation. Commit your changes and start again.'
    }
}

if (Invoke-Git @('status','--porcelain')) { throw 'Commit all source changes before preparing a release.' }
$commit = Invoke-Git @('rev-parse','HEAD')
$remote = Invoke-Git @('config','--get','remote.origin.url')
if ($remote -notmatch '^((https://github\.com/)|(git@github\.com:)|(ssh://git@github\.com/))([^/]+/[^/]+?)(?:\.git)?/?$') {
    throw 'origin must point to a GitHub owner/repository.'
}
$repository = $Matches[5]
if ($repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid GitHub repository in origin.' }
$config = Get-Content -LiteralPath (Join-Path $root 'version.json') -Raw | ConvertFrom-Json
$version = "$($config.major).$($config.minor).$($config.patch).$($config.build)"
if ($config.versionString -ne $version -or $config.fileVersion -ne $version -or $config.productVersion -ne $version -or $config.channel -ne 'stable') {
    throw 'Use scripts/set_version.ps1 to set a consistent four-part stable version.'
}
$tag = "v$version"
$previousTag = ''
if (-not $PrepareOnly) {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'Install GitHub CLI and run gh auth login before publishing.' }
    # Reading the remote commit also validates authentication and repository access.
    $remoteCommit = Invoke-Gh @('api',"repos/$repository/commits/$commit",'--jq','.sha')
    if ($remoteCommit -ne $commit) { throw 'Push the source commit to origin before publishing.' }
    if (Invoke-Git @('ls-remote','origin',"refs/tags/$tag")) { throw "Tag $tag already exists on GitHub. Increase the version; releases are never overwritten." }
    $releasesJson = Invoke-Gh @('api',"repos/$repository/releases?per_page=100")
    $releases = $releasesJson | ConvertFrom-Json
    if ($releases | Where-Object tag_name -eq $tag) { throw "Release $tag already exists, including drafts. Review it on GitHub or increase the version." }
    $latest = $releases | Where-Object { -not $_.draft -and -not $_.prerelease } | Select-Object -First 1
    if ($latest) { $previousTag = $latest.tag_name }
} else {
    # Preparation is offline; use the most recent reachable local version tag.
    $previousTag = & git -C $root describe --tags --abbrev=0 --match 'v*' HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { $previousTag = '' }
}
if ($previousTag) {
    if ($previousTag -notmatch '^v\d+\.\d+\.\d+\.\d+$') { throw "Previous release tag has an unsupported version: $previousTag" }
    if ([version]$version -le [version]$previousTag.Substring(1)) { throw "Version must increase beyond $previousTag." }
    [void](Invoke-Git @('rev-parse','--verify',"refs/tags/$previousTag"))
    [void](Invoke-Git @('merge-base','--is-ancestor',"refs/tags/$previousTag",$commit))
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build_and_release.ps1') -FailFast -Repository $repository
if ($LASTEXITCODE -ne 0) { throw 'Local build, tests, or packaging failed. Nothing was uploaded.' }
Assert-SourceUnchanged
$asset = Join-Path $root "dist/Aperture-$version-windows-x64.zip"
if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) { throw 'Local release ZIP is missing.' }
$checksum = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
$checksumPath = "$asset.sha256"
[IO.File]::WriteAllText($checksumPath, "$checksum  $([IO.Path]::GetFileName($asset))`n", [Text.UTF8Encoding]::new($false))
$range = if ($previousTag) { "refs/tags/$previousTag..$commit" } else { $commit }
$changes = @(Invoke-Git @('log','--reverse','--format=- %s (%H)',$range))
$notes = @("# Aperture $version", '', "Source commit: [$commit](https://github.com/$repository/commit/$commit)", '', '## Changes', '') + $changes
if ($previousTag) { $notes += @('', "[Full comparison](https://github.com/$repository/compare/$previousTag...$tag)") }
$notes += @('', 'Built, regression tested, and packaged locally on Windows x64.', '', "ZIP SHA-256: ``$checksum``")
$notesPath = Join-Path $root "dist/Aperture-$version-release-notes.md"
[IO.File]::WriteAllText($notesPath, ($notes -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
if ($PrepareOnly) {
    Write-Host "Prepared locally: $asset`nRelease notes: $notesPath`nNothing uploaded." -ForegroundColor Green
    return
}
Assert-SourceUnchanged
# The draft keeps an incomplete upload out of the updater's latest-release feed.
[void](Invoke-Gh @('release','create',$tag,$asset,$checksumPath,'--repo',$repository,'--target',$commit,'--title',"Aperture $version",'--notes-file',$notesPath,'--draft'))
[void](Invoke-Gh @('release','edit',$tag,'--repo',$repository,'--draft=false','--latest'))
Write-Host "Published: https://github.com/$repository/releases/tag/$tag" -ForegroundColor Green
