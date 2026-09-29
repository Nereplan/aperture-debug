[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw 'Use a four-part version, for example 2.0.1.0.' }
$parsed = [version]$Version
if (@($parsed.Major,$parsed.Minor,$parsed.Build,$parsed.Revision) | Where-Object { $_ -gt 65535 }) { throw 'Each version component must be between 0 and 65535.' }
$path = Join-Path $PSScriptRoot '../version.json'
$config = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
$config.major = $parsed.Major
$config.minor = $parsed.Minor
$config.patch = $parsed.Build
$config.build = $parsed.Revision
foreach ($field in @('version','versionString','semVer','fileVersion','productVersion')) { $config.$field = $Version }
$config.displayVersion = if ($parsed.Revision -eq 0) { "$($parsed.Major).$($parsed.Minor).$($parsed.Build)" } else { $Version }
$config.preRelease = ''
$config.channel = 'stable'
[IO.File]::WriteAllText([IO.Path]::GetFullPath($path), ($config | ConvertTo-Json -Depth 5) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Host "Version set to $Version. Commit and push, then run the Release workflow."
