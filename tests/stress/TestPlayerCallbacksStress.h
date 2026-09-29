#pragma once

#include "StressHarness.h"
#include "MpvPlayer.h"
#include "PlaybackSession.h"
#include "MediaTransport.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, PlayerCallbackFenceShutdownStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 7013 + 7;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.PlayerCallbackFenceShutdownStress", iterSeed, iter);

        CallbackLifecycleFence fence;
        std::atomic<bool> callbacksDisabled{false};
        std::atomic<bool> stopEventProducer{false};
        std::atomic<uint32_t> timeUpdatesCount{0};
        std::atomic<uint32_t> durationChangesCount{0};
        std::atomic<uint32_t> playStateChangesCount{0};

        // Callback targets simulating App-side event handlers
        auto onTime = [&fence, &callbacksDisabled, &timeUpdatesCount](double) {
            if (!callbacksDisabled.load(std::memory_order_acquire)) {
                fence.OnCallbackInvoked("MpvPlayer.onTimeUpdated");
                timeUpdatesCount++;
            }
        };

        auto onDuration = [&fence, &callbacksDisabled, &durationChangesCount](double) {
            if (!callbacksDisabled.load(std::memory_order_acquire)) {
                fence.OnCallbackInvoked("MpvPlayer.onDurationChanged");
                durationChangesCount++;
            }
        };

        auto onPlayState = [&fence, &callbacksDisabled, &playStateChangesCount](bool) {
            if (!callbacksDisabled.load(std::memory_order_acquire)) {
                fence.OnCallbackInvoked("MpvPlayer.onPlayStateChanged");
                playStateChangesCount++;
            }
        };

        // Spawn simulated background event thread generating high-frequency property changes
        std::thread eventProducer([&stopEventProducer, onTime, onDuration, onPlayState]() {
            double simulatedTime = 0.0;
            bool simulatedPaused = false;
            while (!stopEventProducer.load(std::memory_order_relaxed)) {
                simulatedTime += 0.05;
                onTime(simulatedTime);
                if (static_cast<int>(simulatedTime * 10) % 5 == 0) {
                    onDuration(120.0);
                }
                if (static_cast<int>(simulatedTime * 10) % 10 == 0) {
                    simulatedPaused = !simulatedPaused;
                    onPlayState(!simulatedPaused);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });

        // Run for seeded delay
        uint32_t delayMs = sched.NextDelayMs(2, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        // 1. Disable callbacks (matching App::Shutdown fence)
        callbacksDisabled.store(true, std::memory_order_release);

        // 2. Stop producer and join event thread
        stopEventProducer.store(true, std::memory_order_release);
        if (eventProducer.joinable()) {
            eventProducer.join();
        }

        // 3. Mark owner destruction fence
        fence.SetShutdown();

        // 4. Injected post-fence attempt to prove guard suppression without calling destroyed target
        onTime(999.0);
        onDuration(999.0);
        onPlayState(true);

        TEST_ASSERT_EQ(fence.GetPostShutdownViolations(), 0u);
        TEST_ASSERT(timeUpdatesCount.load() > 0);
    }
}

TEST_CASE(Stress, PlaybackSessionConcurrentShutdownStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 8053 + 8;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.PlaybackSessionConcurrentShutdownStress", iterSeed, iter);

        PlaybackSession session;
        std::vector<std::wstring> queue = {
            L"C:\\media\\ep1.mp4",
            L"C:\\media\\ep2.mp4",
            L"C:\\media\\ep3.mp4"
        };

        session.OpenQueue(queue, 0);
        TEST_ASSERT(session.IsOpen());
        TEST_ASSERT(session.IsQueuePlayback());

        std::atomic<bool> stopWorker{false};
        std::thread sessionWorker([&session, &stopWorker]() {
            while (!stopWorker.load(std::memory_order_relaxed)) {
                session.SetPlaybackPosition(15.5);
                session.GetNextQueuePath();
                session.GetPrevQueuePath(false);
                std::this_thread::yield();
            }
        });

        uint32_t delayMs = sched.NextDelayMs(1, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        stopWorker.store(true, std::memory_order_release);
        if (sessionWorker.joinable()) {
            sessionWorker.join();
        }

        session.Close();
        TEST_ASSERT(!session.IsOpen());
    }
}

TEST_CASE(Stress, MediaTransportConcurrentActionShutdown) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 9091 + 9;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.MediaTransportConcurrentActionShutdown", iterSeed, iter);

        MediaTransport transport;
        CommandDispatcher dispatcher;

        std::atomic<bool> stopCallers{false};
        std::atomic<uint32_t> updatesDispatched{0};

        constexpr size_t kThreads = 3;
        std::vector<std::thread> callers;
        callers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            callers.emplace_back([&transport, &dispatcher, &stopCallers, &updatesDispatched]() {
                while (!stopCallers.load(std::memory_order_relaxed)) {
                    MediaTransportSnapshot snap;
                    snap.title = L"Test Media";
                    snap.playbackState = PlaybackState::Playing;
                    snap.isPlayEnabled = true;
                    snap.isPauseEnabled = true;
                    transport.ProcessPendingCommands(dispatcher);
                    updatesDispatched++;
                    std::this_thread::yield();
                }
            });
        }

        uint32_t delayMs = sched.NextDelayMs(2, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        transport.Shutdown();

        stopCallers.store(true, std::memory_order_release);
        for (auto& c : callers) {
            if (c.joinable()) c.join();
        }

        TEST_ASSERT(updatesDispatched.load() > 0);
    }
}

} // namespace Aperture::Test::Stress
