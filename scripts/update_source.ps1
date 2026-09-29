[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not (Test-Path -LiteralPath (Join-Path $root '.git'))) { throw 'Create or clone the GitHub repository first. Use Build Release.cmd to build this local copy.' }
$changes = & git -C $root status --porcelain
if ($LASTEXITCODE -ne 0) { throw 'Could not read Git status.' }
if ($changes) { throw 'Commit or stash source changes before updating. Saved application data is ignored by Git.' }
& git -C $root pull --ff-only
if ($LASTEXITCODE -ne 0) { throw 'Git update failed. Resolve the branch or connection issue and try again.' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build_and_release.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Build or validation failed. Your previous application and saved data remain available.' }
