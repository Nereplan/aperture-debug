#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/ChannelPolicy.h"
#include "../../src/Updater.h"
#include "../../src/Json.h"
#include <chrono>

namespace Aperture::Test {
static void step_UpdaterManifestValidation_UnconfiguredTrustReturnsNotConfiguredWithoutNetwork() {
    ScopedTempDir tempDir;

    UpdaterConfig unconfigured;
    unconfigured.manifestUrl = "";
    unconfigured.overridePinnedPublicKeyHex = "";
    unconfigured.overridePinnedSignerThumbprint = "";

    UpdateCheckResult res = CheckForUpdates(unconfigured, nullptr, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::NotConfigured);
    TEST_ASSERT(!res.updateAvailable);
}
static void step_UpdaterManifestValidation_MalformedJsonParsing() {
    ReleaseManifest manifest;
    std::string err;
    TEST_ASSERT(!ReleaseManifest::Parse("{ invalid json ", manifest, err));
    TEST_ASSERT(!err.empty());

    TEST_ASSERT(!ReleaseManifest::Parse("[]", manifest, err));
    TEST_ASSERT(!ReleaseManifest::Parse("", manifest, err));
}
static void step_UpdaterManifestValidation_MissingRequiredFields() {
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
static void step_UpdaterManifestValidation_ExpiredManifestRejected() {
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
static void step_UpdaterManifestValidation_FuturePublishedTimestampRejected() {
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
static void step_UpdaterManifestValidation_DowngradeAndSameVersionProtection() {
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
static void step_UpdaterManifestValidation_MinimumUpdaterVersionRequirement() {
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
static void step_ChannelPolicy_DefaultPolicyDefinitionAndChannels() {
    const auto& policy = ChannelPolicy::Instance();

    TEST_ASSERT_EQ(policy.GetDefaultChannel(), "stable");
    TEST_ASSERT(policy.IsValidChannel("stable"));
    TEST_ASSERT(policy.IsValidChannel("beta"));
    TEST_ASSERT(policy.IsValidChannel("nightly"));
    TEST_ASSERT(!policy.IsValidChannel("unknown_channel"));
    TEST_ASSERT(!policy.IsValidChannel(""));

    TEST_ASSERT_EQ(policy.CanonicalizeChannel("STABLE"), "stable");
    TEST_ASSERT_EQ(policy.CanonicalizeChannel("Beta"), "beta");
    TEST_ASSERT_EQ(policy.CanonicalizeChannel("NIGHTLY"), "nightly");
    TEST_ASSERT_EQ(policy.CanonicalizeChannel("invalid"), "stable");

    const auto* stableDef = policy.FindChannel("stable");
    TEST_ASSERT(stableDef != nullptr);
    TEST_ASSERT_EQ(stableDef->stabilityRank, 0);
    TEST_ASSERT(!stableDef->prerelease.allowed);
    TEST_ASSERT_EQ(stableDef->maxExpiryDays, 90);

    const auto* betaDef = policy.FindChannel("beta");
    TEST_ASSERT(betaDef != nullptr);
    TEST_ASSERT_EQ(betaDef->stabilityRank, 1);
    TEST_ASSERT(betaDef->prerelease.allowed);
    TEST_ASSERT_EQ(betaDef->prerelease.prefix, "beta");
    TEST_ASSERT_EQ(betaDef->maxExpiryDays, 30);

    const auto* nightlyDef = policy.FindChannel("nightly");
    TEST_ASSERT(nightlyDef != nullptr);
    TEST_ASSERT_EQ(nightlyDef->stabilityRank, 2);
    TEST_ASSERT(nightlyDef->prerelease.allowed);
    TEST_ASSERT_EQ(nightlyDef->prerelease.prefix, "nightly");
    TEST_ASSERT_EQ(nightlyDef->maxExpiryDays, 14);

    // Stability Downgrade (moving to a less stable channel)
    TEST_ASSERT(policy.IsStabilityDowngrade("stable", "beta"));
    TEST_ASSERT(policy.IsStabilityDowngrade("stable", "nightly"));
    TEST_ASSERT(policy.IsStabilityDowngrade("beta", "nightly"));
    TEST_ASSERT(!policy.IsStabilityDowngrade("nightly", "beta"));
    TEST_ASSERT(!policy.IsStabilityDowngrade("nightly", "stable"));
    TEST_ASSERT(!policy.IsStabilityDowngrade("beta", "stable"));
    TEST_ASSERT(!policy.IsStabilityDowngrade("stable", "stable"));
}
static void step_ChannelPolicy_ChannelAcceptanceAndPromotion() {
    const auto& policy = ChannelPolicy::Instance();

    // Stable channel accepts only stable
    TEST_ASSERT(policy.IsChannelAccepted("stable", "stable"));
    TEST_ASSERT(!policy.IsChannelAccepted("stable", "beta"));
    TEST_ASSERT(!policy.IsChannelAccepted("stable", "nightly"));
    TEST_ASSERT(!policy.IsChannelAccepted("stable", "unknown"));

    // Beta channel accepts beta and stable promotions
    TEST_ASSERT(policy.IsChannelAccepted("beta", "beta"));
    TEST_ASSERT(policy.IsChannelAccepted("beta", "stable"));
    TEST_ASSERT(!policy.IsChannelAccepted("beta", "nightly"));
    TEST_ASSERT(!policy.IsChannelAccepted("beta", "unknown"));

    // Nightly channel accepts nightly, beta, and stable promotions
    TEST_ASSERT(policy.IsChannelAccepted("nightly", "nightly"));
    TEST_ASSERT(policy.IsChannelAccepted("nightly", "beta"));
    TEST_ASSERT(policy.IsChannelAccepted("nightly", "stable"));
    TEST_ASSERT(!policy.IsChannelAccepted("nightly", "unknown"));

    // Unknown current channel fails closed
    TEST_ASSERT(!policy.IsChannelAccepted("unknown", "stable"));
    TEST_ASSERT(!policy.IsChannelAccepted("unknown", "beta"));
    TEST_ASSERT(!policy.IsChannelAccepted("unknown", "nightly"));
}
static void step_ChannelPolicy_SemVerPrereleaseSyntaxAndOrdering() {
    // 1. Parsing SemVer with prerelease and build metadata
    auto v1 = SemanticVersion::Parse("2.1.0");
    TEST_ASSERT(v1.has_value());
    TEST_ASSERT_EQ(v1->major, 2);
    TEST_ASSERT_EQ(v1->minor, 1);
    TEST_ASSERT_EQ(v1->patch, 0);
    TEST_ASSERT(v1->preRelease.empty());
    TEST_ASSERT(v1->buildMetadata.empty());

    auto v2 = SemanticVersion::Parse("v2.1.0-beta.1");
    TEST_ASSERT(v2.has_value());
    TEST_ASSERT_EQ(v2->preRelease, "beta.1");

    auto v3 = SemanticVersion::Parse("2.1.0-beta.10+20260831.x64");
    TEST_ASSERT(v3.has_value());
    TEST_ASSERT_EQ(v3->preRelease, "beta.10");
    TEST_ASSERT_EQ(v3->buildMetadata, "20260831.x64");

    // 2. SemVer 2.0.0 Numeric Identifier Precedence (Numeric vs Lexical)
    // In SemVer 2.0.0: beta.2 < beta.10 (numeric comparison of identifier parts)
    auto vBeta2 = SemanticVersion::Parse("2.1.0-beta.2");
    auto vBeta10 = SemanticVersion::Parse("2.1.0-beta.10");
    TEST_ASSERT(vBeta2.has_value() && vBeta10.has_value());
    TEST_ASSERT(*vBeta2 < *vBeta10);
    TEST_ASSERT(!(*vBeta10 < *vBeta2));

    // 3. Normal version has higher precedence than prerelease
    auto vNormal = SemanticVersion::Parse("2.1.0");
    TEST_ASSERT(*vBeta10 < *vNormal);
    TEST_ASSERT(!(*vNormal < *vBeta10));

    // 4. Identifier length precedence: beta < beta.1
    auto vBetaBare = SemanticVersion::Parse("2.1.0-beta");
    auto vBetaOne = SemanticVersion::Parse("2.1.0-beta.1");
    TEST_ASSERT(*vBetaBare < *vBetaOne);

    // 5. Alpha < Beta < RC
    auto vAlpha = SemanticVersion::Parse("2.1.0-alpha.1");
    auto vRc = SemanticVersion::Parse("2.1.0-rc.1");
    TEST_ASSERT(*vAlpha < *vBetaOne);
    TEST_ASSERT(*vBetaOne < *vRc);
    TEST_ASSERT(*vRc < *vNormal);

    // 6. Major/minor takes precedence over prerelease
    auto vOldNormal = SemanticVersion::Parse("2.0.0");
    auto vNewPrerelease = SemanticVersion::Parse("2.1.0-beta.1");
    TEST_ASSERT(*vOldNormal < *vNewPrerelease);

    // 7. Nightly timestamps
    auto vNightlyA = SemanticVersion::Parse("2.1.0-nightly.20260830.1");
    auto vNightlyB = SemanticVersion::Parse("2.1.0-nightly.20260831.1");
    auto vNightlyC = SemanticVersion::Parse("2.1.0-nightly.20260831.2");
    TEST_ASSERT(*vNightlyA < *vNightlyB);
    TEST_ASSERT(*vNightlyB < *vNightlyC);
}
static void step_ChannelPolicy_ChannelPrereleaseValidationRules() {
    const auto& policy = ChannelPolicy::Instance();

    // Stable: no prerelease allowed
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("stable", ""));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("stable", "beta.1"));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("stable", "nightly.20260831"));

    // Beta: must start with beta and have valid numeric sequence
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("beta", "beta"));
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("beta", "beta.1"));
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("beta", "beta.12"));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("beta", ""));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("beta", "nightly.1"));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("beta", "beta.alpha")); // non-numeric sequence
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("beta", "beta.01")); // leading zero numeric

    // Nightly: must start with nightly and have valid numeric sequence
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("nightly", "nightly"));
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("nightly", "nightly.20260831"));
    TEST_ASSERT(policy.IsValidPrereleaseForChannel("nightly", "nightly.20260831.1"));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("nightly", ""));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("nightly", "beta.1"));
    TEST_ASSERT(!policy.IsValidPrereleaseForChannel("nightly", "nightly.abc"));
}
static void step_ChannelPolicy_ManifestValidationChannelEligibilityAndPromotion() {
    ScopedTempDir tempDir;

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 15;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-08-25T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // Case 1: Unknown channel in manifest is rejected
    config.channel = "stable";
    manifest.channel = "unrecognized_channel";
    manifest.version = "2.1.0.0";
    UpdateCheckResult resUnknown = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resUnknown.status, UpdaterStatusCode::InvalidManifestJson);
    TEST_ASSERT(!resUnknown.updateAvailable);

    // Case 2: Configured stable, Manifest is beta -> UpToDate report (channel mismatch/not accepted)
    config.channel = "stable";
    manifest.channel = "beta";
    manifest.version = "2.1.0-beta.1";
    UpdateCheckResult resBetaOnStable = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resBetaOnStable.status, UpdaterStatusCode::UpToDate);
    TEST_ASSERT(!resBetaOnStable.updateAvailable);

    // Case 3: Configured beta, Manifest is stable (Promotion) -> Accepted and Update Available
    config.channel = "beta";
    manifest.channel = "stable";
    manifest.version = "2.1.0.0";
    UpdateCheckResult resStableOnBeta = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resStableOnBeta.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT(resStableOnBeta.updateAvailable);

    // Case 4: Stable manifest has prerelease tag -> InvalidManifestJson
    config.channel = "stable";
    manifest.channel = "stable";
    manifest.version = "2.1.0-beta.1";
    UpdateCheckResult resPrereleaseOnStable = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resPrereleaseOnStable.status, UpdaterStatusCode::InvalidManifestJson);
    TEST_ASSERT(!resPrereleaseOnStable.updateAvailable);

    // Case 5: Beta manifest with valid beta prerelease -> Update Available
    config.channel = "beta";
    manifest.channel = "beta";
    manifest.version = "2.1.0-beta.1";
    UpdateCheckResult resBetaOnBeta = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resBetaOnBeta.status, UpdaterStatusCode::UpdateAvailable);
    TEST_ASSERT(resBetaOnBeta.updateAvailable);

    // Case 6: Expiry duration exceeds channel policy maxExpiryDays
    // Nightly has maxExpiryDays = 14. Setting expiry window to 20 days should fail.
    config.channel = "nightly";
    manifest.channel = "nightly";
    manifest.version = "2.1.0-nightly.20260831.1";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-08-25T00:00:00Z"; // 24 days > 14 maxExpiryDays
    UpdateCheckResult resOverExpiry = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(resOverExpiry.status, UpdaterStatusCode::InvalidManifestJson);
    TEST_ASSERT(!resOverExpiry.updateAvailable);
}
static void step_ChannelPolicy_ChannelSwitchingDowngradeProtection() {
    ScopedTempDir tempDir;

    UpdaterConfig config;
    config.manifestUrl = "https://updates.example.com/release.json";
    config.allowUnsignedForTesting = true;
    std::tm tm = {};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 7;
    tm.tm_mday = 10;
    config.mockCurrentTime = std::chrono::system_clock::from_time_t(_mkgmtime(&tm));

    // Scenario: An older version manifest is presented (e.g. current is 2.0.0.0, manifest is 1.9.0.0)
    ReleaseManifest manifest;
    manifest.schemaVersion = 1;
    manifest.application = "Aperture";
    manifest.channel = "stable";
    manifest.version = "1.9.0.0";
    manifest.publishedUtc = "2026-08-01T00:00:00Z";
    manifest.expiryUtc = "2026-08-20T00:00:00Z";
    manifest.downloadUrl = "https://updates.example.com/Aperture-1.9.0.0.zip";
    manifest.fileSizeBytes = 10 * 1024 * 1024;
    manifest.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    config.channel = "stable";
    UpdateCheckResult res = ValidateReleaseManifest(manifest, config, tempDir.GetPath());
    TEST_ASSERT_EQ(res.status, UpdaterStatusCode::ManifestDowngrade);
    TEST_ASSERT(!res.updateAvailable);
}
static void step_ChannelPolicy_MissingTrustFailsClosedPerChannel() {
    ScopedTempDir tempDir;

    UpdaterConfig configStable("stable");
    configStable.manifestUrl = "";
    configStable.overridePinnedPublicKeyHex = "";
    configStable.overridePinnedSignerThumbprint = "";

    TEST_ASSERT(!configStable.IsTrustConfigured());
    UpdateCheckResult resStable = CheckForUpdates(configStable, nullptr, tempDir.GetPath());
    TEST_ASSERT_EQ(resStable.status, UpdaterStatusCode::NotConfigured);
    TEST_ASSERT(!resStable.updateAvailable);

    UpdaterConfig configBeta("beta");
    configBeta.manifestUrl = "";
    configBeta.overridePinnedPublicKeyHex = "";
    configBeta.overridePinnedSignerThumbprint = "";

    TEST_ASSERT(!configBeta.IsTrustConfigured());
    UpdateCheckResult resBeta = CheckForUpdates(configBeta, nullptr, tempDir.GetPath());
    TEST_ASSERT_EQ(resBeta.status, UpdaterStatusCode::NotConfigured);
    TEST_ASSERT(!resBeta.updateAvailable);
}



TEST_CASE(Updater, ManifestAndChannelPolicyContract) {
    step_UpdaterManifestValidation_UnconfiguredTrustReturnsNotConfiguredWithoutNetwork();
    step_UpdaterManifestValidation_MalformedJsonParsing();
    step_UpdaterManifestValidation_MissingRequiredFields();
    step_UpdaterManifestValidation_ExpiredManifestRejected();
    step_UpdaterManifestValidation_FuturePublishedTimestampRejected();
    step_UpdaterManifestValidation_DowngradeAndSameVersionProtection();
    step_UpdaterManifestValidation_MinimumUpdaterVersionRequirement();
    step_ChannelPolicy_DefaultPolicyDefinitionAndChannels();
    step_ChannelPolicy_ChannelAcceptanceAndPromotion();
    step_ChannelPolicy_SemVerPrereleaseSyntaxAndOrdering();
    step_ChannelPolicy_ChannelPrereleaseValidationRules();
    step_ChannelPolicy_ManifestValidationChannelEligibilityAndPromotion();
    step_ChannelPolicy_ChannelSwitchingDowngradeProtection();
    step_ChannelPolicy_MissingTrustFailsClosedPerChannel();
}

} // namespace Aperture::Test
