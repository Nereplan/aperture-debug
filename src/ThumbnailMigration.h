#pragma once

#include "ThumbnailCodec.h"
#include "CacheStorage.h"
#include <condition_variable>
#include <deque>
#include <thread>
#include <set>
#include <functional>

namespace Aperture {

// One bounded worker. Conversion never blocks the render thread and never
// reopens media. Existing cache bytes are the sole input to the conversion.
class ThumbnailMigrationQueue {
public:
    ~ThumbnailMigrationQueue() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; queue_.clear(); }
        ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    bool Queue(const std::wstring& root, const std::wstring& key, const std::string& bytes) {
        if (bytes.size() < sizeof(ThumbnailDiskHeader) || bytes.size() > 8 * 1024 * 1024) return false;
        ThumbnailDiskHeader header{}; std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.version != 1 && header.version != 2) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        auto identity = root + L"\\" + key;
        if (pending_.count(identity)) return true;
        const auto remembered = std::find_if(optimal_.begin(), optimal_.end(),
            [&](const Optimal& entry) { return entry.identity == identity; });
        if (remembered != optimal_.end()) {
            const bool unchanged = remembered->bytes == bytes.size() && remembered->hash == std::hash<std::string>{}(bytes);
            if (unchanged) {
                auto recent = std::move(*remembered);
                optimal_.erase(remembered); optimal_.push_back(std::move(recent));
                return false;
            }
            optimal_.erase(remembered);
        }
        if (queue_.size() >= 32 || queuedBytes_ + bytes.size() > 8 * 1024 * 1024) return false;
        if (!worker_.joinable()) worker_ = std::thread([this]() { Run(); });
        queue_.push_back({root, key, bytes}); pending_.insert(identity); queuedBytes_ += bytes.size();
        ready_.notify_one();
        return true;
    }
    void WaitIdle() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this]() { return queue_.empty() && !active_; });
    }
private:
    struct Item { std::wstring root, key; std::string bytes; };
    struct Optimal { std::wstring identity; size_t bytes, hash; };
    static constexpr size_t kRememberedLimit = 128;
    void RememberOptimal(const std::wstring& root, const std::wstring& key, const std::string& bytes) {
        Optimal entry{root + LR"(\)" + key, bytes.size(), std::hash<std::string>{}(bytes)};
        std::lock_guard<std::mutex> lock(mutex_);
        if (optimal_.size() == kRememberedLimit) optimal_.pop_front();
        optimal_.push_back(std::move(entry));
    }
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                item = std::move(queue_.front()); queue_.pop_front(); queuedBytes_ -= item.bytes.size(); active_ = true;
            }
            try {
                std::string smaller;
                const auto result = ThumbnailCodec::TryUpgrade(item.bytes, smaller);
                if (result == ThumbnailCodec::UpgradeResult::Converted) {
                    if (AtomicWriteCacheFile(CacheClass::Thumbnails, item.key, smaller, GetGlobalCacheBudgets(), item.root, &item.bytes).is_ok()) {
                        ThumbnailDiskHeader header{};
                        std::memcpy(&header, smaller.data(), sizeof(header));
                        if (header.version == 2) RememberOptimal(item.root, item.key, smaller);
                    }
                } else if (result == ThumbnailCodec::UpgradeResult::AlreadyOptimal) {
                    RememberOptimal(item.root, item.key, item.bytes);
                }
            } catch (...) { /* Disposable cache: keep the old entry and allow a later retry. */ }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending_.erase(item.root + L"\\" + item.key); active_ = false;
                if (queue_.empty()) idle_.notify_all();
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<Item> queue_;
    // Content fingerprints only: bounded RAM, no duplicate pixels or disk metadata.
    std::deque<Optimal> optimal_;
    std::set<std::wstring> pending_;
    std::thread worker_;
    size_t queuedBytes_ = 0;
    bool active_ = false, stopping_ = false;
};

inline ThumbnailMigrationQueue& ThumbnailMigrations() {
    // Dependencies must outlive the worker during static teardown.
    (void)GetGlobalCacheBudgets(); (void)Detail::CacheIOMutex(); (void)Detail::CacheIndexes(); (void)GetExeDir();
    static ThumbnailMigrationQueue worker;
    return worker;
}
} // namespace Aperture
