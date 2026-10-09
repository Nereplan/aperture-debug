#pragma once

#include "AsyncOwnedJob.h"
#include "Win32IoCancellation.h"
#include <array>
#include <deque>
#include <unordered_map>

namespace Aperture {

// The cache and demand queue belong to the UI. Workers receive only bounded
// path batches; lookups never take a worker lock or query the filesystem.
class BackgroundPathAvailability {
public:
    using Probe = std::function<PathAvailabilityObservation(const std::wstring&, const std::shared_ptr<OperationCancellation>&)>;
    static constexpr ULONGLONG kTtlMs = 2000;
    static constexpr size_t kBatchSize = 32, kExtraPaths = 4096;
    explicit BackgroundPathAvailability(Probe probe = {}) : probe_(std::move(probe)) {
        if (!probe_) probe_ = [](const auto& path, const auto&) {
            DWORD attributes = INVALID_FILE_ATTRIBUTES;
            const auto value = QueryPathAvailability(path, nullptr, &attributes);
            return PathAvailabilityObservation{value, 0, true,
                attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
        };
    }
    ~BackgroundPathAvailability() { StopAndJoin(); }
    BackgroundPathAvailability(const BackgroundPathAvailability&) = delete;
    BackgroundPathAvailability& operator=(const BackgroundPathAvailability&) = delete;

    void SetLibrarySize(size_t count) { capacity_ = count + kExtraPaths; }
    PathAvailabilityObservation Lookup(const std::wstring& path, ULONGLONG now = GetTickCount64()) {
        if (path.empty() || stopped_) return {};
        auto key = NormalizePathKey(path);
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            if (entries_.size() >= capacity_ && !EvictOne()) return {};
            auto inserted = entries_.try_emplace(std::move(key)).first;
            try {
                inserted->second.path = path;
                residentOrder_.push_back(&inserted->first);
            } catch (...) { entries_.erase(inserted); throw; }
            found = inserted;
        }
        auto& entry = found->second;
        if (!entry.pending && now >= entry.retryAtTick &&
            (!entry.observation.known || now - entry.observation.checkedAtTick >= kTtlMs)) {
            pending_.push_back(&found->first); entry.pending = true;
        }
        // Keep the previous observation while refreshing. An unknown path is
        // not reported as missing merely because its check has not finished.
        return entry.observation;
    }

    bool Pump(ULONGLONG now = GetTickCount64()) {
        if (stopped_) return false;
        bool changed = false;
        for (auto& slot : slots_) {
            if (slot.active) {
                auto completion = slot.job.TakeResult();
                if (completion) {
                    for (const auto& request : slot.requests) {
                        auto found = entries_.find(request.key);
                        if (found != entries_.end()) {
                            found->second.pending = false;
                            found->second.retryAtTick = now + kTtlMs;
                        }
                    }
                    if (completion->result) for (const auto& result : *completion->result) {
                        auto found = entries_.find(result.key);
                        if (found == entries_.end() || !result.observation.known) continue;
                        auto& entry = found->second;
                        changed |= !entry.observation.known || entry.observation.value != result.observation.value ||
                            entry.observation.isDirectory != result.observation.isDirectory;
                        entry.observation = result.observation; entry.retryAtTick = 0;
                        ++completedProbeCount_;
                    }
                    slot.requests.clear(); slot.active = false;
                }
            }
            if (slot.active || pending_.empty()) continue;
            try {
                slot.requests.reserve(kBatchSize);
                while (!pending_.empty() && slot.requests.size() < kBatchSize) {
                    const auto& key = *pending_.front();
                    slot.requests.push_back({key, entries_.at(key).path});
                    pending_.pop_front();
                }
                const auto generation = slot.job.Start([requests = slot.requests, probe = probe_](const auto& cancellation) {
                    Win32IoCancellation interrupt(cancellation);
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    Results results; results.reserve(requests.size());
                    for (const auto& request : requests) {
                        if (cancellation->IsCancelled()) break;
                        PathAvailabilityObservation observation;
                        try { observation = probe(request.path, cancellation); } catch (...) {}
                        observation.checkedAtTick = GetTickCount64();
                        results.push_back({request.key, observation});
                    }
                    return results;
                });
                if (!generation) throw std::runtime_error("Availability worker is stopped");
                slot.active = true;
            } catch (...) {
                for (const auto& request : slot.requests) {
                    auto found = entries_.find(request.key);
                    if (found != entries_.end()) { found->second.pending = false; found->second.retryAtTick = now + kTtlMs; }
                }
                slot.requests.clear();
            }
        }
        // Shrinking a library releases old observations gradually. Work per UI
        // tick remains bounded even if every resident entry is still pending.
        for (size_t i = 0; i < 8 && entries_.size() > capacity_; ++i) if (!EvictOne()) break;
        return changed;
    }

    void Clear() {
        for (auto& slot : slots_) { slot.job.Cancel(); slot.requests.clear(); slot.active = false; }
        pending_.clear(); residentOrder_.clear(); entries_.clear();
    }
    bool IsRunning() const {
        for (const auto& slot : slots_) if (slot.job.IsRunning()) return true;
        return false;
    }
    bool HasPending() const {
        if (!pending_.empty()) return true;
        for (const auto& slot : slots_) if (slot.active) return true;
        return false;
    }
    size_t CacheSize() const { return entries_.size(); }
    uint64_t CompletedProbeCount() const { return completedProbeCount_; }
    void StopAndJoin() {
        Clear(); stopped_ = true;
        for (auto& slot : slots_) slot.job.StopAndJoin();
    }
private:
    struct Entry {
        std::wstring path;
        PathAvailabilityObservation observation;
        ULONGLONG retryAtTick = 0;
        bool pending = false;
    };
    struct Request { std::wstring key, path; };
    struct Result { std::wstring key; PathAvailabilityObservation observation; };
    using Results = std::vector<Result>;
    struct Slot {
        AsyncOwnedJob<Results> job;
        std::vector<Request> requests;
        bool active = false;
    };
    bool EvictOne() {
        const size_t attempts = (std::min)(size_t{64}, residentOrder_.size());
        for (size_t i = 0; i < attempts; ++i) {
            const auto* key = residentOrder_.front(); residentOrder_.pop_front();
            auto found = entries_.find(*key);
            if (!found->second.pending) { entries_.erase(found); return true; }
            residentOrder_.push_back(key);
        }
        return false;
    }
    Probe probe_;
    size_t capacity_ = kExtraPaths;
    uint64_t completedProbeCount_ = 0;
    bool stopped_ = false;
    std::unordered_map<std::wstring, Entry> entries_;
    // unordered_map rehash preserves key references. Pending/in-flight entries
    // cannot be evicted; each queue is cleared before the map during reset.
    std::deque<const std::wstring*> pending_, residentOrder_;
    std::array<Slot, 2> slots_;
};

} // namespace Aperture
