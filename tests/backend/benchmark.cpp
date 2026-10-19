#include "Common.h"
#include "Json.h"
#include "../benchmark/AllocationProbe.h"
#include <filesystem>
#include <cstdio>
#include <stdexcept>
using namespace Aperture;
namespace fs = std::filesystem;
static void Check(bool ok) { if (!ok) throw std::runtime_error("Backend benchmark output mismatch"); }
static uint64_t Hash(std::string_view text) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
    return hash;
}
int main() {
    const auto root = GetExeDir();
    Check(FileExists(root + L"\\.backend-test-root"));
    for (int i = 0; i < 2000; ++i) {
        const auto path = GetCacheDir(static_cast<CacheClass>(i % 4), root) + L"\\" + std::to_wstring(i) + L".bin";
        Check(AtomicWriteTextFile(path, std::string(1 + i % 31, 'x')).is_ok());
    }
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    std::puts("workload,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,result_hash");
    for (int mode = 0; mode < 2; ++mode) for (int sample = 0; sample < 8; ++sample) {
        std::string output;
        output.reserve(2000000);
        Check(stats.live == 0);
        stats = {};
        uint64_t hash = 0;
        measuring = sample != 0;
        const auto start = std::chrono::steady_clock::now();
        if (mode == 0) {
            const auto usage = QueryCacheStats(root);
            Check(usage.totalFiles == 2000);
            hash = usage.totalBytes;
            for (const auto& entry : usage.classStats) hash = hash * 31 + entry.second.totalBytes;
        } else {
            for (int i = 0; i < 100000; ++i) { JsonValue(i + 0.125).append_to(output); output.push_back(','); }
        }
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        if (mode == 1) hash = Hash(output);
        if (sample) std::printf("%s,%d,%.3f,%zu,%zu,%zu,%llu\n", mode == 0 ? "cache_stats_2000" : "json_numbers_100000", sample,
            std::chrono::duration<double, std::milli>(end - start).count(), stats.calls, stats.bytes, stats.peak, hash);
    }
}
