$ErrorActionPreference = 'Stop'
$rootDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $rootDir 'tests/support/native_build.ps1')
$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$tempDir = Join-Path $tempBase ('Aperture_diagnostics_tests_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir | Out-Null
try {
    $exe = Join-Path $tempDir 'Aperture_Diagnostics_Tests.exe'
    $diagnosticsDir = Join-Path $tempDir 'diagnostics'
    # Include the production implementation to test its internal live-dump
    # writer without exposing an application API used only by this harness.
    $buildArgs = @{
        Sources = @((Join-Path $rootDir 'src/Json.cpp'), (Join-Path $PSScriptRoot 'diagnostics_test.cpp'))
        Output = $exe
        Libraries = @('shell32.lib','shlwapi.lib','dbghelp.lib','ole32.lib','advapi32.lib','user32.lib')
    }
    Build-ApertureTestHarness @buildArgs
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Diagnostics log/startup/dump verification failed with code $LASTEXITCODE" }
    $child = Start-Process -FilePath $exe -ArgumentList '--crash' -WindowStyle Hidden -PassThru -Wait
    if ($child.ExitCode -eq 0) { throw 'Crash test unexpectedly exited successfully' }
    foreach ($kind in @('crash','hang')) {
        $dumps = @(Get-ChildItem -LiteralPath $diagnosticsDir -Filter "$kind-*.dmp" -File)
        if (-not $dumps.Count -or @($dumps | Where-Object Length -eq 0).Count) {
            throw "$kind capture did not produce a nonempty portable dump"
        }
    }
    Write-Host 'Diagnostics tests passed: root-local log, startup report, live dump writer, and crash dump' -ForegroundColor Green
} finally {
    $resolved = [IO.Path]::GetFullPath($tempDir)
    if (-not $resolved.StartsWith($tempBase,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture_diagnostics_tests_[a-f0-9]{32}$') { throw 'Unsafe diagnostics test cleanup.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
