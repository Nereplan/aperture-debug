#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(UpdaterCryptoSignature, ComputeSha256) {
    // Known test vectors for SHA-256
    std::string empty = "";
    std::string emptyHash = ComputeSha256Hex(empty.data(), empty.size());
    TEST_ASSERT_EQ(emptyHash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    std::string abc = "abc";
    std::string abcHash = ComputeSha256Hex(abc.data(), abc.size());
    TEST_ASSERT_EQ(abcHash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE(UpdaterCryptoSignature, ComputeFileSha256) {
    ScopedTempDir tempDir;
    std::wstring filePath = tempDir.SubPath(L"test_file.bin");
    std::string content = "Aperture deterministic update package test payload 2026";
    tempDir.WriteFile(L"test_file.bin", content);

    std::string memoryHash = ComputeSha256Hex(content.data(), content.size());
    std::string fileHash = ComputeFileSha256Hex(filePath);

    TEST_ASSERT(!fileHash.empty());
    TEST_ASSERT_EQ(fileHash, memoryHash);
}

TEST_CASE(UpdaterCryptoSignature, EcdsaP256SignAndVerify) {
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

} // namespace Aperture::Test
