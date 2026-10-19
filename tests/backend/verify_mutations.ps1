# Deliberately break production behavior in disposable copies. A compiler error
# or unrelated failing test is not evidence that a regression is detected.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/') + '\'
$run = Join-Path $tempRoot ('Aperture-backend-mutations-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$source = Join-Path $run 'src'
Copy-Item -LiteralPath (Join-Path $repo 'src') -Destination $source -Recurse
$mutations = @(
    @{ name='bookmark-extensions'; file='MediaLibrary.cpp'; old='bm.extraFields = std::move(bJson);'; replacement='bm.extraFields = JsonValue();'; test='NestedPreferencesBookmarksAndUnknownFieldsSurviveSaveAndRelocation'; expected='actual.bookmarks.at(0).extraFields.get("nested").get("value").as_int() == 42' },
    @{ name='settings-extensions'; file='Settings.cpp'; old='JsonValue root = persistedFields.is_object() ? persistedFields : JsonValue::Object();'; replacement='JsonValue root = JsonValue::Object();'; test='SettingsDefaultsCustomBudgetsAndExtensionsSurviveRestart'; expected='result.get("extension").get("nested").as_string() == "retained"' },
    @{ name='cache-budget'; file='CacheStorage.h'; old='if (!expectedContent && !fits()) {'; replacement='if (false) {'; test='RealWritesRespectBudgetsReplacementAndLockedFiles'; expected='diskBytes <= budget.totalCeilingBytes' }
)
try {
    foreach ($mutation in $mutations) {
        $path = Join-Path $source $mutation.file
        $original = [IO.File]::ReadAllText($path)
        if (-not $original.Contains($mutation.old)) { throw "Mutation target missing: $($mutation.name)" }
        [IO.File]::WriteAllText($path, $original.Replace($mutation.old, $mutation.replacement), [Text.UTF8Encoding]::new($false))
        $log = Join-Path $run ($mutation.name + '.log')
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_tests.ps1') -SourceDirectory $source -Test $mutation.test *> $log
        $code = $LASTEXITCODE
        [IO.File]::WriteAllText($path, $original, [Text.UTF8Encoding]::new($false))
        $output = [IO.File]::ReadAllText($log)
        if ($code -eq 0 -or $output.Contains('Native test compilation failed') -or
            -not $output.Contains('[  FAILED  ]') -or -not $output.Contains($mutation.expected)) {
            Write-Host $output
            throw "Mutation was not rejected by the expected behavioral assertion: $($mutation.name)"
        }
        Write-Host "PASS: $($mutation.name) rejected by its behavioral assertion."
    }
} finally {
    $resolved = [IO.Path]::GetFullPath($run)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notmatch '^Aperture-backend-mutations-[a-f0-9]{32}$') { throw 'Unsafe backend mutation cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
