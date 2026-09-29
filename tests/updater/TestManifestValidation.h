#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include "../../src/Json.h"
#include <chrono>

namespace Aperture::Test {

TEST_CASE(UpdaterManifestValidation, UnconfiguredTrustReturnsNotConfiguredWithoutNetwork) {
    ScopedTempDir tempDir;

    UpdaterConfig unconfigured;
    unconfigured.manifestUrl = "";
    unconfigured.overridePinnedPublicKeyHex = "";
    unconfigured.overridePinnedSignerThumbprint = "";

    UpdateCheckResult res = CheckForUpdates(unconfigured, nullptr, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::NotConfigured);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterManifestValidation, MalformedJsonParsing) {
    ReleaseManifest manifest;
    std::string err;
    TEST_ASSERT(!ReleaseManifest::Parse("{ invalid json ", manifest, err));
    TEST_ASSERT(!err.empty());

    TEST_ASSERT(!ReleaseManifest::Parse("[]", manifest, err));
    TEST_ASSERT(!ReleaseManifest::Parse("", manifest, err));
}

TEST_CASE(UpdaterManifestValidation, MissingRequiredFields) {
    ReleaseManifest manifest;
    std::string err;

    // Missing version
    std::string jsonNoVer = R"({
        "schemaVersion": 1,
        "application": "Aperture",
        "downloadUrl": "https://updates.example.com/pkg.zip",
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    })";
    TEST_ASSERT(!ReleaseManifest::Parse(jsonNoVer, manifest, err));

    // Missing downloadUrl
    std::string jsonNoUrl = R"({
        "schemaVersion": 1,
        "application": "Aperture",
        "version": "2.1.0",
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    })";
    TEST_ASSERT(!ReleaseManifest::Parse(jsonNoUrl, manifest, err));

    // Missing sha256
    std::string jsonNoSha = R"({
        "schemaVersion": 1,
        "application": "Aperture",
        "version": "2.1.0",
        "downloadUrl": "https://updates.example.com/pkg.zip"
    })";
    TEST_ASSERT(!ReleaseManifest::Parse(jsonNoSha, manifest, err));
}

TEST_CASE(UpdaterManifestValidation, ExpiredManifestRejected) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-01-01T00:00:00Z";
    manifest.expiryUtc = "2026-02-01T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-2.1.0.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
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
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::ManifestExpired);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterManifestValidation, FuturePublishedTimestampRejected) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "2.1.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-10-01T00:00:00Z";
    manifest.expiryUtc = "2026-11-01T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-2.1.0.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
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
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::ManifestFutureTimestamp);
    TEST_ASSERT(!res.updateAvailable);
}

TEST_CASE(UpdaterManifestValidation, DowngradeAndSameVersionProtection) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 31;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    // Test older version (downgrade rejection) - Current is 2.0.0.0
    manifest.version = "1.9.0.0";
    UpdateCheckResult resDown = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resDown.status, UpdaterStatusCode::ManifestDowngrade);
    TEST_ASSERT(!resDown.updateAvailable);

    // Test same version (up to date report)
    manifest.version = SemanticVersion::Current().ToString();
    UpdateCheckResult resSame = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resSame.status, UpdaterStatusCode::UpToDate);
    TEST_ASSERT(!resSame.updateAvailable);

    // Test newer version (update available)
    manifest.version = "2.1.0.0";
    UpdateCheckResult resNew = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resNew.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT(resNew.updateAvailable);
}

TEST_CASE(UpdaterManifestValidation, MinimumUpdaterVersionRequirement) {
    ScopedTempDir tempDir;

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.version = "3.0.0.0";
    manifest.minimumUpdaterVersion = "4.0.0.0";
    manifest.channel = "stable";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-09-30T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-3.0.0.0.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
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
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::MinimumUpdaterVersionUnsatisfied);
    TEST_ASSERT(!res.updateAvailable);
}

} // namespace Aperture::Test
