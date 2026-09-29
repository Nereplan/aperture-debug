#pragma once

#include "StressHarness.h"
#include "FolderWatcher.h"
#include "MediaLibrary.h"
#include "MpvPlayer.h"
#include "Updater.h"
#include "HttpTransport.h"
#include "OnlineSubtitles.h"
#include "Diagnostics.h"
#include "NetworkStream.h"
#include "Settings.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, CompositeMultiSubsystemConcurrentShutdown) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 18217 + 18;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.CompositeMultiSubsystemConcurrentShutdown", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Composite_");
        std::wstring rootDir = tempDir.GetPath();

        // 1. Diagnostics Subsystem
        TEST_ASSERT(Diagnostics::Initialize(true));

        // 2. FolderWatcher Subsystem
        CallbackLifecycleFence fwFence;
        auto folderWatcher = std::make_unique<FolderWatcher>();
        folderWatcher->SetCoalesceWindowMs(25);
        folderWatcher->SetCallback([&fwFence](const WatchChangeBatch&) {
            fwFence.OnCallbackInvoked("Composite.FolderWatcher");
        });
        TEST_ASSERT(folderWatcher->Start());
        TEST_ASSERT(folderWatcher->AddWatch(rootDir));

        // 3. MediaLibrary Subsystem
        MediaLibrary library;
        constexpr size_t kMediaCount = 30;
        for (size_t i = 0; i < kMediaCount; ++i) {
            std::wstring rel = L"comp_video_" + std::to_wstring(i) + L".mp4";
            tempDir.WriteFile(rel, "video_payload_" + std::to_string(i));
            MediaItem item;
            item.id = "comp_id_" + std::to_string(i);
            item.filePath = tempDir.SubPath(rel);
            item.title = L"Composite " + std::to_wstring(i);
            item.durationSec = 0.0;
            library.mediaItems.push_back(std::move(item));
        }
        library.RebuildPathIndex();

        std::atomic<bool> backfillCancel{false};
        std::thread backfillWorker([kMediaCount, &library, &backfillCancel]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MetadataBackfillOptions opt;
            opt.maxItems = kMediaCount;
            opt.forceRetry = true;
            auto progress = [&backfillCancel](size_t, size_t, const std::wstring&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return !backfillCancel.load(std::memory_order_acquire);
            };
            library.BackfillMissingMetadata(opt, progress, false);
            CoUninitialize();
        });

        // 4. Player Event Subsystem
        CallbackLifecycleFence playerFence;
        std::atomic<bool> playerCallbacksDisabled{false};
        std::atomic<bool> stopPlayerEvents{false};

        std::thread playerProducer([&playerFence, &playerCallbacksDisabled, &stopPlayerEvents]() {
            double t = 0.0;
            while (!stopPlayerEvents.load(std::memory_order_relaxed)) {
                t += 0.1;
                if (!playerCallbacksDisabled.load(std::memory_order_acquire)) {
                    playerFence.OnCallbackInvoked("Composite.PlayerEvent");
                }
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });

        // 5. Updater Subsystem with Mock HTTP Transport
        MockHttpTransport mockTransport;
        std::string mockPkg(512 * 1024, 'U');
        HttpResponse pkgResp;
        pkgResp.success = true;
        pkgResp.statusCode = 200;
        pkgResp.body = mockPkg;
        pkgResp.status = HttpTransportStatus::Success;

        std::atomic<bool> updaterBlocked{true};
        mockTransport.SetHandler(L"https://update.aperture.local/",
            [&pkgResp, &updaterBlocked](const std::wstring&, const HttpRequestOptions&) -> HttpResponse {
                while (updaterBlocked.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return pkgResp;
            });

        ReleaseManifest manifest;
        manifest.version = "2.5.0";
        manifest.downloadUrl = "https://update.aperture.local/pkg.zip";
        manifest.fileSizeBytes = mockPkg.size();
        manifest.sha256 = ComputeSha256Hex(mockPkg.data(), mockPkg.size());

        UpdaterConfig uConfig("stable");
        uConfig.allowUnsignedForTesting = true;
        uConfig.allowSameVersion = true;
        uConfig.allowedHosts = { "update.aperture.local" };

        std::thread updaterWorker([&manifest, &uConfig, &mockTransport, &rootDir]() {
            DownloadAndStageUpdate(manifest, uConfig, &mockTransport, nullptr, rootDir);
        });

        // 6. NetworkStreamLoader Subsystem
        NetworkStreamLoader nsLoader;
        CallbackLifecycleFence nsFence;
        nsLoader.BeginOpen(L"https://stream.aperture.local/live.m3u8", nullptr,
            [&nsFence](bool, const std::wstring&, const std::wstring&, uint64_t) {
                nsFence.OnCallbackInvoked("Composite.NetworkStreamLoader");
            });

        // 7. Active Disk Mutations Thread
        std::atomic<bool> stopDiskMutator{false};
        std::thread diskMutator([&tempDir, &stopDiskMutator]() {
            uint32_t idx = 0;
            while (!stopDiskMutator.load(std::memory_order_relaxed)) {
                std::wstring fn = L"active_" + std::to_wstring(idx++) + L".dat";
                tempDir.WriteFile(fn, "live mutation payload");
                std::this_thread::yield();
            }
        });

        // Let all concurrent operations execute for a seeded duration
        uint32_t delayMs = sched.NextDelayMs(5, 35);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        // =====================================================================
        // Ordered Concurrent Shutdown Sequence
        // =====================================================================

        // 1. Disable player callbacks and join producer thread
        playerCallbacksDisabled.store(true, std::memory_order_release);
        stopPlayerEvents.store(true, std::memory_order_release);
        if (playerProducer.joinable()) playerProducer.join();
        playerFence.SetShutdown();

        // 2. Cancel and join NetworkStreamLoader
        nsLoader.Cancel();
        nsFence.SetShutdown();
        // 3. Cancel and join MediaLibrary backfill
        backfillCancel.store(true, std::memory_order_release);
        if (backfillWorker.joinable()) backfillWorker.join();

        // 4. Unblock / cancel and join Updater
        updaterBlocked.store(false, std::memory_order_release);
        if (updaterWorker.joinable()) updaterWorker.join();

        // 5. Stop disk mutator
        stopDiskMutator.store(true, std::memory_order_release);
        if (diskMutator.joinable()) diskMutator.join();

        // 6. Fence and stop FolderWatcher
        folderWatcher->Stop();
        fwFence.SetShutdown();
        folderWatcher.reset();

        // 7. Shutdown Diagnostics watchdog
        Diagnostics::Shutdown();

        // Verify lifecycle invariants
        TEST_ASSERT_EQ(playerFence.GetPostShutdownViolations(), 0u);
        TEST_ASSERT_EQ(fwFence.GetPostShutdownViolations(), 0u);
    }
}

} // namespace Aperture::Test::Stress
