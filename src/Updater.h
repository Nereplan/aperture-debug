#pragma once

#include "Common.h"
#include "Json.h"
#include "ChannelPolicy.h"
#include "HttpTransport.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <chrono>
#include <functional>

namespace Aperture {

enum class UpdaterStatusCode {
    Success = 0, UpToDate, UpdateAvailable, Disabled, NotConfigured, InvalidManifestJson, ManifestExpired, ManifestFutureTimestamp,
    ManifestDowngrade, ManifestReplay, MinimumUpdaterVersionUnsatisfied, InvalidSignature, UntrustedSigner, HashMismatch,
    AuthenticodeVerificationFailed, AuthenticodeSignerMismatch, InvalidPeArchitecture, NetworkError, Timeout, TooManyRedirects,
    DisallowedRedirect, HttpError, ResponseTooLarge, UnwritableRoot, InsufficientDiskSpace, StagingError, HelperValidationError, SwapFailed,
    RollbackSucceeded, RollbackFailed, Cancelled
};

// --- Semantic Versioning ---
struct SemanticVersion {
    int major = 0, minor = 0, patch = 0, build = 0;
    std::string preRelease, buildMetadata;
    static std::optional<SemanticVersion> Parse(const std::string& str);
    std::string ToString() const;
    bool operator<(const SemanticVersion& other) const;
    bool operator<=(const SemanticVersion& other) const;
    bool operator==(const SemanticVersion& other) const;
    bool operator!=(const SemanticVersion& other) const;
    bool operator>=(const SemanticVersion& other) const;
    bool operator>(const SemanticVersion& other) const;
    static SemanticVersion Current();
};

// --- Manifest File Entry ---
struct ManifestFileEntry { std::string path; uint64_t size = 0; std::string sha256; };

// --- Canonical Release Manifest ---
struct ReleaseManifest {
    int schemaVersion = 1;
    std::string application = "Aperture";
    std::string version;
    std::string channel = "stable";
    std::string minimumUpdaterVersion, publishedUtc, expiryUtc, downloadUrl;
    uint64_t fileSizeBytes = 0;
    std::string sha256, signerThumbprint, signature;
    std::string signatureAlgorithm = "ECDSA_P256_SHA256";
    std::vector<ManifestFileEntry> files;
    std::string releaseNotes;
    JsonValue extraFields = JsonValue::Object();
    std::string rawJson;
    std::string ComputeCanonicalString() const;
    static bool Parse(const std::string& jsonText, ReleaseManifest& outManifest, std::string& outError);
    std::string SerializeJson() const;
};

// --- Build-Time Immutable Trust Configuration ---
struct BuildTimeTrustConfig {
    static std::string GetManifestUrl(const std::string& channel = "");
    static std::vector<std::string> GetAllowedHosts(const std::string& channel = "");
    static std::string GetPinnedPublicKeyHex(const std::string& channel = "");
    static std::string GetPinnedSignerThumbprint(const std::string& channel = "");
};

// --- Updater Configuration ---
struct UpdaterConfig {
    std::string manifestUrl;                        // Default initialized from BuildTimeTrustConfig
    std::string channel = "stable";
    std::vector<std::string> allowedHosts;          // Default initialized from BuildTimeTrustConfig
    size_t maxManifestSizeBytes = 128 * 1024;        // 128 KiB
    size_t maxPackageSizeBytes = 150 * 1024 * 1024;  // 150 MiB
    int connectTimeoutMs = 5000, downloadTimeoutMs = 60000, maxRedirects = 3;
    bool allowSameVersion = false;                  // Set true in tests / manual reinstall
    bool allowUnsignedForTesting = false;
    std::string overridePinnedPublicKeyHex;         // Explicit test trust injection
    std::string overridePinnedSignerThumbprint;     // Explicit test trust injection
    std::optional<std::chrono::system_clock::time_point> mockCurrentTime; // For deterministic tests
    std::shared_ptr<OperationCancellation> cancellation;
    UpdaterConfig(const std::string& channelName = "stable");
    bool IsTrustConfigured() const;
    std::string GetEffectivePublicKeyHex() const;
    std::string GetEffectiveSignerThumbprint() const;
    std::string GetEffectiveManifestUrl() const;
};

// --- Results & Outcomes ---
struct UpdateCheckResult {
    UpdaterStatusCode status = UpdaterStatusCode::Success;
    bool updateAvailable = false;
    ReleaseManifest manifest;
    std::string message;
    int httpStatusCode = 0;
};

class ScopedTempDirectory;
struct UpdateDownloadResult {
    UpdaterStatusCode status = UpdaterStatusCode::Success;
    std::wstring stagingDirectory;
    std::vector<std::wstring> stagedFiles;
    ReleaseManifest manifest;
    std::string message;
    // Cancelled/stale asynchronous results release their staging directory.
    std::shared_ptr<ScopedTempDirectory> stagingLease;
};

struct UpdateApplyResult { UpdaterStatusCode status = UpdaterStatusCode::Success; std::string message; };

struct UpdaterState { std::string lastCheckUtc; std::string lastCheckStatus; std::string lastAvailableVersion; std::string lastError; };

// --- Core Updater Functions ---

// Validates a parsed release manifest against updater configuration, current version, time, signature, etc.
UpdateCheckResult ValidateReleaseManifest( const ReleaseManifest& manifest, const UpdaterConfig& config,
    const std::wstring& rootDir = GetExeDir());

// Queries update endpoint and checks for an available release (returns NotConfigured immediately if unprovisioned)
UpdateCheckResult CheckForUpdates( const UpdaterConfig& config, IHttpTransport* transport = nullptr,
    const std::wstring& rootDir = GetExeDir());

// Downloads package, verifies SHA-256, stages files, verifies Authenticode and PE architecture
UpdateDownloadResult DownloadAndStageUpdate( const ReleaseManifest& manifest, const UpdaterConfig& config,
    IHttpTransport* transport = nullptr, std::function<void(float progress)> progressCallback = nullptr,
    const std::wstring& rootDir = GetExeDir());

// Spawns the current binary in `--update-helper` mode and signals process exit
UpdateApplyResult LaunchHelperAndExit( const UpdateDownloadResult& downloadResult, bool relaunch = true,
    const std::wstring& targetDir = GetExeDir());

// Helper execution entry point dispatched when `Aperture.exe --update-helper ...` is invoked
int RunHelperMode(int argc, wchar_t* argv[]);

// Durable state management (persisted to `<rootDir>/data/updater.json`, suppressed in private mode)
StorageStatus LoadUpdaterState(UpdaterState& outState, const std::wstring& rootDir = GetExeDir());
StorageStatus SaveUpdaterState(const UpdaterState& state, bool isPrivateMode, const std::wstring& rootDir = GetExeDir());

// --- Cryptographic & PE Inspection Helpers ---
std::string ComputeSha256Hex(const void* data, size_t size);
std::string ComputeFileSha256Hex(const std::wstring& path);
std::string FormatIso8601Utc(std::chrono::system_clock::time_point tp);
bool ParseIso8601Utc(const std::string& str, std::chrono::system_clock::time_point& outTime);

bool VerifyEcdsaP256Signature( const std::string& canonicalData, const std::string& signatureHexOrBase64,
    const std::vector<uint8_t>& publicKeyBlob);

bool SignEcdsaP256( const std::string& canonicalData, const std::vector<uint8_t>& privateKeyBlob, std::string& outSignatureHex);

bool GenerateEcdsaP256KeyPair( std::vector<uint8_t>& outPublicBlob, std::vector<uint8_t>& outPrivateBlob);

bool VerifyAuthenticodeAndPin( const std::wstring& filePath, const std::string& expectedThumbprintHex, std::string& outActualThumbprint,
    std::string& outError);

bool VerifyPeArchitectureX64( const std::wstring& filePath, std::string& outError);

std::wstring GetUpdaterStatusMessage(UpdaterStatusCode status, const std::string& detail = "");

// Helper RAII for temporary directory cleanup
class ScopedTempDirectory {
public:
    ScopedTempDirectory(const std::wstring& prefix = L"Aperture_Update_");
    ~ScopedTempDirectory();
    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory(ScopedTempDirectory&& other) noexcept;
    ScopedTempDirectory& operator=(ScopedTempDirectory&& other) noexcept;
    const std::wstring& GetPath() const { return path_; }
    std::wstring SubPath(const std::wstring& rel) const;
    void DisableAutoCleanup() { autoCleanup_ = false; }
    void Cleanup();

private:
    std::wstring path_;
    bool autoCleanup_ = true;
};

} // namespace Aperture
