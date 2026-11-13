<#
.SYNOPSIS
    Cross-compiles and packages the Linux x86_64 build of Aperture (CachyOS / Arch Linux)
    from Windows or Linux using Zig C++ (or native Clang/GCC).

.DESCRIPTION
    Generates build/linux-generated/Version.h from version.json, incrementally compiles
    shared and Linux platform C++17 translation units into build/linux-obj/, links the
    stripped ELF executable dist/linux-x86_64/aperture, stages runtime assets (fonts,
    locales, Update.sh, aperture.desktop, portable data/), and produces the portable
    release archive dist/Aperture-<version>-linux-x86_64.tar.gz + .sha256.
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$Rebuild,
    [switch]$SkipArchive
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot

# 1. Read authoritative version from version.json
$versionJsonPath = Join-Path $repoRoot "version.json"
$v = Get-Content -LiteralPath $versionJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
$versionString = $v.versionString

# 2. Generate build/linux-generated/Version.h from src/Version.h.in
$genDir = Join-Path $repoRoot "build\linux-generated"
New-Item -ItemType Directory -Force -Path $genDir | Out-Null
$tpl = Get-Content -LiteralPath (Join-Path $repoRoot "src\Version.h.in") -Raw -Encoding UTF8
$versionCsv = "$($v.major),$($v.minor),$($v.patch),$($v.build)"
$translation = "$($v.language)$($v.codePage)"
$rep = @{
    "@APERTURE_VERSION_MAJOR@" = "$($v.major)"
    "@APERTURE_VERSION_MINOR@" = "$($v.minor)"
    "@APERTURE_VERSION_PATCH@" = "$($v.patch)"
    "@APERTURE_VERSION_BUILD@" = "$($v.build)"
    "@APERTURE_VERSION_CSV@" = $versionCsv
    "@APERTURE_VERSION_STRING@" = $v.versionString
    "@APERTURE_DISPLAY_VERSION@" = $v.displayVersion
    "@APERTURE_FILE_VERSION@" = $v.fileVersion
    "@APERTURE_PRODUCT_VERSION@" = $v.productVersion
    "@APERTURE_COMPANY_NAME@" = $v.companyName
    "@APERTURE_FILE_DESCRIPTION@" = $v.fileDescription
    "@APERTURE_INTERNAL_NAME@" = $v.internalName
    "@APERTURE_LEGAL_COPYRIGHT@" = $v.legalCopyright
    "@APERTURE_ORIGINAL_FILENAME@" = $v.originalFilename
    "@APERTURE_PRODUCT_NAME@" = $v.productName
    "@APERTURE_COMMENTS@" = $v.comments
    "@APERTURE_LANG_ID_INT@" = "$($v.languageId)"
    "@APERTURE_CODEPAGE_ID_INT@" = "$($v.codePageId)"
    "@APERTURE_LANG_HEX@" = $v.language
    "@APERTURE_CODEPAGE_HEX@" = $v.codePage
    "@APERTURE_TRANSLATION_STRING@" = $translation
}
foreach ($k in $rep.Keys) {
    $tpl = $tpl.Replace($k, $rep[$k])
}
[System.IO.File]::WriteAllText((Join-Path $genDir "Version.h"), $tpl, [System.Text.UTF8Encoding]::new($false))

# 3. Locate Zig C++ cross-compiler
$zigCandidates = @(
    "$env:TEMP\aperture-zig\zig-windows-x86_64-0.13.0\zig.exe",
    (Get-Command zig -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

if (-not $zigCandidates) {
    throw "Zig C++ cross-compiler not found. Install zig or place it in `$env:TEMP\aperture-zig\zig-windows-x86_64-0.13.0\zig.exe."
}
$zig = @($zigCandidates)[0]

$zigRoot = Split-Path -Parent $zig
$zigLibcDir = Join-Path $zigRoot "lib\libc"
$zigConfigCheck = Join-Path $zigLibcDir "glibc\include\config.h"
if ((Test-Path -LiteralPath $zigLibcDir) -and (-not (Test-Path -LiteralPath $zigConfigCheck))) {
    $zigParent = Split-Path -Parent $zigRoot
    $zigZip = Join-Path $zigParent "zig.zip"
    if (Test-Path -LiteralPath $zigZip) {
        Write-Host "  [FIX] Incomplete Zig libc detected; restoring from $zigZip ..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $zigRoot -Recurse -Force
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($zigZip, $zigParent)
    }
    if (-not (Test-Path -LiteralPath $zigConfigCheck)) {
        throw "Zig C++ installation at '$zigRoot' is incomplete (missing lib\libc\glibc\include\config.h). Re-extract or reinstall Zig 0.13.0."
    }
}

# 4. Prepare object & staging directories
$objDir = Join-Path $repoRoot "build\linux-obj"
$stageDir = Join-Path $repoRoot "dist\linux-x86_64"

if ($Rebuild -and (Test-Path -LiteralPath $objDir)) {
    Remove-Item -LiteralPath $objDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

# 5. Collect Linux target sources
$winOnly = @("main.cpp","FolderWatcher.cpp","HttpTransport.cpp","MediaTransport.cpp","Diagnostics.cpp","FileAssociations.cpp","JumpList.cpp","MediaFoundation.cpp","MediaProbeIsolation.cpp")
$sources = @()
Get-ChildItem (Join-Path $repoRoot "src\*.cpp") | Where-Object { $winOnly -notcontains $_.Name } | ForEach-Object { $sources += $_ }
Get-ChildItem (Join-Path $repoRoot "src\linux\*.cpp") | ForEach-Object { $sources += $_ }

$optFlag = if ($Configuration -eq "Debug") { "-O0" } else { "-O2" }
$objs = @()
$compiledCount = 0

foreach ($src in $sources) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($src.Name)
    $obj = Join-Path $objDir "$base.o"
    $depFile = Join-Path $objDir "$base.d"
    $objs += $obj

    $needsCompile = $Rebuild -or (-not (Test-Path -LiteralPath $obj)) -or (-not (Test-Path -LiteralPath $depFile))
    if (-not $needsCompile) {
        $objTime = (Get-Item -LiteralPath $obj).LastWriteTimeUtc
        if ($src.LastWriteTimeUtc -gt $objTime) {
            $needsCompile = $true
        } else {
            $depRaw = [System.IO.File]::ReadAllText($depFile)
            $depJoined = $depRaw -replace "\\\r?\n", " "
            $colonMatch = [regex]::Match($depJoined, ':(?=\s|$)')
            if (-not $colonMatch.Success) {
                $needsCompile = $true
            } else {
                $rhs = $depJoined.Substring($colonMatch.Index + 1)
                $deps = [regex]::Matches($rhs, '(?:\\.|[^\s\\])+') | ForEach-Object {
                    ($_.Value -replace '\\ ', ' ') -replace '\\\\', '\'
                }
                foreach ($dep in $deps) {
                    if (-not $dep) { continue }
                    if (-not (Test-Path -LiteralPath $dep) -or ((Get-Item -LiteralPath $dep).LastWriteTimeUtc -gt $objTime)) {
                        $needsCompile = $true
                        break
                    }
                }
            }
        }
    }

    if ($needsCompile) {
        Write-Host "  [CXX] $($src.Name)" -ForegroundColor Cyan
        & $zig c++ -target x86_64-linux-gnu.2.31 -std=c++17 $optFlag -fno-autolink -Wno-c++11-narrowing -Wno-constant-conversion -MMD -MF $depFile -c `
            "-I$repoRoot/src/linux/include" "-I$repoRoot/src" "-I$repoRoot" "-I$genDir" `
            $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to compile $($src.FullName)"
        }
        $compiledCount++
    }
}

# 6. Link ELF executable
$outElf = Join-Path $stageDir "aperture"
$stripFlag = if ($Configuration -eq "Release") { "-s" } else { "-g" }
Write-Host "  [LD]  dist/linux-x86_64/aperture ($($objs.Count) objects, $compiledCount recompiled)" -ForegroundColor Cyan
& $zig c++ -target x86_64-linux-gnu.2.31 $optFlag $stripFlag $objs -ldl -lpthread -lm -o $outElf
if ($LASTEXITCODE -ne 0) {
    throw "Failed to link Linux executable $outElf"
}

# 7. Stage portable runtime layout
New-Item -ItemType Directory -Force -Path (Join-Path $stageDir "data") | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "packaging\linux\Update.sh") -Destination (Join-Path $stageDir "Update.sh") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "packaging\linux\aperture.desktop") -Destination (Join-Path $stageDir "aperture.desktop") -Force
if (Test-Path -LiteralPath (Join-Path $repoRoot "README.md")) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $stageDir "README.md") -Force
}
if (Test-Path -LiteralPath (Join-Path $repoRoot "LICENSE")) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $stageDir "LICENSE") -Force
}
if (Test-Path -LiteralPath (Join-Path $repoRoot "THIRD_PARTY_NOTICES.txt")) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "THIRD_PARTY_NOTICES.txt") -Destination (Join-Path $stageDir "THIRD_PARTY_NOTICES.txt") -Force
}

if (Test-Path -LiteralPath (Join-Path $repoRoot "fonts")) {
    $destFonts = Join-Path $stageDir "fonts"
    if (Test-Path -LiteralPath $destFonts) { Remove-Item -LiteralPath $destFonts -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $repoRoot "fonts") -Destination $destFonts -Recurse -Force
}
if (Test-Path -LiteralPath (Join-Path $repoRoot "locales")) {
    $destLocales = Join-Path $stageDir "locales"
    if (Test-Path -LiteralPath $destLocales) { Remove-Item -LiteralPath $destLocales -Recurse -Force }
    Copy-Item -LiteralPath (Join-Path $repoRoot "locales") -Destination $destLocales -Recurse -Force
}

if (-not $SkipArchive) {
    $archiveName = "Aperture-$versionString-linux-x86_64.tar.gz"
    $archivePath = Join-Path $repoRoot "dist\$archiveName"
    if (Test-Path -LiteralPath $archivePath) { Remove-Item -LiteralPath $archivePath -Force }
    tar.exe -czf $archivePath -C (Join-Path $repoRoot "dist") "linux-x86_64"
    if ($LASTEXITCODE -ne 0) { throw "Failed to create Linux release archive $archivePath" }
    $hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText("$archivePath.sha256", "$hash  $archiveName`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "  [PKG] $archivePath (SHA256: $hash)" -ForegroundColor Green
}

$elfItem = Get-Item -LiteralPath $outElf
Write-Host "Linux x86_64 build complete: $($elfItem.FullName) ($([Math]::Round($elfItem.Length / 1MB, 2)) MB)" -ForegroundColor Green
