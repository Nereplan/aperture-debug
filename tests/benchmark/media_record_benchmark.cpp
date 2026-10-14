// Requested C++ allocation costs of media records, excluding lookup indexes,
// process RSS, decoding and GPU resources. No files are read or written.
#include "MediaLibrary.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace Aperture;

static std::vector<MediaItem> MakeRecords(size_t count, size_t density) {
    std::vector<MediaItem> records;
    records.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Benchmark\\Series\\Episode " + std::to_wstring(i) + L".mkv";
        item.title = L"Episode title " + std::to_wstring(i);
        item.durationSec = 1200.0;
        item.lastPosSec = double(i % 1200);
        if (i % 100 < density) {
            auto& prefs = item.preferences.emplace();
            prefs.playbackSpeed = 1.25;
            prefs.audioTrackTitle = L"Original audio track";
            prefs.extraFields["plugin"] = "retained";
            auto& overrides = item.overrides.emplace();
            overrides.title = L"Custom episode title " + std::to_wstring(i);
            overrides.year = 2020;
        }
        records.push_back(std::move(item));
    }
    return records;
}

static uint64_t HashRecords(const std::vector<MediaItem>& records) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto& item : records) {
        for (wchar_t c : item.GetDisplayTitleView()) { hash ^= c; hash *= 1099511628211ull; }
        hash ^= static_cast<uint64_t>(item.lastPosSec); hash *= 1099511628211ull;
        hash ^= item.preferences ? 1 : 0; hash *= 1099511628211ull;
    }
    return hash;
}

int main() {
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    std::puts("items,density,mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,retained_cpp_bytes,record_bytes,content_hash");
    for (size_t count : {size_t(500), size_t(25000)}) {
        for (size_t density : {size_t(0), size_t(10), size_t(100)}) {
            const auto source = MakeRecords(count, density);
            const auto expected = HashRecords(source);
            for (int mode = 0; mode < 3; ++mode) {
                for (int sample = 0; sample < 8; ++sample) {
                    if (stats.live != 0) throw std::runtime_error("Allocation probe scope leaked");
                    stats = {};
                    std::vector<MediaItem> records;
                    uint64_t hash = 0;
                    measuring = sample > 0;
                    const auto start = std::chrono::steady_clock::now();
                    if (mode == 0) records = MakeRecords(count, density);
                    else if (mode == 1) records = source;
                    else hash = HashRecords(source);
                    const auto end = std::chrono::steady_clock::now();
                    measuring = false;
                    const auto measured = stats;
                    if (mode != 2) hash = HashRecords(records);
                    if (hash != expected || (mode != 2 && records.size() != count))
                        throw std::runtime_error("Media record content mismatch");
                    if (sample > 0) std::printf("%zu,%zu,%s,%d,%.6f,%zu,%zu,%zu,%zu,%zu,%llu\n",
                        count, density, mode == 0 ? "build" : mode == 1 ? "copy" : "scan", sample,
                        std::chrono::duration<double, std::milli>(end - start).count(), measured.calls,
                        measured.bytes, measured.peak, measured.live, sizeof(MediaItem), hash);
                }
            }
        }
    }
}
