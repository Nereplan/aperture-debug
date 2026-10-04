// Same harness for baseline and current source. Input construction is excluded;
// cold rows include the first lookup, warm rows include cache misses and hits.
#include "Subtitles.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <set>
#include <stdexcept>

using namespace Aperture;

static uint64_t Hash(uint64_t hash, std::wstring_view text) {
    for (wchar_t c : text) { hash ^= c; hash *= 1099511628211ull; }
    return (hash ^ 0xff) * 1099511628211ull;
}

static std::wstring Reference(const SubtitleTrack& track, double time) {
    std::wstring text;
    std::set<std::wstring> seen;
    size_t count = 0;
    for (const auto& cue : track.cues) {
        if (cue.startTimeSec > time) break;
        if (cue.endTimeSec < time || cue.text.empty() || !seen.insert(cue.text).second) continue;
        if (!text.empty()) text += L'\n';
        text += cue.text;
        if (++count == 64 || text.size() >= SubtitleEngine::kMaxCueTextBytes) break;
    }
    return text;
}

static void Run(const char* mode, int sample, size_t cueCount, bool overlap, bool seeks, bool cold, bool hits = false) {
    SubtitleEngine engine;
    SubtitleTrack track;
    track.cues.reserve(cueCount + 1);
    if (overlap) track.cues.push_back({0, double(cueCount * 2), L"long overlapping cue"});
    for (size_t i = 0; i < cueCount; ++i) {
        track.cues.push_back({double(i * 2), double(i * 2 + 1), L"Subtitle cue " + std::to_wstring(i)});
    }
    engine.tracks.push_back(std::move(track));
    engine.SelectPrimaryTrack(0);
    std::vector<double> times;
    const size_t queryCount = cold ? 1 : 2000;
    uint32_t seed = 20261005;
    for (size_t i = 0; i < queryCount; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const size_t cue = hits ? cueCount / 2 : seeks ? seed % cueCount : i * cueCount / queryCount;
        // Mix active cues, exact inclusive ends, and gaps; consecutive queries
        // in the 500-cue case also exercise the existing rendered-text cache.
        const double fraction = hits ? 0.5 : static_cast<double>(i % 4) * 0.5;
        times.push_back(double(cue * 2) + fraction);
    }
    uint64_t expected = 14695981039346656037ull;
    for (double time : times) expected = Hash(expected, Reference(engine.tracks[0], time));
    if (!cold) engine.GetPrimarySubtitleText(hits ? times[0] : 0.5);
    stats = {};
    measuring = true;
    const auto start = std::chrono::steady_clock::now();
    uint64_t actual = 14695981039346656037ull;
    for (double time : times) actual = Hash(actual, engine.GetPrimarySubtitleText(time));
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    if (actual != expected) throw std::runtime_error("Subtitle output differs from the linear oracle");
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::printf("%s,%d,%.6f,%zu,%zu,%zu,%zu,%zu,%llu\n", mode, sample, ms, stats.calls, stats.bytes, stats.peak,
                engine.tracks[0].cues.size(), queryCount, static_cast<unsigned long long>(actual));
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const int sample = std::stoi(argv[1]);
        // Keep before/after processes on the same logical processor.
        DWORD_PTR processMask = 0, systemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask ||
            !SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1))) return 3;
        std::puts("mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,cues,queries,result_hash");
        Run("small_playback", sample, 500, false, false, false);
        Run("typical_playback", sample, 5000, false, false, false);
        Run("large_playback", sample, 250000, false, false, false);
        Run("large_seeks", sample, 250000, false, true, false);
        Run("nested_playback", sample, 249999, true, false, false);
        Run("cached_frames", sample, 250000, false, false, false, true);
        Run("cold_typical", sample, 5000, false, false, true);
        Run("cold_large", sample, 250000, false, false, true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
