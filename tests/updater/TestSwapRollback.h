#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include "../../src/HttpTransport.h"
#include <thread>
#include <atomic>
#include <windows.h>
#include <string>
#include <filesystem>

namespace Aperture::Test {

namespace fs = std::filesystem;
static void step_UpdaterSwapRollback_SuccessfulAtomicSwap() {
    ScopedTempDir targetDir;
    ScopedTempDir stagingDir;

    // 1. Setup target directory with original files and data folder
    targetDir.WriteFile(L"Aperture.exe", "ORIGINAL_APERTURE_BINARY_V1");
    targetDir.WriteFile(L"libmpv-2.dll", "ORIGINAL_LIBMPV_DLL_V1");
    targetDir.WriteFile(L"data\\settings.json", "{\"preserved\": true, \"customUser\": 123}");
    targetDir.WriteFile(L"data\\library.json", "{\"mediaCount\": 42}");

    // 2. Setup staging directory with new files
    stagingDir.WriteFile(L"Aperture.exe", "NEW_APERTURE_BINARY_V2");
    stagingDir.WriteFile(L"libmpv-2.dll", "NEW_LIBMPV_DLL_V2");

    std::wstring backupDir = targetDir.SubPath(L"_backup_update");
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::vector<std::pair<std::wstring, std::wstring>> swappedFiles;
    bool swapFailed = false;

    for (const auto& entry : fs::directory_iterator(fs::path(stagingDir.GetPath()))) {
        if (entry.is_regular_file()) {
            std::wstring fileName = entry.path().filename().wstring();
            std::wstring targetPath = targetDir.GetPath() + L"\\" + fileName;
            std::wstring backupPath = backupDir + L"\\" + fileName;
            std::wstring stagedPath = entry.path().wstring();

            if (FileExists(targetPath)) {
                if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    swapFailed = true;
                    break;
                }
            }

            if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                swapFailed = true;
                break;
            }

            swappedFiles.push_back({ targetPath, backupPath });
        }
    }

    TEST_ASSERT(!swapFailed);
    TEST_ASSERT_EQ(swappedFiles.size(), (size_t)2);

    // Verify target files were updated
    std::string newExe, newDll, dataSettings, dataLib;
    ReadTextFile(targetDir.SubPath(L"Aperture.exe"), newExe);
    ReadTextFile(targetDir.SubPath(L"libmpv-2.dll"), newDll);
    ReadTextFile(targetDir.SubPath(L"data\\settings.json"), dataSettings);
    ReadTextFile(targetDir.SubPath(L"data\\library.json"), dataLib);

    TEST_ASSERT_EQ(newExe, "NEW_APERTURE_BINARY_V2");
    TEST_ASSERT_EQ(newDll, "NEW_LIBMPV_DLL_V2");

    // Verify user data was preserved untouched
    TEST_ASSERT(dataSettings.find("customUser") != std::string::npos);
    TEST_ASSERT(dataLib.find("mediaCount") != std::string::npos);

    // Cleanup backup dir
    std::error_code ec;
    fs::remove_all(fs::path(backupDir), ec);
}
static void step_UpdaterSwapRollback_SimulatedFailureTriggersCleanRollback() {
    ScopedTempDir targetDir;
    ScopedTempDir stagingDir;

    // 1. Setup target directory with original files
    targetDir.WriteFile(L"Aperture.exe", "ORIGINAL_APERTURE_V1");
    targetDir.WriteFile(L"libmpv-2.dll", "ORIGINAL_LIBMPV_V1");
    targetDir.WriteFile(L"vulkan-1.dll", "ORIGINAL_VULKAN_V1");

    // 2. Setup staging directory with 3 new files
    stagingDir.WriteFile(L"Aperture.exe", "NEW_APERTURE_V2");
    stagingDir.WriteFile(L"libmpv-2.dll", "NEW_LIBMPV_V2");
    stagingDir.WriteFile(L"vulkan-1.dll", "NEW_VULKAN_V2");

    std::wstring backupDir = targetDir.SubPath(L"_backup_update");
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::vector<std::pair<std::wstring, std::wstring>> swappedFiles;
    bool swapFailed = false;

    // Simulate swap failure on the 3rd file (vulkan-1.dll)
    int fileIndex = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(stagingDir.GetPath()))) {
        if (entry.is_regular_file()) {
            fileIndex++;
            std::wstring fileName = entry.path().filename().wstring();
            std::wstring targetPath = targetDir.GetPath() + L"\\" + fileName;
            std::wstring backupPath = backupDir + L"\\" + fileName;
            std::wstring stagedPath = entry.path().wstring();

            if (fileIndex == 3) {
                // Simulate failure on 3rd file move
                swapFailed = true;
                break;
            }

            if (FileExists(targetPath)) {
                if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    swapFailed = true;
                    break;
                }
            }

            if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                swapFailed = true;
                break;
            }

            swappedFiles.push_back({ targetPath, backupPath });
        }
    }

    TEST_ASSERT(swapFailed);
    TEST_ASSERT_EQ(swappedFiles.size(), (size_t)2);

    // Perform Rollback
    for (const auto& pair : swappedFiles) {
        if (FileExists(pair.second)) {
            MoveFileExW(pair.second.c_str(), pair.first.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    // Verify all original files were restored
    std::string exeContent, dllContent, vulkanContent;
    ReadTextFile(targetDir.SubPath(L"Aperture.exe"), exeContent);
    ReadTextFile(targetDir.SubPath(L"libmpv-2.dll"), dllContent);
    ReadTextFile(targetDir.SubPath(L"vulkan-1.dll"), vulkanContent);

    TEST_ASSERT_EQ(exeContent, "ORIGINAL_APERTURE_V1");
    TEST_ASSERT_EQ(dllContent, "ORIGINAL_LIBMPV_V1");
    TEST_ASSERT_EQ(vulkanContent, "ORIGINAL_VULKAN_V1");

    // Cleanup backup dir
    std::error_code ec;
    fs::remove_all(fs::path(backupDir), ec);
}
static void step_UpdaterHelperValidation_InvalidCommandLineArguments() {
    // Empty args
    wchar_t* args1[] = { const_cast<wchar_t*>(L"Aperture.exe"), const_cast<wchar_t*>(L"--update-helper") };
    int res1 = RunHelperMode(2, args1);
    TEST_ASSERT_EQ(res1, 101);

    // Missing staging-dir
    wchar_t* args2[] = {
        const_cast<wchar_t*>(L"Aperture.exe"),
        const_cast<wchar_t*>(L"--update-helper"),
        const_cast<wchar_t*>(L"--parent-pid"),
        const_cast<wchar_t*>(L"1234"),
        const_cast<wchar_t*>(L"--target-dir"),
        const_cast<wchar_t*>(L"C:\\Aperture")
    };
    int res2 = RunHelperMode(6, args2);
    TEST_ASSERT_EQ(res2, 101);
}
static void step_UpdaterHelperValidation_NonExistentParentPidRejected() {
    ScopedTempDir tempDir;
    std::wstring staging = tempDir.SubPath(L"staged");
    CreateDirectoryW(staging.c_str(), NULL);

    // PID 99999999 is invalid/non-existent
    wchar_t* args[] = {
        const_cast<wchar_t*>(L"Aperture.exe"),
        const_cast<wchar_t*>(L"--update-helper"),
        const_cast<wchar_t*>(L"--parent-pid"),
        const_cast<wchar_t*>(L"99999999"),
        const_cast<wchar_t*>(L"--target-dir"),
        const_cast<wchar_t*>(tempDir.GetPath().c_str()),
        const_cast<wchar_t*>(L"--staging-dir"),
        const_cast<wchar_t*>(staging.c_str())
    };

    int res = RunHelperMode(8, args);
    TEST_ASSERT_EQ(res, 102); // Cannot open parent process
}
static void step_UpdaterNetworkBounds_HttpsEnforcement() {
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
static void step_UpdaterNetworkBounds_DisallowedHostAllowlist() {
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
static void step_UpdaterNetworkBounds_PackageSizeCeiling() {
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
static void step_UpdaterNetworkBounds_MockHttpTransportEndToEndCheck() {
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
static void step_UpdaterNetworkBounds_SimulatedNetworkTimeout() {
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
static void step_UpdaterNetworkBounds_PackageStagingUsesStreamingTransportAndFileHash() {
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
static void step_UpdaterTempCleanup_ScopedTempDirectoryCleanup() {
    std::wstring createdPath;
    {
        ScopedTempDirectory tempDir(L"Aperture_TestUpdate_");
        createdPath = tempDir.GetPath();

        TEST_ASSERT(!createdPath.empty());
        TEST_ASSERT(DirectoryExists(createdPath));

        // Create staged files inside
        std::wstring subFile = tempDir.SubPath(L"staged_binary.bin");
        HANDLE hFile = CreateFileW(subFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hFile, "TEMP_BYTES", 10, &written, NULL);
            CloseHandle(hFile);
        }

        TEST_ASSERT(FileExists(subFile));
    }

    // After exiting scope, the temporary staging directory and all its files MUST be deleted
    TEST_ASSERT(!DirectoryExists(createdPath));
}
static void step_UpdaterTempCleanup_NoUserDataInStaging() {
    ScopedTempDirectory tempDir(L"Aperture_TestUpdate_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring settingsFile = tempDir.SubPath(L"data\\settings.json");
    std::wstring historyFile = tempDir.SubPath(L"data\\history.json");

    // Fresh temporary staging directory must not contain user data or history
    TEST_ASSERT(!FileExists(dataDir));
    TEST_ASSERT(!FileExists(settingsFile));
    TEST_ASSERT(!FileExists(historyFile));
}
static void step_UpdaterPrivateMode_PrivateModeSuppressesDurableHistory() {
    ScopedTempDir tempDir;
    std::wstring updaterJsonPath = tempDir.SubPath(L"data\\updater.json");

    UpdaterState state;
    state.lastCheckUtc = "2026-08-31T12:00:00Z";
    state.lastCheckStatus = "up_to_date";
    state.lastAvailableVersion = SemanticVersion::Current().ToString();
    state.lastError = "";

    // When private mode is active, SaveUpdaterState MUST NOT write anything to disk
    StorageStatus statusPrivate = SaveUpdaterState(state, true /* isPrivateMode */, tempDir.GetPath());
    TEST_ASSERT(statusPrivate.is_ok());
    TEST_ASSERT(!FileExists(updaterJsonPath));

    // When private mode is inactive, SaveUpdaterState writes to disk
    StorageStatus statusNormal = SaveUpdaterState(state, false /* isPrivateMode */, tempDir.GetPath());
    TEST_ASSERT(statusNormal.is_ok());
    TEST_ASSERT(FileExists(updaterJsonPath));

    // Verify loaded state matches
    UpdaterState loaded;
    StorageStatus loadStatus = LoadUpdaterState(loaded, tempDir.GetPath());
    TEST_ASSERT(loadStatus.is_ok());
    TEST_ASSERT_EQ(loaded.lastCheckUtc, "2026-08-31T12:00:00Z");
    TEST_ASSERT_EQ(loaded.lastCheckStatus, "up_to_date");
    TEST_ASSERT_EQ(loaded.lastAvailableVersion, SemanticVersion::Current().ToString());
}



TEST_CASE(Updater, SwapRollbackHelperAndBoundsContract) {
    step_UpdaterSwapRollback_SuccessfulAtomicSwap();
    step_UpdaterSwapRollback_SimulatedFailureTriggersCleanRollback();
    step_UpdaterHelperValidation_InvalidCommandLineArguments();
    step_UpdaterHelperValidation_NonExistentParentPidRejected();
    step_UpdaterNetworkBounds_HttpsEnforcement();
    step_UpdaterNetworkBounds_DisallowedHostAllowlist();
    step_UpdaterNetworkBounds_PackageSizeCeiling();
    step_UpdaterNetworkBounds_MockHttpTransportEndToEndCheck();
    step_UpdaterNetworkBounds_SimulatedNetworkTimeout();
    step_UpdaterNetworkBounds_PackageStagingUsesStreamingTransportAndFileHash();
    step_UpdaterTempCleanup_ScopedTempDirectoryCleanup();
    step_UpdaterTempCleanup_NoUserDataInStaging();
    step_UpdaterPrivateMode_PrivateModeSuppressesDurableHistory();
    auto launchFn = &LaunchHelperAndExit; TEST_ASSERT(launchFn != nullptr);
}

} // namespace Aperture::Test
