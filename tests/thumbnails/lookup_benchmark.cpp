// Isolated CPU benchmark for Renderer bitmap ownership lookup. It models the
// same 291-entry cache limit (64 MiB / 320x180 BGRA) and the old front-LRU
// probe followed by a full map scan. It does not measure Direct2D or FPS.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

namespace {
constexpr size_t kEntries = 291;
constexpr size_t kLookups = 80000;
constexpr int kSamples = 7;

struct Entry { int id; };
using Cache = std::map<int, Entry>;
using Index = std::unordered_map<const void*, const Entry*>;

const Entry* OldLookup(const Cache& cache, const std::vector<Entry>& storage,
                       int frontId, const void* needle) {
    auto front = cache.find(frontId);
    if (front != cache.end() && &storage[front->second.id] == needle) return &front->second;
    for (const auto& item : cache)
        if (&storage[item.second.id] == needle) return &item.second;
    return nullptr;
}

template <typename Lookup>
double Time(const std::vector<const void*>& workload, Lookup lookup, uint64_t& checksum) {
    uint64_t total = 0;
    auto start = std::chrono::steady_clock::now();
    for (const void* needle : workload) {
        const Entry* found = lookup(needle);
        total += found ? static_cast<uint64_t>(found->id + 1) : 0;
    }
    auto end = std::chrono::steady_clock::now();
    checksum = total;
    return std::chrono::duration<double, std::nano>(end - start).count() / workload.size();
}

void RunCase(const char* name, const std::vector<const void*>& workload,
             const Cache& cache, const Index& index, const std::vector<Entry>& storage) {
    std::array<double, kSamples> oldSamples{}, newSamples{};
    const auto oldLookup = [&](const void* needle) { return OldLookup(cache, storage, 0, needle); };
    const auto newLookup = [&](const void* needle) -> const Entry* {
        auto it = index.find(needle);
        return it == index.end() ? nullptr : it->second;
    };
    for (int sample = 0; sample < kSamples; ++sample) {
        uint64_t oldSum = 0, newSum = 0;
        if (sample % 2 == 0) {
            oldSamples[sample] = Time(workload, oldLookup, oldSum);
            newSamples[sample] = Time(workload, newLookup, newSum);
        } else {
            newSamples[sample] = Time(workload, newLookup, newSum);
            oldSamples[sample] = Time(workload, oldLookup, oldSum);
        }
        if (oldSum != newSum) { std::fprintf(stderr, "lookup mismatch in %s\n", name); std::exit(1); }
    }
    std::sort(oldSamples.begin(), oldSamples.end());
    std::sort(newSamples.begin(), newSamples.end());
    std::printf("%s: old %.1f ns/lookup, indexed %.1f ns/lookup, %.1fx (median of %d)\n",
        name, oldSamples[kSamples / 2], newSamples[kSamples / 2],
        oldSamples[kSamples / 2] / newSamples[kSamples / 2], kSamples);
}
}

int main() {
    std::vector<Entry> storage(kEntries);
    Cache cache;
    Index index;
    index.reserve(kEntries);
    for (size_t i = 0; i < kEntries; ++i) {
        storage[i].id = static_cast<int>(i);
        auto inserted = cache.emplace(static_cast<int>(i), storage[i]);
        index.emplace(&storage[i], &inserted.first->second);
    }
    std::vector<const void*> hot(kLookups, &storage[0]);
    std::vector<const void*> nonMru;
    nonMru.reserve(kLookups);
    std::mt19937 rng(20260923);
    std::uniform_int_distribution<int> ids(1, static_cast<int>(kEntries - 1));
    for (size_t i = 0; i < kLookups; ++i) nonMru.push_back(&storage[ids(rng)]);
    Entry external{-1};
    std::vector<const void*> misses(kLookups, &external);
    RunCase("most-recent thumbnail", hot, cache, index, storage);
    RunCase("other cached thumbnails", nonMru, cache, index, storage);
    RunCase("external preview miss", misses, cache, index, storage);
}
