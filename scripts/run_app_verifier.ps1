param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) "debug\Aperture.exe"),
    [string[]]$ArgumentList = @(),
    [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"
$Executable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Aperture executable not found: $Executable"
}

$verifier = Join-Path $env:SystemRoot "System32\appverif.exe"
if (-not (Test-Path -LiteralPath $verifier -PathType Leaf)) {
    throw "Microsoft Application Verifier is not installed."
}

$imageName = Split-Path -Leaf $Executable
$tests = @("Heaps", "Handles", "Locks", "Memory", "Exceptions")
$enabled = $false
try {
    $enableArguments = @("-enable") + $tests + @("-for", $imageName)
    $configuration = Start-Process -FilePath $verifier -ArgumentList $enableArguments -PassThru -Wait
    if ($configuration.ExitCode -ne 0) {
        throw "Application Verifier configuration failed with code $($configuration.ExitCode)"
    }
    $enabled = $true

    if ($ConfigureOnly) {
        Write-Host "Application Verifier configuration accepted for $imageName; cleanup follows." -ForegroundColor Green
        return
    }

    Write-Host "Running $imageName under Application Verifier. Close Aperture to finish verification." -ForegroundColor Cyan
    $process = Start-Process -FilePath $Executable -ArgumentList $ArgumentList -PassThru -Wait
    if ($process.ExitCode -ne 0) {
        throw "Aperture exited with code $($process.ExitCode) under Application Verifier. Inspect the debugger or Application Verifier event log."
    }
    Write-Host "Application Verifier run completed without a failing process exit." -ForegroundColor Green
}
finally {
    if ($enabled) {
        $disable = Start-Process -FilePath $verifier -ArgumentList @("-disable", "*", "-for", $imageName) -PassThru -Wait
        $delete = Start-Process -FilePath $verifier -ArgumentList @("-delete", "settings", "-for", $imageName) -PassThru -Wait
        if ($disable.ExitCode -ne 0 -or $delete.ExitCode -ne 0) {
            Write-Warning "Application Verifier settings for $imageName could not be fully removed."
        }
    }
}
