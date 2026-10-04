# One object pool per central test invocation. No persistent compiler cache.
function Build-ApertureTestHarness {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string[]]$Sources,
        [Parameter(Mandatory)][string]$Output,
        [Parameter(Mandatory)][string[]]$Libraries,
        [string]$SourceDirectory = '',
        [string]$Manifest = ''
    )
    $repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
    if (-not $SourceDirectory) { $SourceDirectory = Join-Path $repo 'src' }
    $SourceDirectory = [IO.Path]::GetFullPath($SourceDirectory)
    $outDir = Split-Path -Parent ([IO.Path]::GetFullPath($Output))
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'MSVC x64 toolchain not found.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $version = (Get-Content -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
    $compiler = Join-Path $vs "VC/Tools/MSVC/$version/bin/Hostx64/x64/cl.exe"
    $flags = @('/nologo','/utf-8','/O2','/MT','/bigobj','/EHsc','/std:c++17','/DUNICODE','/D_UNICODE',
        "/I`"$SourceDirectory`"", "/I`"$repo`"", "/I`"$(Join-Path $repo 'tests/support')`"")
    $core = @()
    $harness = @()
    $prefix = $SourceDirectory.TrimEnd('\','/') + '\'
    foreach ($source in $Sources) {
        $full = [IO.Path]::GetFullPath($source.Trim('"'))
        if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $core += $full }
        else { $harness += $full }
    }
    $pool = $env:APERTURE_NATIVE_TEST_OBJECTS
    if ($pool) {
        $pool = [IO.Path]::GetFullPath($pool)
        $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
        if (-not $pool.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $pool) -notmatch '^Aperture-test-objects-[a-f0-9]{32}$') { throw 'Unsafe native test object pool.' }
        $entry = Get-Item -LiteralPath $pool -Force
        for ($ancestor = $entry; $null -ne $ancestor; $ancestor = $ancestor.Parent) {
            if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked native test object pools are unsupported.' }
        }
        # Conservative content key: compiler, exact flags, every production input
        # and shared test header. Alternate/mutated source trees cannot collide.
        $inputs = @(Get-ChildItem -LiteralPath $SourceDirectory -Recurse -File |
            Where-Object Extension -in @('.h','.hpp','.cpp','.in'))
        $inputs += @(Get-ChildItem -LiteralPath $PSScriptRoot -File -Filter '*.h')
        $key = @($compiler, (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash, ($flags -join ' '))
        foreach ($input in $inputs | Sort-Object FullName) {
            $key += $input.FullName + ':' + (Get-FileHash -LiteralPath $input.FullName -Algorithm SHA256).Hash
        }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($key -join "`n"))).Replace('-','').ToLowerInvariant() }
        finally { $sha.Dispose() }
        $objects = Join-Path $pool $digest
    } else { $objects = Join-Path $outDir 'objects' }
    New-Item -ItemType Directory -Path $objects -Force | Out-Null
    if ((Get-Item -LiteralPath $objects -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked object directory is unsupported.' }
    $missing = @()
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($source in $core) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
        if (-not $names.Add($name)) { throw "Duplicate production object name: $name" }
        if (-not (Test-Path -LiteralPath (Join-Path $objects $name) -PathType Leaf)) { $missing += $source }
    }
    $batch = @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b %errorlevel%')
    $staging = Join-Path $outDir ('compile-' + [guid]::NewGuid().ToString('N'))
    if ($missing.Count) {
        New-Item -ItemType Directory -Path $staging | Out-Null
        $compile = Join-Path $outDir 'core.rsp'
        @($flags + @('/c', "/Fo`"$staging\\`"") + @($missing | ForEach-Object { "`"$_`"" })) |
            Set-Content -LiteralPath $compile -Encoding UTF8
        $batch += @("cl.exe @`"$compile`"", 'if errorlevel 1 exit /b %errorlevel%')
    }
    $linkObjects = foreach ($source in $core) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
        $path = if ($missing -contains $source) { Join-Path $staging $name } else { Join-Path $objects $name }
        "`"$path`""
    }
    $link = Join-Path $outDir 'harness.rsp'
    $responseArgs = $flags + @("/Fo`"$outDir\\`"", "/Fe:`"$Output`"") +
        @($harness | ForEach-Object { "`"$_`"" }) + @($linkObjects) + $Libraries + @('/link','/SUBSYSTEM:CONSOLE')
    if ($Manifest) { $responseArgs += @('/MANIFEST:EMBED', "/MANIFESTINPUT:`"$Manifest`"") }
    ($responseArgs -join ' ') | Set-Content -LiteralPath $link -Encoding UTF8
    $batch += @("cl.exe @`"$link`"", 'exit /b %errorlevel%')
    $command = Join-Path $outDir 'build.cmd'
    $batch | Set-Content -LiteralPath $command -Encoding ASCII
    & cmd.exe /d /c "`"$command`""
    if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed: $LASTEXITCODE" }
    foreach ($source in $missing) {
        $name = [IO.Path]::GetFileNameWithoutExtension($source) + '.obj'
        Move-Item -LiteralPath (Join-Path $staging $name) -Destination (Join-Path $objects $name) -Force
    }
    Write-Host "Native build: $($missing.Count) production files compiled, $($core.Count - $missing.Count) reused."
}
