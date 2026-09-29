#pragma once

#include "StressHarness.h"
#include "FolderWatcher.h"
#include <filesystem>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

namespace fs = std::filesystem;

TEST_CASE(Stress, FolderWatcherConcurrentMutations) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 1337 + 1;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.FolderWatcherConcurrentMutations", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_FW_");
        std::wstring watchRoot = tempDir.GetPath();

        // Create subdirectories
        std::wstring subDirA = tempDir.SubPath(L"dir_a");
        std::wstring subDirB = tempDir.SubPath(L"dir_b");
        CreateDirectoryW(subDirA.c_str(), NULL);
        CreateDirectoryW(subDirB.c_str(), NULL);

        CallbackLifecycleFence fence;
        std::atomic<uint32_t> batchCount{0};
        std::atomic<uint32_t> totalItemsReceived{0};

        auto watcher = std::make_unique<FolderWatcher>();
        watcher->SetCoalesceWindowMs(20);
        watcher->SetCallback([&fence, &batchCount, &totalItemsReceived](const WatchChangeBatch& batch) {
            fence.OnCallbackInvoked("FolderWatcher.Callback");
            batchCount++;
            totalItemsReceived += (uint32_t)batch.items.size();
        });

        TEST_ASSERT(watcher->Start());
        TEST_ASSERT(watcher->AddWatch(watchRoot));
        TEST_ASSERT(watcher->AddWatch(subDirA));
        TEST_ASSERT(watcher->AddWatch(subDirB));

        std::atomic<bool> stopMutators{false};
        std::atomic<uint32_t> filesCreated{0};
        std::atomic<uint32_t> filesModified{0};
        std::atomic<uint32_t> filesDeleted{0};

        // Spawn mutator threads
        constexpr size_t kMutatorCount = 4;
        std::vector<std::thread> mutators;
        mutators.reserve(kMutatorCount);

        for (size_t t = 0; t < kMutatorCount; ++t) {
            mutators.emplace_back([t, &stopMutators, &tempDir, &sched, &filesCreated, &filesModified, &filesDeleted]() {
                uint32_t fileIdx = 0;
                while (!stopMutators.load(std::memory_order_relaxed)) {
                    std::wstring fileName = L"dir_a\\mut_" + std::to_wstring(t) + L"_" + std::to_wstring(fileIdx++) + L".mp4";
                    // Create file
                    if (tempDir.WriteFile(fileName, "initial content")) {
                        filesCreated++;
                    }
                    // Modify file
                    if (tempDir.WriteFile(fileName, "modified content payload")) {
                        filesModified++;
                    }
                    // Occasional delete
                    if ((fileIdx % 3) == 0) {
                        std::wstring full = tempDir.SubPath(fileName);
                        if (DeleteFileW(full.c_str())) {
                            filesDeleted++;
                        }
                    }
                    std::this_thread::yield();
                }
            });
        }

        // Let mutations run for a seeded duration before triggering shutdown
        uint32_t runDurationMs = sched.NextDelayMs(5, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(runDurationMs));

        // Stop mutator threads and join
        stopMutators.store(true, std::memory_order_release);
        for (auto& m : mutators) {
            if (m.joinable()) m.join();
        }

        watcher->Stop();
        fence.SetShutdown();

        // Verify watcher state and lifecycle invariants
        TEST_ASSERT(!watcher->IsRunning());
        TEST_ASSERT_EQ(fence.GetPostShutdownViolations(), 0u);
        TEST_ASSERT(filesCreated.load() > 0);

        // Explicitly destroy watcher before next iteration
        watcher.reset();
    }
}

TEST_CASE(Stress, FolderWatcherRapidStartStopCycles) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 2027 + 2;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.FolderWatcherRapidStartStopCycles", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_FW_Rapid_");
        std::wstring watchRoot = tempDir.GetPath();

        CallbackLifecycleFence fence;
        auto watcher = std::make_unique<FolderWatcher>();
        watcher->SetCallback([&fence](const WatchChangeBatch&) {
            fence.OnCallbackInvoked("FolderWatcher.RapidCallback");
        });

        constexpr uint32_t kCycles = 15;
        for (uint32_t c = 0; c < kCycles; ++c) {
            TEST_ASSERT(watcher->Start());
            TEST_ASSERT(watcher->AddWatch(watchRoot));

            // Generate a rapid file creation
            std::wstring testFile = L"rapid_" + std::to_wstring(c) + L".mkv";
            tempDir.WriteFile(testFile, "sample video data");

            // Seeded delay before Stop
            uint32_t delayMs = sched.NextDelayMs(0, 10);
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            watcher->Stop();
            TEST_ASSERT(!watcher->IsRunning());
        }

        watcher.reset();
    }
}

TEST_CASE(Stress, FolderWatcherCoalescingAndOverflowStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 3041 + 3;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.FolderWatcherCoalescingAndOverflowStress", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_FW_Coalesce_");
        std::wstring watchRoot = tempDir.GetPath();

        CallbackLifecycleFence fence;
        std::atomic<uint32_t> batchesReceived{0};

        auto watcher = std::make_unique<FolderWatcher>();
        watcher->SetCoalesceWindowMs(50);
        watcher->SetMaxBatchSize(64);
        watcher->SetCallback([&fence, &batchesReceived](const WatchChangeBatch&) {
            fence.OnCallbackInvoked("FolderWatcher.CoalesceCallback");
            batchesReceived++;
        });

        TEST_ASSERT(watcher->Start());
        TEST_ASSERT(watcher->AddWatch(watchRoot));

        // Inject burst of manual events and trigger rescan
        std::thread injector([&watcher, &watchRoot, &sched]() {
            for (int i = 0; i < 200; ++i) {
                WatchChangeItem item;
                item.action = (i % 2 == 0) ? WatchAction::Added : WatchAction::Modified;
                item.path = watchRoot + L"\\burst_" + std::to_wstring(i) + L".mp4";
                item.rootFolder = watchRoot;
                watcher->EnqueueEvent(item);

                if (i % 50 == 0) {
                    watcher->TriggerRescan(watchRoot);
                }
            }
        });

        if (injector.joinable()) injector.join();

        watcher->Stop();
        fence.SetShutdown();

        TEST_ASSERT(!watcher->IsRunning());
        TEST_ASSERT_EQ(fence.GetPostShutdownViolations(), 0u);

        watcher.reset();
    }
}

TEST_CASE(Stress, FolderWatcherRemoveWithPendingCompletion) {
    const auto& config = GetGlobalStressConfig();

    for (uint32_t iter = 0; iter < config.iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 4093 + 4;
        TestWatchdog watchdog(
            config.defaultTimeoutMs,
            "Stress.FolderWatcherRemoveWithPendingCompletion",
            iterSeed,
            iter);
        ScopedTempDir tempDir(L"Aperture_Stress_FW_Remove_");
        const std::wstring watchRoot = tempDir.GetPath();

        FolderWatcher watcher;
        watcher.SetCoalesceWindowMs(1);
        TEST_ASSERT(watcher.Start());

        constexpr uint32_t kCycles = 30;
        for (uint32_t cycle = 0; cycle < kCycles; ++cycle) {
            TEST_ASSERT(watcher.AddWatch(watchRoot));
            TEST_ASSERT(tempDir.WriteFile(
                L"pending_" + std::to_wstring(cycle) + L".mp4",
                "pending completion"));
            TEST_ASSERT(watcher.RemoveWatch(watchRoot));
        }

        TEST_ASSERT(watcher.AddWatch(watchRoot));
        TEST_ASSERT(tempDir.WriteFile(L"clear_pending.mp4", "pending completion"));
        watcher.ClearWatches();
        watcher.Stop();
        TEST_ASSERT(!watcher.IsRunning());
        TEST_ASSERT(watcher.GetWatchedFolders().empty());
    }
}

} // namespace Aperture::Test::Stress
