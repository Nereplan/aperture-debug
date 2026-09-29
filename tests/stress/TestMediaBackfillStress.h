#pragma once

#include "StressHarness.h"
#include "MediaLibrary.h"
#include "FolderWatcher.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, MediaLibraryBackfillCancellationStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 4001 + 4;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.MediaLibraryBackfillCancellationStress", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Backfill_");

        MediaLibrary library;
        constexpr size_t kItemCount = 50;

        // Populate library with synthetic media items needing backfill
        for (size_t i = 0; i < kItemCount; ++i) {
            std::wstring rel = L"video_" + std::to_wstring(i) + L".mp4";
            std::string content = "synthetic_media_content_" + std::to_string(i);
            tempDir.WriteFile(rel, content);

            MediaItem item;
            item.id = "id_" + std::to_string(i);
            item.filePath = tempDir.SubPath(rel);
            item.title = L"Video " + std::to_wstring(i);
            item.durationSec = 0.0; // Needs backfill
            item.metadataProbeFailed = false;
            library.mediaItems.push_back(std::move(item));
        }
        library.RebuildPathIndex();

        std::atomic<bool> cancelFlag{false};
        std::atomic<uint32_t> progressCalls{0};
        std::atomic<bool> threadFinished{false};
        MetadataBackfillResult backfillResult{};

        MetadataBackfillOptions options;
        options.maxItems = kItemCount;
        options.forceRetry = true;

        std::thread worker([&library, options, &cancelFlag, &progressCalls, &backfillResult, &threadFinished]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto progress = [&cancelFlag, &progressCalls](size_t, size_t, const std::wstring&) {
                progressCalls++;
                // Injected small delay to guarantee in-flight cancellation observation
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return !cancelFlag.load(std::memory_order_acquire);
            };

            backfillResult = library.BackfillMissingMetadata(options, progress, false);
            threadFinished.store(true, std::memory_order_release);
            CoUninitialize();
        });

        // Seeded delay before requesting cancellation
        uint32_t delayMs = sched.NextDelayMs(2, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        cancelFlag.store(true, std::memory_order_release);

        if (worker.joinable()) {
            worker.join();
        }

        TEST_ASSERT(threadFinished.load());
        TEST_ASSERT(progressCalls.load() > 0);
        // Operation outcome must be either Completed or Canceled
        TEST_ASSERT(backfillResult.outcome == LongOperationOutcome::Completed ||
                    backfillResult.outcome == LongOperationOutcome::Canceled);
    }
}

TEST_CASE(Stress, MediaLibraryImportCancellationStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 5011 + 5;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.MediaLibraryImportCancellationStress", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Import_");

        std::vector<std::wstring> importTargets;
        constexpr size_t kFileCount = 40;
        for (size_t i = 0; i < kFileCount; ++i) {
            std::wstring subName = L"sub_" + std::to_wstring(i % 4);
            tempDir.CreateSubDir(subName);
            std::wstring filePath = subName + L"\\import_" + std::to_wstring(i) + L".mkv";
            tempDir.WriteFile(filePath, "sample file data");
            importTargets.push_back(tempDir.SubPath(filePath));
        }

        std::atomic<bool> cancelRequested{false};
        std::atomic<uint32_t> progressCount{0};
        MediaImportResult result{};

        std::thread importThread([&tempDir, &cancelRequested, &progressCount, &result]() {
            MediaLibrary lib;
            auto progress = [&cancelRequested, &progressCount](size_t, size_t, const std::wstring&) {
                progressCount++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return !cancelRequested.load(std::memory_order_acquire);
            };

            std::vector<std::wstring> roots = { tempDir.GetPath() };
            result = lib.ImportPaths(roots, progress);
        });

        uint32_t delayMs = sched.NextDelayMs(1, 15);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        cancelRequested.store(true, std::memory_order_release);

        if (importThread.joinable()) {
            importThread.join();
        }

        TEST_ASSERT(result.outcome == LongOperationOutcome::Completed ||
                    result.outcome == LongOperationOutcome::Canceled);
    }
}

TEST_CASE(Stress, MediaLibraryConcurrentWatchReconciliation) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 6029 + 6;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.MediaLibraryConcurrentWatchReconciliation", iterSeed, iter);

        MediaLibrary library;
        std::mutex libMutex;
        std::atomic<bool> stopWorkers{false};
        std::atomic<uint32_t> batchesProcessed{0};

        constexpr size_t kThreads = 3;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([t, &library, &libMutex, &stopWorkers, &batchesProcessed]() {
                uint32_t itemIdx = 0;
                while (!stopWorkers.load(std::memory_order_relaxed)) {
                    WatchChangeBatch batch;
                    batch.rootFolder = L"C:\\MediaRoot";
                    for (int j = 0; j < 5; ++j) {
                        WatchChangeItem item;
                        item.action = (j % 2 == 0) ? WatchAction::Added : WatchAction::Modified;
                        item.path = L"C:\\MediaRoot\\t_" + std::to_wstring(t) + L"_" + std::to_wstring(itemIdx++) + L".mp4";
                        item.rootFolder = batch.rootFolder;
                        batch.items.push_back(std::move(item));
                    }

                    {
                        std::lock_guard<std::mutex> lock(libMutex);
                        library.ReconcileWatchBatch(batch, true);
                    }
                    batchesProcessed++;
                    std::this_thread::yield();
                }
            });
        }

        uint32_t delayMs = sched.NextDelayMs(5, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        stopWorkers.store(true, std::memory_order_release);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        TEST_ASSERT(batchesProcessed.load() > 0);
    }
}

} // namespace Aperture::Test::Stress
