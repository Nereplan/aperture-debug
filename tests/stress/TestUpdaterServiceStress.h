#pragma once

#include "StressHarness.h"
#include "Updater.h"
#include "HttpTransport.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, UpdaterAsyncDownloadCancellationAndCleanup) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 10111 + 10;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.UpdaterAsyncDownloadCancellationAndCleanup", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Updater_");
        std::wstring rootDir = tempDir.GetPath();

        // Prepare Mock HTTP transport with injected latency
        MockHttpTransport mockTransport;
        std::wstring downloadUrl = L"https://updates.aperture.local/packages/v2.1.0.zip";

        std::string mockZipPayload(1024 * 1024, 'X'); // 1 MB payload
        HttpResponse downloadResponse;
        downloadResponse.success = true;
        downloadResponse.statusCode = 200;
        downloadResponse.body = mockZipPayload;
        downloadResponse.finalUrl = downloadUrl;
        downloadResponse.status = HttpTransportStatus::Success;

        std::atomic<bool> transportBlocked{true};
        mockTransport.SetHandler(L"https://updates.aperture.local/packages/",
            [&downloadResponse, &transportBlocked](const std::wstring&, const HttpRequestOptions&) -> HttpResponse {
                // Simulate network latency / chunked transfer delay
                for (int i = 0; i < 20 && transportBlocked.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return downloadResponse;
            });

        ReleaseManifest manifest;
        manifest.schemaVersion = 1;
        manifest.application = "Aperture";
        manifest.version = "2.1.0";
        manifest.channel = "stable";
        manifest.downloadUrl = "https://updates.aperture.local/packages/v2.1.0.zip";
        manifest.fileSizeBytes = mockZipPayload.size();
        manifest.sha256 = ComputeSha256Hex(mockZipPayload.data(), mockZipPayload.size());

        UpdaterConfig uConfig("stable");
        uConfig.allowUnsignedForTesting = true;
        uConfig.allowSameVersion = true;
        uConfig.allowedHosts = { "updates.aperture.local" };

        std::atomic<bool> downloadFinished{false};
        UpdateDownloadResult downloadResult{};

        std::thread downloadThread([&manifest, &uConfig, &mockTransport, &rootDir, &downloadResult, &downloadFinished]() {
            downloadResult = DownloadAndStageUpdate(
                manifest, uConfig, &mockTransport, nullptr, rootDir);
            downloadFinished.store(true, std::memory_order_release);
        });

        // Seeded delay before unblocking or completing
        uint32_t delayMs = sched.NextDelayMs(2, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        transportBlocked.store(false, std::memory_order_release);

        if (downloadThread.joinable()) {
            downloadThread.join();
        }

        TEST_ASSERT(downloadFinished.load());
    }
}

TEST_CASE(Stress, UpdaterPrivateModeWriteSuppression) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 11113 + 11;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.UpdaterPrivateModeWriteSuppression", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Updater_Priv_");
        std::wstring rootDir = tempDir.GetPath();

        UpdaterState state;
        state.lastCheckUtc = "2026-08-31T12:00:00Z";
        state.lastCheckStatus = "UpdateAvailable";
        state.lastAvailableVersion = "2.2.0";

        // In private mode, SaveUpdaterState must succeed with OK status but produce NO durable file
        StorageStatus statusPrivate = SaveUpdaterState(state, true, rootDir);
        TEST_ASSERT(statusPrivate.is_ok());

        std::wstring updaterFilePath = tempDir.SubPath(L"data\\updater.json");
        TEST_ASSERT(!FileExists(updaterFilePath));

        // In normal (non-private) mode, SaveUpdaterState writes data/updater.json
        StorageStatus statusNormal = SaveUpdaterState(state, false, rootDir);
        TEST_ASSERT(statusNormal.is_ok());
        TEST_ASSERT(FileExists(updaterFilePath));

        // Rapid concurrent saves across threads with mixed private/normal modes
        std::atomic<bool> stopSaves{false};
        constexpr size_t kThreads = 3;
        std::vector<std::thread> saveWorkers;
        saveWorkers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            saveWorkers.emplace_back([t, &state, &rootDir, &stopSaves]() {
                while (!stopSaves.load(std::memory_order_relaxed)) {
                    bool priv = (t % 2 == 0);
                    SaveUpdaterState(state, priv, rootDir);
                    std::this_thread::yield();
                }
            });
        }

        uint32_t delayMs = sched.NextDelayMs(2, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        stopSaves.store(true, std::memory_order_release);
        for (auto& w : saveWorkers) {
            if (w.joinable()) w.join();
        }
    }
}

} // namespace Aperture::Test::Stress
