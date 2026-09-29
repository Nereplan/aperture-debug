#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <random>
#include <functional>
#include <sstream>
#include <thread>
#include <future>
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"

namespace Aperture::Test::Stress {

struct StressConfig {
    uint32_t iterations = 10;
    uint64_t seed = 0x578E55; // Default deterministic seed
    bool isSoak = false;
    uint32_t defaultTimeoutMs = 15000; // 15 seconds per stress test
};

inline StressConfig& GetGlobalStressConfig() {
    static StressConfig config;
    return config;
}

class DeterministicSchedule {
public:
    explicit DeterministicSchedule(uint64_t seed)
        : rng_(seed), seed_(seed) {}

    uint64_t GetSeed() const { return seed_; }

    // Returns a delay in milliseconds uniformly distributed in [minMs, maxMs]
    uint32_t NextDelayMs(uint32_t minMs, uint32_t maxMs) {
        std::uniform_int_distribution<uint32_t> dist(minMs, maxMs);
        return dist(rng_);
    }

    // Returns a random boolean with specified true probability (0.0 to 1.0)
    bool NextBool(double trueProbability = 0.5) {
        std::bernoulli_distribution dist(trueProbability);
        return dist(rng_);
    }

    // Selects an index from [0, size - 1]
    size_t NextIndex(size_t size) {
        if (size == 0) return 0;
        std::uniform_int_distribution<size_t> dist(0, size - 1);
        return dist(rng_);
    }

    // Shuffles a container deterministically
    template <typename T>
    void Shuffle(std::vector<T>& vec) {
        std::shuffle(vec.begin(), vec.end(), rng_);
    }

private:
    std::mt19937_64 rng_;
    uint64_t seed_;
};

// Snapshot of process thread count and handle count to detect leaks
struct ResourceSnapshot {
    DWORD threadCount = 0;
    DWORD handleCount = 0;

    static ResourceSnapshot Capture() {
        ResourceSnapshot snap{};
        DWORD handleCount = 0;
        if (GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
            snap.handleCount = handleCount;
        }

        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(THREADENTRY32);
            DWORD pid = GetCurrentProcessId();
            if (Thread32First(hSnapshot, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid) {
                        snap.threadCount++;
                    }
                } while (Thread32Next(hSnapshot, &te));
            }
            CloseHandle(hSnapshot);
        }
        return snap;
    }
};

// Per-test watchdog ensuring bounded execution time
class TestWatchdog {
public:
    explicit TestWatchdog(uint32_t timeoutMs, const std::string& testName, uint64_t seed, uint32_t iteration)
        : timeoutMs_(timeoutMs), testName_(testName), seed_(seed), iteration_(iteration), done_(false)
    {
        worker_ = std::thread([this]() {
            auto start = std::chrono::steady_clock::now();
            while (!done_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeoutMs_ && !done_.load(std::memory_order_acquire)) {
                    std::cerr << "\n[STRESS WATCHDOG TIMEOUT] Test: " << testName_
                              << " | Seed: 0x" << std::hex << seed_ << std::dec
                              << " | Iteration: " << iteration_
                              << " exceeded deadline of " << timeoutMs_ << " ms! Failing closed.\n" << std::flush;
                    // Record failure in TestRegistry
                    TestRegistry::Instance().AddFailure(
                        testName_, __FILE__, __LINE__,
                        "Watchdog deadline exceeded",
                        "Operation did not complete in bounded time (potential deadlock or hang)");
                    break;
                }
            }
        });
    }

    ~TestWatchdog() {
        done_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    uint32_t timeoutMs_;
    std::string testName_;
    uint64_t seed_;
    uint32_t iteration_;
    std::atomic<bool> done_;
    std::thread worker_;
};

// Tracks callback invocations and asserts no callbacks occur after owner shutdown/destruction
class CallbackLifecycleFence {
public:
    CallbackLifecycleFence() = default;

    void SetShutdown() {
        shutdownTriggered_.store(true, std::memory_order_release);
    }

    bool IsShutdown() const {
        return shutdownTriggered_.load(std::memory_order_acquire);
    }

    void OnCallbackInvoked(const std::string& callbackName = "unnamed") {
        totalCallbacks_++;
        if (shutdownTriggered_.load(std::memory_order_acquire)) {
            postShutdownViolations_++;
            std::lock_guard<std::mutex> lock(violationMutex_);
            violationDetails_.push_back("Post-shutdown callback violation: " + callbackName);
        }
    }

    uint32_t GetTotalCallbacks() const { return totalCallbacks_.load(); }
    uint32_t GetPostShutdownViolations() const { return postShutdownViolations_.load(); }

    const std::vector<std::string>& GetViolationDetails() {
        std::lock_guard<std::mutex> lock(violationMutex_);
        return violationDetails_;
    }

    void Reset() {
        shutdownTriggered_.store(false);
        totalCallbacks_.store(0);
        postShutdownViolations_.store(0);
        std::lock_guard<std::mutex> lock(violationMutex_);
        violationDetails_.clear();
    }

private:
    std::atomic<bool> shutdownTriggered_{false};
    std::atomic<uint32_t> totalCallbacks_{0};
    std::atomic<uint32_t> postShutdownViolations_{0};
    std::mutex violationMutex_;
    std::vector<std::string> violationDetails_;
};

} // namespace Aperture::Test::Stress
