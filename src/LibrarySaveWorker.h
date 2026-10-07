#pragma once

#include "MediaLibrary.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <thread>

namespace Aperture {

// UI-owned admission control: coalesce before capturing a potentially large
// immutable snapshot. A continuous edit stream still saves every 500 ms.
class LibrarySaveCaptureSchedule {
public:
    void Request(uint64_t nowMs, bool newBatch = false) {
        if (!pending_ || newBatch) firstRequestMs_ = nowMs;
        lastRequestMs_ = nowMs; pending_ = true;
    }
    bool IsDue(uint64_t nowMs) const {
        return pending_ && (nowMs - lastRequestMs_ >= 100 || nowMs - firstRequestMs_ >= 500);
    }
    void Complete() { pending_ = false; }
private:
    uint64_t firstRequestMs_ = 0, lastRequestMs_ = 0;
    bool pending_ = false;
};

// One owned writer and one replaceable pending snapshot. No worker accesses
// the live model or calls the UI; completion is consumed by the owner.
class LibrarySaveWorker {
public:
    using Writer = std::function<MediaLibrarySaveResult(const MediaLibrarySaveSnapshot&, MediaLibrarySaveState&)>;
    explicit LibrarySaveWorker(Writer writer = WriteLibrarySaveSnapshot) : writer_(std::move(writer)) {}
    ~LibrarySaveWorker() { StopAndFlush(); }
    LibrarySaveWorker(const LibrarySaveWorker&) = delete;
    LibrarySaveWorker& operator=(const LibrarySaveWorker&) = delete;

    uint64_t Submit(std::shared_ptr<const MediaLibrarySaveSnapshot> snapshot) {
        if (!snapshot) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return 0;
        if (pending_ == snapshot) return pendingSerial_;
        if (!pending_ && active_ == snapshot) return activeSerial_;
        const uint64_t serial = ++submitted_;
        pending_ = std::move(snapshot); pendingSerial_ = serial;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        if (!thread_.joinable()) {
            try { thread_ = std::thread([this] { Run(); }); }
            catch (...) { pending_.reset(); --submitted_; throw; }
        }
        wake_.notify_all();
        return serial;
    }

    uint64_t RequestFlush() {
        std::lock_guard<std::mutex> lock(mutex_);
        flushThrough_ = submitted_;
        wake_.notify_all();
        return submitted_;
    }
    bool WaitUntil(uint64_t serial, const std::atomic<bool>* cancel = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (completed_ < serial) {
            if (cancel && cancel->load(std::memory_order_acquire)) return false;
            done_.wait_for(lock, std::chrono::milliseconds(20));
        }
        return !cancel || !cancel->load(std::memory_order_acquire);
    }
    bool IsIdle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !pending_ && !active_;
    }
    bool IsComplete(uint64_t serial) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_ >= serial;
    }
    bool WasSuccessful(uint64_t serial) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return serial == 0 || (completed_ >= serial && lastSucceeded_);
    }
    std::vector<MediaLibrarySaveResult> TakeResults() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MediaLibrarySaveResult> results;
        results.swap(results_);
        return results;
    }
    bool TakeDroppedFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool dropped = droppedFailure_;
        droppedFailure_ = false;
        return dropped;
    }
    void StopAndFlush() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true; flushThrough_ = submitted_; wake_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stopping_ || pending_; });
            if (!pending_ && stopping_) return;
            while (!stopping_ && flushThrough_ < pendingSerial_ && std::chrono::steady_clock::now() < deadline_) {
                wake_.wait_until(lock, deadline_);
            }
            active_ = std::move(pending_); activeSerial_ = pendingSerial_;
            const auto snapshot = active_; const uint64_t serial = activeSerial_;
            lock.unlock();
            MediaLibrarySaveResult result;
            try { result = writer_(*snapshot, state_); }
            catch (...) {
                result.success = false;
                // Publish a failure even when reporting the original exception
                // cannot allocate. Optional text never controls completion.
                for (StorageStatus* status : { &result.libraryStatus, &result.playlistsStatus,
                        &result.historyStatus, &result.watchedFoldersStatus }) {
                    status->success = false;
                    status->code = StorageErrorCode::SerializationFailed;
                    status->win32Error = 0;
                }
                try { throw; }
                catch (const std::bad_alloc&) {
                    result.libraryStatus.win32Error = ERROR_NOT_ENOUGH_MEMORY;
                } catch (...) {}
                try {
                    result.libraryStatus.path = snapshot->rootDir;
                    result.libraryStatus.message = "Background save failed";
                } catch (...) {}
            }
            lock.lock();
            lastSucceeded_ = result.is_ok();
            // Keep failed results until consumed; redundant successful completions
            // need only one slot even when the UI is temporarily occupied.
            try {
                if (lastSucceeded_ && !results_.empty() && results_.back().is_ok()) results_.back() = std::move(result);
                else results_.push_back(std::move(result));
            } catch (...) {
                // Actual write success is retained independently of the
                // diagnostic queue. Failed writes remain observable without
                // allocating a result or terminating the owning thread.
                droppedFailure_ |= !lastSucceeded_;
            }
            active_.reset(); completed_ = serial; done_.notify_all();
        }
    }
    Writer writer_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::thread thread_;
    std::shared_ptr<const MediaLibrarySaveSnapshot> pending_, active_;
    uint64_t submitted_ = 0, pendingSerial_ = 0, activeSerial_ = 0, completed_ = 0, flushThrough_ = 0;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point deadline_;
    MediaLibrarySaveState state_;
    bool lastSucceeded_ = true, droppedFailure_ = false;
    std::vector<MediaLibrarySaveResult> results_;
};

} // namespace Aperture
