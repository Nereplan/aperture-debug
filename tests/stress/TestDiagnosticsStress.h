#pragma once

#include "StressHarness.h"
#include "Diagnostics.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, DiagnosticsRapidLifecycleCycles) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 14143 + 14;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.DiagnosticsRapidLifecycleCycles", iterSeed, iter);

        constexpr uint32_t kCycles = 10;
        for (uint32_t c = 0; c < kCycles; ++c) {
            TEST_ASSERT(Diagnostics::Initialize(true));
            Diagnostics::RecordHeartbeat(L"stress_cycle_" + std::to_wstring(c));

            AppError err = AppError::Playback(L"Sample diagnostic error log " + std::to_wstring(c));
            Diagnostics::Log(err);

            uint32_t delayMs = sched.NextDelayMs(0, 5);
            if (delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            Diagnostics::Shutdown();
        }
    }
}

TEST_CASE(Stress, DiagnosticsConcurrentLoggingAndShutdown) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 15149 + 15;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.DiagnosticsConcurrentLoggingAndShutdown", iterSeed, iter);

        TEST_ASSERT(Diagnostics::Initialize(true));

        std::atomic<bool> stopLogging{false};
        std::atomic<bool> startLogging{false};
        std::atomic<uint32_t> logsWritten{0};
        std::atomic<uint32_t> loggersReady{0};

        constexpr size_t kThreads = 4;
        std::vector<std::thread> loggers;
        loggers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            loggers.emplace_back([t, &stopLogging, &startLogging, &logsWritten, &loggersReady]() {
                uint32_t count = 0;
                loggersReady.fetch_add(1, std::memory_order_release);
                while (!startLogging.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                AppError first = AppError::Playback(L"Thread " + std::to_wstring(t) + L" initial log message");
                Diagnostics::Log(first);
                Diagnostics::RecordHeartbeat(L"active");
                logsWritten.fetch_add(1, std::memory_order_relaxed);
                while (!stopLogging.load(std::memory_order_relaxed)) {
                    AppError err = AppError::Playback(L"Thread " + std::to_wstring(t) + L" log message " + std::to_wstring(count++));
                    Diagnostics::Log(err);
                    Diagnostics::RecordHeartbeat(L"active");
                    logsWritten.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            });
        }

        const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (loggersReady.load(std::memory_order_acquire) < kThreads &&
               std::chrono::steady_clock::now() < readyDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(loggersReady.load(std::memory_order_acquire) == kThreads);

        startLogging.store(true, std::memory_order_release);
        const auto loggingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (logsWritten.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < loggingDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        TEST_ASSERT(logsWritten.load(std::memory_order_acquire) > 0);

        uint32_t delayMs = sched.NextDelayMs(5, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        // Trigger shutdown while loggers are active
        Diagnostics::Shutdown();

        stopLogging.store(true, std::memory_order_release);
        for (auto& l : loggers) {
            if (l.joinable()) l.join();
        }

        TEST_ASSERT(logsWritten.load() > 0);
    }
}

} // namespace Aperture::Test::Stress
