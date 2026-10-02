#pragma once

#include "OperationCancellation.h"
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Aperture {

// One owned worker, one replaceable pending request, and one completion. Callers
// pass value snapshots, never UI references. Replacing/cancelling a request makes
// its completion ineligible even if an injected operation ignores cancellation.
template<class Result>
class AsyncOwnedJob {
public:
    using Work = std::function<Result(const std::shared_ptr<OperationCancellation>&)>;
    struct Completion {
        uint64_t generation = 0;
        std::optional<Result> result;
        std::string error;
    };
    ~AsyncOwnedJob() { StopAndJoin(); }
    AsyncOwnedJob() = default;
    AsyncOwnedJob(const AsyncOwnedJob&) = delete;
    AsyncOwnedJob& operator=(const AsyncOwnedJob&) = delete;

    uint64_t Start(Work work) {
        std::shared_ptr<OperationCancellation> previous;
        uint64_t generation;
        { std::lock_guard<std::mutex> lock(mutex_);
          if (stopping_) return 0;
          generation = ++generation_;
          previous = activeCancellation_;
          pending_ = Request{generation, std::move(work), std::make_shared<OperationCancellation>()};
          completion_.reset();
          if (!worker_.joinable()) {
              try { worker_ = std::thread([this] { Run(); }); }
              catch (const std::exception& error) {
                  pending_.reset(); completion_ = Completion{generation, {}, error.what()};
              }
          } }
        if (previous) previous->Cancel();
        wake_.notify_one();
        return generation;
    }
    void Cancel() {
        std::shared_ptr<OperationCancellation> active;
        { std::lock_guard<std::mutex> lock(mutex_);
          ++generation_; pending_.reset(); completion_.reset(); active = activeCancellation_; }
        if (active) active->Cancel();
    }
    bool IsRunning() const {
        std::lock_guard<std::mutex> lock(mutex_); return active_ || pending_.has_value();
    }
    std::optional<Completion> TakeResult() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = std::move(completion_); completion_.reset(); return result;
    }
    // The App close pump calls Cancel, keeps dispatching messages until
    // IsRunning is false, and only then destroys/joins the job.
    void StopAndJoin() {
        Cancel();
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
private:
    struct Request { uint64_t generation; Work work; std::shared_ptr<OperationCancellation> cancellation; };
    void Run() {
        for (;;) {
            Request request;
            { std::unique_lock<std::mutex> lock(mutex_);
              wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
              if (stopping_) return;
              request = std::move(*pending_); pending_.reset();
              active_ = true; activeCancellation_ = request.cancellation; }
            Completion result; result.generation = request.generation;
            try { result.result.emplace(request.work(request.cancellation)); }
            catch (const std::exception& error) { result.error = error.what(); }
            catch (...) { result.error = "Background operation failed"; }
            { std::lock_guard<std::mutex> lock(mutex_);
              if (!stopping_ && request.generation == generation_ && !request.cancellation->IsCancelled())
                  completion_ = std::move(result);
              activeCancellation_.reset(); active_ = false; }
        }
    }
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    uint64_t generation_ = 0;
    bool stopping_ = false, active_ = false;
    std::optional<Request> pending_;
    std::optional<Completion> completion_;
    std::shared_ptr<OperationCancellation> activeCancellation_;
};

} // namespace Aperture
