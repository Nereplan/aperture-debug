// Compare the former sequential membership scan with the production batch
// merge. This measures CPU only, excluding imports, metadata probes and IO.
#include "PlaylistIO.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

using Paths = std::vector<std::wstring>;

static void SequentialAppend(Paths& existing, const Paths& candidates) {
    for (const auto& path : candidates) {
        bool duplicate = false;
        for (const auto& prior : existing) {
            if (CompareStringOrdinal(prior.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) existing.push_back(path);
    }
}

template<typename Append>
static double Time(Paths& paths, const Paths& candidates, Append append) {
    const auto start = std::chrono::steady_clock::now();
    append(paths, candidates);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main() {
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    const auto path = [](int id) { return L"C:\\Benchmark\\Video" + std::to_wstring(id) + L".mkv"; };
    std::puts("mode,sample,before_ms,after_ms,items,order_hash");
    for (int mode = 0; mode < 4; ++mode) {
        Paths seed, candidates;
        if (mode != 0) for (int i = 0; i < 10000; ++i) seed.push_back(path(i));
        if (mode == 0) for (int i = 0; i < 2000; ++i) candidates.push_back(path(i));
        if (mode == 1) for (int i = 9500; i < 10500; ++i) candidates.push_back(path(i));
        if (mode == 2) candidates.push_back(path(10001));
        if (mode == 3) candidates.assign(2000, path(10001));
        for (int sample = 0; sample < 8; ++sample) {
            Paths before = seed, after = seed;
            double beforeMs, afterMs;
            const auto batch = [](Paths& paths, const Paths& incoming) {
                Aperture::PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(paths, incoming);
            };
            if (sample % 2) {
                afterMs = Time(after, candidates, batch);
                beforeMs = Time(before, candidates, SequentialAppend);
            } else {
                beforeMs = Time(before, candidates, SequentialAppend);
                afterMs = Time(after, candidates, batch);
            }
            if (before != after) throw std::runtime_error("Playlist merge differs from sequential membership scan");
            uint64_t hash = 14695981039346656037ull;
            for (const auto& item : after) for (wchar_t c : item) { hash ^= c; hash *= 1099511628211ull; }
            if (sample > 0) std::printf("%d,%d,%.3f,%.3f,%zu,%llu\n", mode, sample, beforeMs, afterMs, after.size(), hash);
        }
    }
}
