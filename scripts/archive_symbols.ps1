<#
.SYNOPSIS
    Creates and verifies content-addressed release symbol archives for Aperture binaries.

.DESCRIPTION
    Extracts PE debug directory metadata (CodeView RSDS GUID, age, PDB basename) and
    validates that the PE debug entry contains strictly a basename without local path
    leakage. Parses the corresponding MSF 7.0 PDB file (matching GUID and Age) with a
    pure-PowerShell MSF parser. Computes exact SHA-256 hashes of the final binary and
    PDB, produces a compressed content-addressed symbol archive
    (<binarySha256>.symbols.zip) and a canonical JSON manifest
    (<binarySha256>.symbols.json) containing binary metadata, PDB metadata, CodeView
    GUID/age, product version, and toolchain provenance, then re-verifies the archive
    it just wrote (CreateAndVerify mode).

    Verification mode (-Verify) reopens an existing archive, re-hashes the binary,
    re-reads its CodeView entry, and validates archive naming, manifest hashes, PDB
    payload integrity, and GUID/age provenance. Any mismatch fails with a non-zero
    exit code.

.PARAMETER BinaryPath
    Path to the release binary (executable) whose symbols are being archived.

.PARAMETER PdbPath
    Path to the PDB matching the binary.

.PARAMETER OutputDirectory
    Destination directory for <binarySha256>.symbols.zip and .symbols.json.

.PARAMETER Configuration
    Build configuration label recorded in the manifest (default: Release).

.PARAMETER Verify
    Verification mode: validate an existing archive against the binary.

.PARAMETER ArchiveFile
    The .symbols.zip archive to verify (with -Verify).

.PARAMETER Json
    Emit the machine-readable JSON report on stdout (create mode).

.EXAMPLE
    .\archive_symbols.ps1 -BinaryPath release\Aperture.exe -PdbPath scratch\Aperture.pdb -OutputDirectory dist\symbols -Json
#>
param(
    [string]$BinaryPath = "",
    [string]$PdbPath = "",
    [string]$OutputDirectory = "",
    [string]$Configuration = "Release",
    [switch]$Verify,
    [string]$ArchiveFile = "",
    [switch]$Json
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

function Write-Fail([string]$message) {
    [Console]::Error.WriteLine("[archive_symbols] ERROR: " + $message)
    exit 2
}

function Get-Sha256File([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($path)
        try {
            $hash = $sha.ComputeHash($stream)
            return ([System.BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
        } finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}

function Get-Sha256Bytes([byte[]]$data) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($data)
        return ([System.BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Format-Guid([byte[]]$guidBytes) {
    $d1 = [BitConverter]::ToUInt32($guidBytes, 0)
    $d2 = [BitConverter]::ToUInt16($guidBytes, 4)
    $d3 = [BitConverter]::ToUInt16($guidBytes, 6)
    $d4a = ($guidBytes[8..9] | ForEach-Object { $_.ToString("x2") }) -join ""
    $d4b = ($guidBytes[10..15] | ForEach-Object { $_.ToString("x2") }) -join ""
    return ("{0:x8}-{1:x4}-{2:x4}-{3}-{4}" -f $d1, $d2, $d3, $d4a, $d4b)
}

function Read-Ui4([byte[]]$data, [int]$offset) { return [BitConverter]::ToUInt32($data, $offset) }
function Read-Ui2([byte[]]$data, [int]$offset) { return [BitConverter]::ToUInt16($data, $offset) }

# Parses the PE x64 debug directory and returns the CodeView RSDS record.
function Read-PeCodeView([byte[]]$data) {
    if ($data.Length -lt 64 -or $data[0] -ne 0x4D -or $data[1] -ne 0x5A) { Write-Fail "Binary is not a PE image (missing MZ signature)." }
    $peOff = Read-Ui4 $data 0x3C
    if (($peOff + 24) -gt $data.Length) { Write-Fail "PE header offset out of range." }
    if ($data[$peOff] -ne 0x50 -or $data[$peOff + 1] -ne 0x45) { Write-Fail "Missing PE signature." }
    $machine = Read-Ui2 $data ($peOff + 4)
    if ($machine -ne 0x8664) { Write-Fail ("PE is not x64: 0x{0:x4}" -f $machine) }
    $numSections = Read-Ui2 $data ($peOff + 6)
    $sizeOfOpt = Read-Ui2 $data ($peOff + 20)
    $optOff = $peOff + 24
    if ((Read-Ui2 $data $optOff) -ne 0x20B) { Write-Fail "PE is not PE32+." }
    $numRvaSizes = Read-Ui4 $data ($optOff + 108)
    if ($numRvaSizes -le 6) { Write-Fail "No debug directory present in PE." }
    $dbgRva = Read-Ui4 $data ($optOff + 112 + 6 * 8)
    $dbgSize = Read-Ui4 $data ($optOff + 112 + 6 * 8 + 4)
    if ($dbgRva -eq 0 -or $dbgSize -eq 0) { Write-Fail "Empty debug directory." }

    $secOff = $optOff + $sizeOfOpt
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $secOff + $i * 40
        $vSize = Read-Ui4 $data ($s + 8)
        $vAddr = Read-Ui4 $data ($s + 12)
        $rawSize = Read-Ui4 $data ($s + 16)
        $rawPtr = Read-Ui4 $data ($s + 20)
        $secLen = [Math]::Max($vSize, $rawSize)
        if ($vAddr -le $dbgRva -and $dbgRva -lt ($vAddr + $secLen)) {
            $dbgOff = $dbgRva - $vAddr + $rawPtr
            if (($dbgOff + $dbgSize) -gt $data.Length) { Write-Fail "Debug directory out of file bounds." }
            $entryCount = [Math]::Floor($dbgSize / 28)
            for ($e = 0; $e -lt $entryCount; $e++) {
                $eo = $dbgOff + $e * 28
                $dbgType = Read-Ui4 $data ($eo + 12)
                $szData = Read-Ui4 $data ($eo + 16)
                $ptrRaw = Read-Ui4 $data ($eo + 24)
                if ($dbgType -ne 2) { continue }  # IMAGE_DEBUG_TYPE_CODEVIEW
                if (($ptrRaw + $szData) -gt $data.Length -or $szData -lt 24) { Write-Fail "Malformed CodeView debug entry." }
                $sig = [System.Text.Encoding]::ASCII.GetString($data, $ptrRaw, 4)
                if ($sig -ne "RSDS") { continue }
                $guid = New-Object byte[] 16
                [Array]::Copy($data, $ptrRaw + 4, $guid, 0, 16)
                $age = Read-Ui4 $data ($ptrRaw + 20)
                $end = $ptrRaw + 24
                while ($end -lt $data.Length -and $data[$end] -ne 0) { $end++ }
                $pdbPath = [System.Text.Encoding]::UTF8.GetString($data, $ptrRaw + 24, $end - ($ptrRaw + 24))
                $pdbName = [System.IO.Path]::GetFileName($pdbPath)
                $isBasename = ($pdbPath -eq $pdbName -and $pdbPath.IndexOf('\') -lt 0 -and $pdbPath.IndexOf('/') -lt 0 -and $pdbPath.IndexOf(':') -lt 0)
                return @{
                    signature = "RSDS"; guid = (Format-Guid $guid); age = $age
                    pdbPath = $pdbPath; pdbFileName = $pdbName; isBasenameOnly = [bool]$isBasename
                }
            }
            Write-Fail "No RSDS CodeView entry found in debug directory."
        }
    }
    Write-Fail "Could not resolve debug directory RVA against section headers."
}

# Parses an MSF 7.0 PDB and returns Stream 1 (PDB Info) metadata.
function Read-PdbMsf7([byte[]]$data) {
    $sig = [System.Text.Encoding]::ASCII.GetBytes("Microsoft C/C++ MSF 7.00`r`n\x1aDS")
    # The canonical signature contains a literal 0x1A byte and 'DS'; build it exactly:
    $sigBytes = New-Object byte[] 32
    $sigText = "Microsoft C/C++ MSF 7.00"
    $sigAscii = [System.Text.Encoding]::ASCII.GetBytes($sigText)
    [Array]::Copy($sigAscii, 0, $sigBytes, 0, $sigAscii.Length)
    $sigBytes[24] = 0x0D; $sigBytes[25] = 0x0A; $sigBytes[26] = 0x1A; $sigBytes[27] = 0x44; $sigBytes[28] = 0x53
    $sigBytes[29] = 0x00; $sigBytes[30] = 0x00; $sigBytes[31] = 0x00
    if ($data.Length -lt 32) { Write-Fail "PDB too small to be an MSF 7.0 file." }
    for ($i = 0; $i -lt 32; $i++) { if ($data[$i] -ne $sigBytes[$i]) { Write-Fail "PDB is not an MSF 7.0 file (bad signature)." } }

    $blockSize = Read-Ui4 $data 0x20
    $numBlocks = Read-Ui4 $data 0x28
    $numDirBytes = Read-Ui4 $data 0x2C
    $blockMapAddr = Read-Ui4 $data 0x34
    if ($blockSize -le 0 -or ($blockSize -band ($blockSize - 1)) -ne 0) { Write-Fail "Invalid MSF block size." }
    $numDirBlocks = [int][Math]::Ceiling($numDirBytes / $blockSize)

    $dirBytes = New-Object byte[] $numDirBytes
    for ($i = 0; $i -lt $numDirBlocks; $i++) {
        $dirBlock = Read-Ui4 $data ($blockMapAddr * $blockSize + $i * 4)
        $src = $dirBlock * $blockSize
        if (($src + $blockSize) -gt $data.Length) { Write-Fail "MSF directory block out of range." }
        $copyLen = [Math]::Min($blockSize, $numDirBytes - $i * $blockSize)
        [Array]::Copy($data, $src, $dirBytes, $i * $blockSize, $copyLen)
    }

    $numStreams = Read-Ui4 $dirBytes 0
    if ($numStreams -lt 2) { Write-Fail "MSF directory reports fewer than 2 streams." }
    $streamSizes = New-Object uint32[] $numStreams
    for ($i = 0; $i -lt $numStreams; $i++) { $streamSizes[$i] = Read-Ui4 $dirBytes (4 + $i * 4) }

    $cursor = 4 + $numStreams * 4
    # Skip stream 0 block list
    $s0Size = $streamSizes[0]
    if ($s0Size -ne 0 -and $s0Size -ne 0xFFFFFFFF) {
        $s0Blocks = [int][Math]::Ceiling($s0Size / $blockSize)
        $cursor += $s0Blocks * 4
    }
    $s1Size = $streamSizes[1]
    if ($s1Size -lt 28) { Write-Fail "PDB info stream (1) too small." }
    $s1Blocks = [int][Math]::Ceiling($s1Size / $blockSize)
    $s1 = New-Object byte[] $s1Size
    $written = 0
    for ($i = 0; $i -lt $s1Blocks; $i++) {
        $b = Read-Ui4 $dirBytes ($cursor + $i * 4)
        $src = $b * $blockSize
        if (($src + $blockSize) -gt $data.Length) { Write-Fail "PDB info stream block out of range." }
        $copyLen = [Math]::Min($blockSize, $s1Size - $written)
        [Array]::Copy($data, $src, $s1, $written, $copyLen)
        $written += $copyLen
    }

    $version = Read-Ui4 $s1 0
    $sigStamp = Read-Ui4 $s1 4
    $age = Read-Ui4 $s1 8
    $guid = New-Object byte[] 16
    [Array]::Copy($s1, 12, $guid, 0, 16)
    return @{ version = $version; signature = $sigStamp; age = $age; guid = (Format-Guid $guid) }
}

function Get-FileVersionText([string]$path) {
    try {
        $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        if ($vi.FileVersion) { return $vi.FileVersion }
    } catch { }
    return "unknown"
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
if ($Verify) {
    if ([string]::IsNullOrWhiteSpace($ArchiveFile)) { Write-Fail "-Verify requires -ArchiveFile." }
    if ([string]::IsNullOrWhiteSpace($BinaryPath)) { Write-Fail "-Verify requires -BinaryPath." }
    if (-not (Test-Path -LiteralPath $ArchiveFile -PathType Leaf)) { Write-Fail ("Archive not found: " + $ArchiveFile) }
    if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) { Write-Fail ("Binary not found: " + $BinaryPath) }

    $binaryBytes = [System.IO.File]::ReadAllBytes($BinaryPath)
    $binarySha = Get-Sha256Bytes $binaryBytes
    $codeview = Read-PeCodeView $binaryBytes
    if (-not $codeview.isBasenameOnly) { Write-Fail ("CodeView PDB path leakage detected: '" + $codeview.pdbPath + "'.") }

    $expectedName = $binarySha + ".symbols.zip"
    $archiveName = [System.IO.Path]::GetFileName($ArchiveFile)
    if ($archiveName -ne $expectedName) {
        Write-Fail ("Content address mismatch: archive is '" + $archiveName + "' but binary hashes to '" + $expectedName + "'.")
    }

    $zip = [System.IO.Compression.ZipFile]::OpenRead($ArchiveFile)
    try {
        $manifestEntry = $zip.Entries | Where-Object { $_.FullName -eq "manifest.json" }
        $pdbEntry = $zip.Entries | Where-Object { $_.FullName -eq $codeview.pdbFileName }
        if (-not $manifestEntry) { Write-Fail "Archive is missing manifest.json." }
        if (-not $pdbEntry) { Write-Fail ("Archive is missing PDB payload '" + $codeview.pdbFileName + "'.") }

        $manifestReader = New-Object System.IO.StreamReader($manifestEntry.Open())
        try { $manifestText = $manifestReader.ReadToEnd() } finally { $manifestReader.Dispose() }
        $manifest = $manifestText | ConvertFrom-Json

        if ($manifest.binary.sha256 -ne $binarySha) { Write-Fail "Manifest binary hash does not match the binary." }
        if ($manifest.codeview.guid.ToLowerInvariant() -ne $codeview.guid.ToLowerInvariant()) { Write-Fail "Manifest CodeView GUID does not match the binary." }
        if ([int]$manifest.codeview.age -ne [int]$codeview.age) { Write-Fail "Manifest CodeView age does not match the binary." }
        if ($manifest.codeview.pdbPath -ne $codeview.pdbPath) { Write-Fail "Manifest CodeView PDB path does not match the binary." }

        $pdbStream = $pdbEntry.Open()
        $pdbMem = New-Object System.IO.MemoryStream
        try {
            $pdbStream.CopyTo($pdbMem)
            $pdbBytes = $pdbMem.ToArray()
        } finally { $pdbMem.Dispose(); $pdbStream.Dispose() }
        $pdbSha = Get-Sha256Bytes $pdbBytes
        if ($pdbSha -ne $manifest.pdb.sha256) { Write-Fail "Archived PDB payload hash does not match the manifest (tamper detected)." }

        $pdbInfo = Read-PdbMsf7 $pdbBytes
        if ($pdbInfo.guid.ToLowerInvariant() -ne $codeview.guid.ToLowerInvariant()) { Write-Fail "Archived PDB GUID does not match the binary CodeView GUID." }
        if ([int]$pdbInfo.age -ne [int]$codeview.age) { Write-Fail "Archived PDB age does not match the binary CodeView age." }

        if (-not $Json) {
            [Console]::Error.WriteLine("[archive_symbols] Verified archive: " + $ArchiveFile)
        } else {
            $report = [ordered]@{
                status = "success"; mode = "Verify"; verified = $true
                archiveFile = (Resolve-Path -LiteralPath $ArchiveFile).Path
                binarySha256 = $binarySha
                codeview = [ordered]@{ guid = $codeview.guid; age = $codeview.age; pdbPath = $codeview.pdbPath }
                pdbSha256 = $pdbSha
            }
            Write-Output ($report | ConvertTo-Json -Depth 6)
        }
        exit 0
    } finally { $zip.Dispose() }
}

if ([string]::IsNullOrWhiteSpace($BinaryPath)) { Write-Fail "-BinaryPath is required." }
if ([string]::IsNullOrWhiteSpace($PdbPath)) { Write-Fail "-PdbPath is required." }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { Write-Fail "-OutputDirectory is required." }
if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) { Write-Fail ("Binary not found: " + $BinaryPath) }
if (-not (Test-Path -LiteralPath $PdbPath -PathType Leaf)) { Write-Fail ("PDB not found: " + $PdbPath) }

$binaryBytes = [System.IO.File]::ReadAllBytes($BinaryPath)
$binarySha = Get-Sha256Bytes $binaryBytes
$codeview = Read-PeCodeView $binaryBytes
if (-not $codeview.isBasenameOnly) {
    Write-Fail ("CodeView PDB path leakage detected: '" + $codeview.pdbPath + "'. Build with /PDBALTPATH so the PE embeds only the PDB basename.")
}

$pdbBytes = [System.IO.File]::ReadAllBytes($PdbPath)
$pdbSha = Get-Sha256Bytes $pdbBytes
$pdbInfo = Read-PdbMsf7 $pdbBytes
if ($pdbInfo.guid.ToLowerInvariant() -ne $codeview.guid.ToLowerInvariant()) {
    Write-Fail ("PDB GUID " + $pdbInfo.guid + " does not match binary CodeView GUID " + $codeview.guid + ".")
}
if ([int]$pdbInfo.age -ne [int]$codeview.age) {
    Write-Fail ("PDB age " + $pdbInfo.age + " does not match binary CodeView age " + $codeview.age + ".")
}
if ($codeview.pdbFileName -ne [System.IO.Path]::GetFileName($PdbPath)) {
    Write-Fail ("PDB file name '" + [System.IO.Path]::GetFileName($PdbPath) + "' does not match the binary's CodeView entry '" + $codeview.pdbFileName + "'.")
}

if (-not (Test-Path -LiteralPath $OutputDirectory)) { New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null }
$staging = Join-Path $OutputDirectory ("symbols-stage-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $staging -Force | Out-Null
try {
    $manifest = [ordered]@{
        application = "Aperture"
        configuration = $Configuration
        productVersion = Get-FileVersionText $BinaryPath
        toolchain = "MSVC x64"
        binary = [ordered]@{
            fileName = [System.IO.Path]::GetFileName($BinaryPath)
            sha256 = $binarySha
            lengthBytes = $binaryBytes.LongLength
        }
        pdb = [ordered]@{
            fileName = $codeview.pdbFileName
            sha256 = $pdbSha
            lengthBytes = [int64]$pdbBytes.LongLength
            version = $pdbInfo.version
            signature = $pdbInfo.signature
            age = $pdbInfo.age
            guid = $pdbInfo.guid
        }
        codeview = [ordered]@{
            signature = $codeview.signature
            guid = $codeview.guid
            age = $codeview.age
            pdbPath = $codeview.pdbPath
            isBasenameOnly = $codeview.isBasenameOnly
        }
    }
    $manifestText = $manifest | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText((Join-Path $staging "manifest.json"), $manifestText)
    [System.IO.File]::WriteAllBytes((Join-Path $staging $codeview.pdbFileName), $pdbBytes)

    $archiveTmp = Join-Path $OutputDirectory ("symbols-tmp-" + [System.Guid]::NewGuid().ToString("N") + ".zip")
    [System.IO.Compression.ZipFile]::CreateFromDirectory($staging, $archiveTmp)
    $archiveFile = Join-Path $OutputDirectory ($binarySha + ".symbols.zip")
    if (Test-Path -LiteralPath $archiveFile) { Remove-Item -LiteralPath $archiveFile -Force }
    Move-Item -LiteralPath $archiveTmp -Destination $archiveFile
    $manifestFile = Join-Path $OutputDirectory ($binarySha + ".symbols.json")
    [System.IO.File]::WriteAllText($manifestFile, $manifestText)
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}

# CreateAndVerify: re-open the archive we just wrote and run the full verification.
$verifyArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
    "-Verify", "-ArchiveFile", $archiveFile, "-BinaryPath", $BinaryPath, "-Json")
$verifyOut = & powershell.exe @verifyArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    [Console]::Error.WriteLine("[archive_symbols] Self-verification failed: " + ($verifyOut -join "`n"))
    exit 3
}

if ($Json) {
    $report = [ordered]@{
        status = "success"; mode = "CreateAndVerify"; verified = $true
        archiveFile = (Resolve-Path -LiteralPath $archiveFile).Path
        manifestFile = (Resolve-Path -LiteralPath $manifestFile).Path
        binarySha256 = $binarySha
        pdbSha256 = $pdbSha
        codeview = [ordered]@{ guid = $codeview.guid; age = $codeview.age; pdbPath = $codeview.pdbPath; isBasenameOnly = $codeview.isBasenameOnly }
        verifyReport = ($verifyOut | ConvertFrom-Json)
    }
    Write-Output ($report | ConvertTo-Json -Depth 6)
} else {
    [Console]::Error.WriteLine("[archive_symbols] Created and verified: " + $archiveFile)
}
exit 0
