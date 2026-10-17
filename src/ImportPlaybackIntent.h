#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace Aperture {

// One-way worker -> owner handoff. Directory discovery has no stable ordering;
// only the first entry of the subsequent sorted probe pass may start playback.
class ImportPlaybackIntent {
    std::mutex mutex;
    std::wstring pending;
    uint64_t generation = 0;
    bool enabled = false, offered = false;
public:
    void Reset(uint64_t currentGeneration, bool shouldLaunch) {
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear(); generation = currentGeneration; enabled = shouldLaunch; offered = false;
    }
    bool Offer(size_t processed, size_t total, const std::wstring& path) {
        if (processed || !total || path.empty()) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (!enabled || offered) return false;
        pending = path; offered = true; return true;
    }
    std::wstring Take(uint64_t currentGeneration) {
        std::lock_guard<std::mutex> lock(mutex);
        if (currentGeneration != generation) { pending.clear(); enabled = false; }
        auto result = std::move(pending);
        pending.clear();
        return result;
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear(); enabled = false;
    }
};

} // namespace Aperture
