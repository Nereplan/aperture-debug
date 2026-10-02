#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"

namespace Aperture::Test {

TEST_CASE(UpdaterNetworkBounds, HttpsEnforcement) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "http://insecure.example.com/pkg.zip"; // HTTP is forbidden
    manifest.fileSizeBytes = 1000000;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    UpdateCheckResult res = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::DisallowedRedirect);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterNetworkBounds, DisallowedHostAllowlist) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://untrusted-third-party.com/pkg.zip"; // Host not in allowlist
    manifest.fileSizeBytes = 1000000;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowedHosts = { "updates.example.com", "cdn.example.com" };
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    UpdateCheckResult res = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::DisallowedRedirect);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterNetworkBounds, PackageSizeCeiling) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/huge_pkg.zip";
    manifest.fileSizeBytes = 300 * 1024 * 1024; // 300 MiB (exceeds max 150 MiB)
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.maxPackageSizeBytes = 150 * 1024 * 1024;
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    UpdateCheckResult res = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::ResponseTooLarge);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterNetworkBounds, MockHttpTransportEndToEndCheck) {
    ScopedTempDir tempDir;

    // 1. Generate key pair and valid signed manifest
    std::vector<uint8_t> pubBlob, privBlob;
    TEST_ASSERT(GenerateEcdsaP256KeyPair(pubBlob, privBlob));

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.2.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-15T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-2.2.0.0.zip";
    manifest.fileSizeBytes = 15000000;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    manifest.signerThumbprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    std::string canonicalData = manifest.ComputeCanonicalString();
    std::string sigHex;
    TEST_ASSERT(SignEcdsaP256(canonicalData, privBlob, sigHex));
    manifest.signature = sigHex;

    std::string manifestJson = manifest.SerializeJson();

    // 2. Setup mock transport
    MockHttpTransport mockTransport;
    HttpResponse manifestResp;
    manifestResp.success = true;
    manifestResp.statusCode = 200;
    manifestResp.body = manifestJson;
    manifestResp.finalUrl = L"https://updates.example.com/release.json";
    mockTransport.SetResponse(L"https://updates.example.com/release.json", manifestResp);

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowedHosts = { "updates.example.com" };

    static const char hexChars[] = "0123456789abcdef";
    std::string pubHex;
    for (uint8_t b : pubBlob) {
        pubHex.push_back(hexChars[(b >> 4) & 0x0F]);
        pubHex.push_back(hexChars[b & 0x0F]);
    }
    config.overridePinnedPublicKeyHex = pubHex;
    config.overridePinnedSignerThumbprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    // 3. Perform check with mock transport
    UpdateCheckResult checkRes = CheckForUpdates(config, &mockTransport, tempDir.GetPath());
    TEST_ASSERT_EQ(checkRes.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT(checkRes.updateAvailable);
    TEST_ASSERT_EQ(checkRes.manifest.version, "2.2.0.0");
}

TEST_CASE(UpdaterNetworkBounds, SimulatedNetworkTimeout) {
    ScopedTempDir tempDir;

    MockHttpTransport mockTransport;
    HttpResponse timeoutResp;
    timeoutResp.success = false;
    timeoutResp.statusCode = 0;
    timeoutResp.status = HttpTransportStatus::Timeout;
    timeoutResp.errorMessage = L"Connection timed out";
    mockTransport.SetResponse(L"https://updates.example.com/release.json", timeoutResp);

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.overridePinnedPublicKeyHex = "454353312000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    config.overridePinnedSignerThumbprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    UpdateCheckResult checkRes = CheckForUpdates(config, &mockTransport, tempDir.GetPath());
    TEST_ASSERT_EQ(checkRes.status, UpdaterStatusCode::Timeout);
    TEST_ASSERT(!checkRes.updateAvailable);
}

TEST_CASE(UpdaterNetworkBounds, PackageStagingUsesStreamingTransportAndFileHash) {
    ScopedTempDir root;
    struct StreamingFixture : IHttpTransport {
        int bufferedCalls = 0, streamedCalls = 0;
        HttpResponse Get(const std::wstring&, const HttpRequestOptions&) override { ++bufferedCalls; return {}; }
        HttpResponse GetToFile(const std::wstring& url, const std::wstring& path, const HttpRequestOptions& options) override {
            ++streamedCalls;
            TEST_ASSERT(options.allowedHosts == std::vector<std::wstring>{L"updates.example.com"});
            HttpResponse response; response.finalUrl = url; response.statusCode = 200;
            response.success = CopyFileW(GetExePath().c_str(), path.c_str(), TRUE) != FALSE;
            return response;
        }
    } transport;
    ReleaseManifest manifest;
    manifest.schemaVersion = 1; manifest.application = "Aperture"; manifest.version = "2.2.0.0"; manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z"; manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture.exe";
    manifest.fileSizeBytes = GetFileSizeInBytes(GetExePath()); manifest.sha256 = ComputeFileSha256Hex(GetExePath());
    UpdaterConfig config; config.allowUnsignedForTesting = true; config.allowedHosts = {"updates.example.com"};
    std::tm now{}; now.tm_year = 2026 - 1900; now.tm_mon = 7; now.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&now));
    const auto staged = DownloadAndStageUpdate(manifest, config, &transport, nullptr, root.GetPath());
    struct StageCleanup {
        std::wstring dir;
        ~StageCleanup() {
            if (dir.empty()) return;
            DeleteFileW((dir + L"\\Aperture.exe").c_str()); RemoveDirectoryW(dir.c_str());
            RemoveDirectoryW(dir.substr(0, dir.find_last_of(L'\\')).c_str());
        }
    } cleanup{staged.stagingDirectory};
    TEST_ASSERT_EQ(staged.status, UpdaterStatusCode::Success);
    TEST_ASSERT_EQ(transport.bufferedCalls, 0); TEST_ASSERT_EQ(transport.streamedCalls, 1);
    TEST_ASSERT_EQ(ComputeFileSha256Hex(staged.stagedFiles.front()), manifest.sha256);
    config.allowSameVersion = true; manifest.downloadUrl = "http://insecure.example.com/package";
    TEST_ASSERT_EQ(DownloadAndStageUpdate(manifest, config, &transport, nullptr, root.GetPath()).status, UpdaterStatusCode::DisallowedRedirect);
    TEST_ASSERT_EQ(transport.streamedCalls, 1);
}

} // namespace Aperture::Test
