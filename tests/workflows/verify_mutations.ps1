<# Verifies that workflow assertions reject real behavioral regressions.
   Mutations affect isolated source copies only. Compilation errors do not count. #>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + '\'
$run = Join-Path $tempBase ('Aperture-mutations-' + [Guid]::NewGuid().ToString('N'))
$source = Join-Path $run 'src'
$reports = Join-Path $root 'diagnostics/workflows'
New-Item -ItemType Directory -Path $run,$reports -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'src') -Destination $source -Recurse
$mutations = @(
    @{ name='reverse-duration'; file='LibraryQuery.cpp';
       old='if (a.durationSec < b.durationSec) primaryResult = -1;'; new='if (a.durationSec < b.durationSec) primaryResult = 1;';
       old2='else if (a.durationSec > b.durationSec) primaryResult = 1;'; new2='else if (a.durationSec > b.durationSec) primaryResult = -1;';
       expected='Duration order differs from oracle' },
    @{ name='lose-resume-position'; file='MediaLibrary.cpp';
       old='j["lastPosSec"] = m.lastPosSec;'; new='j["lastPosSec"] = 0.0;';
       expected='Resume position lost after reopen' }
)
try {
    foreach ($mutation in $mutations) {
        $file = Join-Path $source $mutation.file
        $original = [IO.File]::ReadAllText($file)
        if (-not $original.Contains($mutation.old)) { throw "Mutation target disappeared: $($mutation.name)" }
        $changed = $original.Replace($mutation.old, $mutation.new)
        if ($mutation.old2) {
            if (-not $changed.Contains($mutation.old2)) { throw 'Second mutation target disappeared' }
            $changed = $changed.Replace($mutation.old2, $mutation.new2)
        }
        [IO.File]::WriteAllText($file, $changed, [Text.UTF8Encoding]::new($false))
        $log = Join-Path $reports ("mutation-$($mutation.name).log")
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_tests.ps1') -SourceDirectory $source -Seed 20260918 *> $log
        $code = $LASTEXITCODE
        [IO.File]::WriteAllText($file, $original, [Text.UTF8Encoding]::new($false))
        $evidence = [IO.File]::ReadAllText($log)
        if ($code -eq 0 -or $evidence.Contains('Workflow harness compilation failed') -or
            -not $evidence.Contains('FAIL ' + $mutation.expected)) {
            throw "Mutation was not rejected by the expected assertion: $($mutation.name). See $log"
        }
        Write-Host "PASS mutation rejected by behavioral assertion: $($mutation.name)"
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'Aperture-mutations-*') { throw "Unsafe mutation cleanup: $resolved" }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
