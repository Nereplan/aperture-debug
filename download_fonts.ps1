Remove-Item -Path ".\fonts\*" -Force -Recurse -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path ".\fonts" -Force | Out-Null

$f1 = "https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf"
$f2 = "https://raw.githubusercontent.com/google/fonts/main/ofl/spacegrotesk/SpaceGrotesk%5Bwght%5D.ttf"
$f3 = "https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf"

Write-Host "Downloading Inter.ttf..."
Invoke-WebRequest -Uri $f1 -OutFile ".\fonts\Inter.ttf" -UseBasicParsing
Write-Host "Inter size:" (Get-Item ".\fonts\Inter.ttf").Length

Write-Host "Downloading SpaceGrotesk.ttf..."
Invoke-WebRequest -Uri $f2 -OutFile ".\fonts\SpaceGrotesk.ttf" -UseBasicParsing
Write-Host "SpaceGrotesk size:" (Get-Item ".\fonts\SpaceGrotesk.ttf").Length

Write-Host "Downloading JetBrainsMono.ttf..."
Invoke-WebRequest -Uri $f3 -OutFile ".\fonts\JetBrainsMono.ttf" -UseBasicParsing
Write-Host "JetBrainsMono size:" (Get-Item ".\fonts\JetBrainsMono.ttf").Length

# Validate downloaded fonts against authoritative dependencies.json
$depManifestPath = Join-Path $PSScriptRoot "scripts\dependencies.json"
if (Test-Path -LiteralPath $depManifestPath) {
    $depManifest = Get-Content -LiteralPath $depManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($vendored in @($depManifest.vendoredFiles)) {
        if ($vendored.path -match '^fonts/(.+\.ttf)$') {
            $localPath = Join-Path $PSScriptRoot ($vendored.path -replace '/', '\')
            if (Test-Path -LiteralPath $localPath) {
                $hash = (Get-FileHash -LiteralPath $localPath -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($hash -ne $vendored.sha256.ToLowerInvariant()) {
                    throw "Downloaded font $($vendored.path) hash mismatch! Expected $($vendored.sha256), got $hash"
                }
            }
        }
    }
    Write-Host "All downloaded font hashes match scripts/dependencies.json." -ForegroundColor Green
}

Write-Host "Font downloads complete!"
