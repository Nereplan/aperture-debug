#pragma once

#include <atomic>
#include <functional>
#include <mutex>

namespace Aperture {

// A job owns this token. Blocking operations may register one short interrupt
// action; cancellation never waits for the worker or calls application objects.
class OperationCancellation {
public:
    bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void Cancel() {
        cancelled_.store(true, std::memory_order_release);
        std::function<void()> interrupt;
        { std::lock_guard<std::mutex> lock(mutex_); interrupt = std::move(interrupt_); }
        if (interrupt) interrupt();
    }
    void SetInterrupt(std::function<void()> interrupt) {
        bool cancelled;
        { std::lock_guard<std::mutex> lock(mutex_);
          cancelled = IsCancelled();
          if (!cancelled) interrupt_ = std::move(interrupt); }
        if (cancelled && interrupt) interrupt();
    }
    void ClearInterrupt() {
        std::lock_guard<std::mutex> lock(mutex_); interrupt_ = {};
    }
private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::function<void()> interrupt_;
};

} // namespace Aperture
