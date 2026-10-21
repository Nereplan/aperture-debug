#pragma once

#include "../../src/Updater.h"
#include <bcrypt.h>

namespace Aperture::Test {

inline bool GenerateEcdsaP256KeyPair(std::vector<uint8_t>& outPublicBlob, std::vector<uint8_t>& outPrivateBlob) {
    outPublicBlob.clear();
    outPrivateBlob.clear();
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status = BCryptGenerateKeyPair(hAlg, &hKey, 256, 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }
    status = BCryptFinalizeKeyPair(hKey, 0);
    DWORD pubLen = 0, privLen = 0;
    if (BCRYPT_SUCCESS(status)) status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pubLen, 0);
    if (BCRYPT_SUCCESS(status)) {
        outPublicBlob.resize(pubLen);
        status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPUBLIC_BLOB, outPublicBlob.data(), pubLen, &pubLen, 0);
    }
    if (BCRYPT_SUCCESS(status)) status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &privLen, 0);
    if (BCRYPT_SUCCESS(status)) {
        outPrivateBlob.resize(privLen);
        status = BCryptExportKey(hKey, nullptr, BCRYPT_ECCPRIVATE_BLOB, outPrivateBlob.data(), privLen, &privLen, 0);
    }
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!BCRYPT_SUCCESS(status)) { outPublicBlob.clear(); outPrivateBlob.clear(); }
    return BCRYPT_SUCCESS(status);
}

inline bool SignEcdsaP256(const std::string& canonicalData, const std::vector<uint8_t>& privateKeyBlob, std::string& outSignatureHex) {
    outSignatureHex.clear();
    if (privateKeyBlob.empty()) return false;
    const std::string hashHex = ComputeSha256Hex(canonicalData.data(), canonicalData.size());
    if (hashHex.size() != 64) return false;
    const auto hexValue = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    uint8_t hash[32];
    for (size_t i = 0; i < sizeof(hash); ++i) hash[i] = static_cast<uint8_t>((hexValue(hashHex[i * 2]) << 4) | hexValue(hashHex[i * 2 + 1]));
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS status = BCryptImportKeyPair(hAlg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &hKey,
        const_cast<PUCHAR>(privateKeyBlob.data()), static_cast<ULONG>(privateKeyBlob.size()), 0);
    if (!BCRYPT_SUCCESS(status)) { BCryptCloseAlgorithmProvider(hAlg, 0); return false; }
    DWORD sigLen = 0;
    status = BCryptSignHash(hKey, nullptr, hash, sizeof(hash), nullptr, 0, &sigLen, 0);
    std::vector<uint8_t> sig(sigLen);
    if (BCRYPT_SUCCESS(status)) status = BCryptSignHash(hKey, nullptr, hash, sizeof(hash), sig.data(), sigLen, &sigLen, 0);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!BCRYPT_SUCCESS(status)) return false;
    constexpr char digits[] = "0123456789abcdef";
    outSignatureHex.reserve(sigLen * 2);
    for (DWORD i = 0; i < sigLen; ++i) {
        outSignatureHex.push_back(digits[sig[i] >> 4]);
        outSignatureHex.push_back(digits[sig[i] & 0x0f]);
    }
    return true;
}

} // namespace Aperture::Test
