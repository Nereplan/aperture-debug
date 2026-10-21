#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "TestCryptoSupport.h"
#include "../../src/Updater.h"
#include <limits>
#include <vector>
#include <string>

namespace Aperture::Test {
static void step_UpdaterCryptoSignature_ComputeSha256() {
    // Known test vectors for SHA-256
    std::string empty = "";
    std::string emptyHash = ComputeSha256Hex(empty.data(), empty.size());
    TEST_ASSERT_EQ(emptyHash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    std::string abc = "abc";
    std::string abcHash = ComputeSha256Hex(abc.data(), abc.size());
    TEST_ASSERT_EQ(abcHash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    TEST_ASSERT(ComputeSha256Hex(nullptr, 1).empty());
    TEST_ASSERT(ComputeSha256Hex(abc.data(), static_cast<size_t>(std::numeric_limits<ULONG>::max()) + 1).empty());
}
static void step_UpdaterCryptoSignature_ComputeFileSha256() {
    ScopedTempDir tempDir;
    std::wstring filePath = tempDir.SubPath(L"test_file.bin");
    std::string content = "Aperture deterministic update package test payload 2026";
    tempDir.WriteFile(L"test_file.bin", content);

    std::string memoryHash = ComputeSha256Hex(content.data(), content.size());
    std::string fileHash = ComputeFileSha256Hex(filePath);

    TEST_ASSERT(!fileHash.empty());
    TEST_ASSERT_EQ(fileHash, memoryHash);
}
static void step_UpdaterCryptoSignature_EcdsaP256SignAndVerify() {
    // 1. Generate ephemeral ECDSA P-256 key pair
    std::vector<uint8_t> pubKeyBlob, privKeyBlob;
    TEST_ASSERT(GenerateEcdsaP256KeyPair(pubKeyBlob, privKeyBlob));
    TEST_ASSERT(!pubKeyBlob.empty());
    TEST_ASSERT(!privKeyBlob.empty());

    // 2. Construct canonical release manifest
    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-31T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-2.1.0.0.zip";
    manifest.fileSizeBytes = 25000000;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    manifest.signerThumbprint = "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0";

    std::string canonicalData = manifest.ComputeCanonicalString();

    // 3. Sign canonical data with private key
    std::string signatureHex;
    TEST_ASSERT(SignEcdsaP256(canonicalData, privKeyBlob, signatureHex));
    TEST_ASSERT(!signatureHex.empty());
    manifest.signature = signatureHex;

    // 4. Verify signature against public key (Success expected)
    TEST_ASSERT(VerifyEcdsaP256Signature(canonicalData, signatureHex, pubKeyBlob));

    // 5. Verify that tampering with canonical data causes verification failure
    ReleaseManifest tamperedManifest = manifest;
    tamperedManifest.version = "2.1.1.0"; // Tamper version
    std::string tamperedCanonical = tamperedManifest.ComputeCanonicalString();
    TEST_ASSERT(!VerifyEcdsaP256Signature(tamperedCanonical, signatureHex, pubKeyBlob));

    // Tamper download URL
    tamperedManifest = manifest;
    tamperedManifest.downloadUrl = "https://malicious.example.com/fake.zip";
    tamperedCanonical = tamperedManifest.ComputeCanonicalString();
    TEST_ASSERT(!VerifyEcdsaP256Signature(tamperedCanonical, signatureHex, pubKeyBlob));

    // Tamper hash
    tamperedManifest = manifest;
    tamperedManifest.sha256 = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    tamperedCanonical = tamperedManifest.ComputeCanonicalString();
    TEST_ASSERT(!VerifyEcdsaP256Signature(tamperedCanonical, signatureHex, pubKeyBlob));

    // 6. Verify that verification fails with a different public key
    std::vector<uint8_t> otherPub, otherPriv;
    TEST_ASSERT(GenerateEcdsaP256KeyPair(otherPub, otherPriv));
    TEST_ASSERT(!VerifyEcdsaP256Signature(canonicalData, signatureHex, otherPub));

    // 7. Verify that corrupted signature fails
    std::string corruptedSig = signatureHex;
    corruptedSig[0] = (corruptedSig[0] == 'a') ? 'b' : 'a';
    TEST_ASSERT(!VerifyEcdsaP256Signature(canonicalData, corruptedSig, pubKeyBlob));
}




static void CreateMockPeFile(const std::wstring& path, WORD machine, WORD magic, WORD subsystem) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    IMAGE_DOS_HEADER dosHdr = {};
    dosHdr.e_magic = IMAGE_DOS_SIGNATURE; // 'MZ'
    dosHdr.e_lfanew = sizeof(IMAGE_DOS_HEADER) + 64; // Place NT headers after DOS stub

    DWORD written = 0;
    WriteFile(hFile, &dosHdr, sizeof(dosHdr), &written, NULL);

    // Padding for DOS stub
    std::vector<uint8_t> stub(64, 0);
    WriteFile(hFile, stub.data(), (DWORD)stub.size(), &written, NULL);

    DWORD peSignature = IMAGE_NT_SIGNATURE; // 'PE\0\0'
    WriteFile(hFile, &peSignature, sizeof(peSignature), &written, NULL);

    IMAGE_FILE_HEADER fileHdr = {};
    fileHdr.Machine = machine;
    fileHdr.NumberOfSections = 1;
    fileHdr.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    fileHdr.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    WriteFile(hFile, &fileHdr, sizeof(fileHdr), &written, NULL);

    IMAGE_OPTIONAL_HEADER64 optHdr = {};
    optHdr.Magic = magic;
    optHdr.Subsystem = subsystem;
    optHdr.SizeOfCode = 1024;
    optHdr.ImageBase = 0x0000000140000000ULL;
    optHdr.SectionAlignment = 0x1000;
    optHdr.FileAlignment = 0x200;
    WriteFile(hFile, &optHdr, sizeof(optHdr), &written, NULL);

    CloseHandle(hFile);
}
static void step_UpdaterAuthenticodePe_ValidPeArchitectureX64() {
    ScopedTempDir tempDir;
    std::wstring pePath = tempDir.SubPath(L"ValidAperture.exe");

    CreateMockPeFile(pePath, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string err;
    TEST_ASSERT(VerifyPeArchitectureX64(pePath, err));
    TEST_ASSERT(err.empty());
}
static void step_UpdaterAuthenticodePe_NonPeFileRejected() {
    ScopedTempDir tempDir;
    std::wstring txtPath = tempDir.SubPath(L"NotAPe.exe");
    tempDir.WriteFile(L"NotAPe.exe", "This is plain text and definitely not a portable executable.");

    std::string err;
    TEST_ASSERT(!VerifyPeArchitectureX64(txtPath, err));
    TEST_ASSERT(!err.empty());
}
static void step_UpdaterAuthenticodePe_IncompatiblePeMachineArchitectureRejected() {
    ScopedTempDir tempDir;

    // x86 (32-bit i386) Machine type: 0x014c
    std::wstring x86Path = tempDir.SubPath(L"Aperture_x86.exe");
    CreateMockPeFile(x86Path, 0x014c, IMAGE_NT_OPTIONAL_HDR32_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string err;
    TEST_ASSERT(!VerifyPeArchitectureX64(x86Path, err));
    TEST_ASSERT(!err.empty());

    // ARM64 Machine type: 0xaa64
    std::wstring arm64Path = tempDir.SubPath(L"Aperture_arm64.exe");
    CreateMockPeFile(arm64Path, 0xaa64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    TEST_ASSERT(!VerifyPeArchitectureX64(arm64Path, err));
    TEST_ASSERT(!err.empty());
}
static void step_UpdaterAuthenticodePe_AuthenticodeThumbprintMismatch() {
    ScopedTempDir tempDir;
    std::wstring pePath = tempDir.SubPath(L"UnsignedAperture.exe");
    CreateMockPeFile(pePath, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_SUBSYSTEM_WINDOWS_GUI);

    std::string actualThumbprint;
    std::string err;
    // An unsigned binary should fail validation when a signed thumbprint is expected
    bool ok = VerifyAuthenticodeAndPin(pePath, "a1b2c3d4e5f60718293a4b5c6d7e8f90123456789abcdef0123456789abcdef0", actualThumbprint, err);
    TEST_ASSERT(!ok);
    TEST_ASSERT(!err.empty());
}



TEST_CASE(Updater, CryptoAuthenticodeAndPeContract) {
    step_UpdaterCryptoSignature_ComputeSha256();
    step_UpdaterCryptoSignature_ComputeFileSha256();
    step_UpdaterCryptoSignature_EcdsaP256SignAndVerify();
    step_UpdaterAuthenticodePe_ValidPeArchitectureX64();
    step_UpdaterAuthenticodePe_NonPeFileRejected();
    step_UpdaterAuthenticodePe_IncompatiblePeMachineArchitectureRejected();
    step_UpdaterAuthenticodePe_AuthenticodeThumbprintMismatch();
}

} // namespace Aperture::Test
