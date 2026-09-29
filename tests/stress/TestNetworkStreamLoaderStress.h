#pragma once

#include "StressHarness.h"
#include "NetworkStream.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, NetworkStreamLoaderDestructionDuringOpen) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 16183 + 16;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.NetworkStreamLoaderDestructionDuringOpen", iterSeed, iter);

        CallbackLifecycleFence fence;
        std::atomic<uint32_t> callbacksFired{0};

        constexpr uint32_t kCycles = 15;
        for (uint32_t c = 0; c < kCycles; ++c) {
            auto loader = std::make_unique<NetworkStreamLoader>();

            std::wstring url = L"https://stream.example.com/live/video_" + std::to_wstring(c) + L".m3u8";

            uint64_t gen = loader->BeginOpen(url, nullptr,
                [&fence, &callbacksFired](bool, const std::wstring&, const std::wstring&, uint64_t) {
                    fence.OnCallbackInvoked("NetworkStreamLoader.onComplete");
                    callbacksFired++;
                });

            TEST_ASSERT(gen > 0);

            // Seeded delay before destruction
            uint32_t delayMs = sched.NextDelayMs(0, 10);
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            fence.SetShutdown();
            // Destroy loader immediately while worker thread may be running
            loader.reset();
            fence.Reset();
        }
    }
}

TEST_CASE(Stress, NetworkStreamLoaderRapidCancelCycles) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 17191 + 17;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.NetworkStreamLoaderRapidCancelCycles", iterSeed, iter);

        NetworkStreamLoader loader;
        std::atomic<uint32_t> completedCallbacks{0};

        constexpr uint32_t kCycles = 20;
        for (uint32_t c = 0; c < kCycles; ++c) {
            std::wstring url = L"https://live.test.local/stream_" + std::to_wstring(c) + L".mpd";

            loader.BeginOpen(url, nullptr,
                [&completedCallbacks](bool, const std::wstring&, const std::wstring&, uint64_t) {
                    completedCallbacks++;
                });

            uint32_t delayMs = sched.NextDelayMs(0, 5);
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            loader.Cancel();
            TEST_ASSERT(!loader.IsOpening());
        }
    }
}

TEST_CASE(Stress, NetworkStreamModelConcurrentAsyncOpenAndCancel) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 18211 + 19;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.NetworkStreamModelConcurrentAsyncOpenAndCancel", iterSeed, iter);

        NetworkStreamModel model;
        std::atomic<bool> stopRunning{false};
        std::atomic<uint32_t> callbacksCompleted{0};

        // UI projection / query thread
        std::thread uiReader([&model, &stopRunning]() {
            while (!stopRunning.load(std::memory_order_relaxed)) {
                NetworkStreamProjection proj = model.GetProjection();
                std::wstring err = model.GetLastError();
                bool opening = model.IsOpening();
                (void)proj;
                (void)err;
                (void)opening;
                std::this_thread::yield();
            }
        });

        constexpr uint32_t kCycles = 20;
        for (uint32_t c = 0; c < kCycles; ++c) {
            std::wstring url = (c % 2 == 0)
                ? (L"https://stream.example.com/live/segment_" + std::to_wstring(c) + L".m3u8")
                : (L"invalid_url_" + std::to_wstring(c));

            model.SetInput(url);

            model.OpenStreamAsync(nullptr,
                [&callbacksCompleted](bool, const std::wstring&, const std::wstring&) {
                    callbacksCompleted.fetch_add(1, std::memory_order_relaxed);
                });

            uint32_t delayMs = sched.NextDelayMs(0, 4);
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            if (c % 3 == 0) {
                model.CancelOpen();
            } else if (c % 3 == 1) {
                model.Close();
            }
        }

        stopRunning.store(true, std::memory_order_release);
        if (uiReader.joinable()) {
            uiReader.join();
        }
        model.CancelOpen();
    }
}

} // namespace Aperture::Test::Stress
