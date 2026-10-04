#include "Accessibility.h"
#include "RenderViewModel.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

#if __has_include("ChapterMarkerCache.h")
#include "ChapterMarkerCache.h"
#else
// Frozen App::GetChapterMarkersSnapshot implementation from 0373c27,
// parameterized to measure its cache without starting the player/window.
namespace Aperture {
class ChapterMarkerCache {
    std::shared_ptr<const std::vector<TimelineChapterMarker>> snapshot;
    uint64_t version = UINT64_MAX;
    long long quarter = -1;
    double duration = -1;
public:
    auto Get(const std::vector<MpvChapterInfo>& chapters, uint64_t nextVersion, double nextDuration, double time) {
        const long long nextQuarter = static_cast<long long>(time * 4.0);
        if (snapshot && version == nextVersion && quarter == nextQuarter && duration == nextDuration) return snapshot;
        auto markers = std::make_shared<std::vector<TimelineChapterMarker>>();
        if (nextDuration > 0) {
            markers->reserve(std::min<size_t>(chapters.size(), 500));
            for (size_t i = 0; i < chapters.size() && i < 500; ++i) {
                const auto& chapter = chapters[i];
                TimelineChapterMarker marker;
                marker.time = chapter.time;
                marker.normalizedPosition = std::clamp(static_cast<float>(chapter.time / nextDuration), 0.0f, 1.0f);
                marker.title = chapter.title.length() > 80 ? chapter.title.substr(0, 77) + L"..." : chapter.title;
                marker.isPassed = chapter.time <= time;
                markers->push_back(std::move(marker));
            }
        }
        snapshot = std::move(markers);
        version = nextVersion; quarter = nextQuarter; duration = nextDuration;
        return snapshot;
    }
};
}
#endif

using namespace Aperture;

static uint64_t Hash(uint64_t hash, std::wstring_view text) {
    for (wchar_t c : text) { hash ^= c; hash *= 1099511628211ull; }
    return (hash ^ 0xff) * 1099511628211ull;
}

static void Row(const char* mode, int sample, double ms, size_t items, size_t queries, uint64_t hash) {
    std::printf("%s,%d,%.6f,%zu,%zu,%zu,%zu,%zu,%llu\n", mode, sample, ms, stats.calls, stats.bytes, stats.peak,
                items, queries, static_cast<unsigned long long>(hash));
}

static void AccessibilityRefresh(const char* mode, int sample, size_t count, size_t queries) {
    AccessibilityTree tree;
    std::vector<AccessibilityNode> nodes(count);
    for (size_t i = 0; i < count; ++i) {
        auto& node = nodes[i];
        node.id = static_cast<LONG>(i + 1);
        node.name = L"Media item with a descriptive title and episode number " + std::to_wstring(i);
        node.description = std::wstring(100, L'd');
        node.helpText = std::wstring(100, L'h');
        node.automationId = L"MediaItem_" + std::to_wstring(i);
        node.value = std::to_wstring(i);
        node.bounds = {0, static_cast<LONG>(i), 100, static_cast<LONG>(i + 1)};
        node.selected = i % 3 == 0;
        if (i > 0) node.flowsFromIds = {static_cast<LONG>(i)};
    }
    stats = {}; measuring = true;
    // Include the retained tree in heap accounting for both implementations:
    // the baseline reuses its original storage while replacement swaps it out.
    // Only refreshes, not this initial publication, contribute to elapsed time.
    if (!tree.ReplaceNodes(nodes)) throw std::runtime_error("Initial tree rejected");
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < queries; ++i) if (!tree.ReplaceNodes(nodes)) throw std::runtime_error("Tree refresh rejected");
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    const auto actual = tree.GetNodes();
    if (actual.size() != nodes.size()) throw std::runtime_error("Tree size changed");
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < count; ++i) {
        if (actual[i].id != nodes[i].id || actual[i].name != nodes[i].name || actual[i].description != nodes[i].description ||
            actual[i].helpText != nodes[i].helpText || actual[i].automationId != nodes[i].automationId ||
            actual[i].value != nodes[i].value || actual[i].selected != nodes[i].selected || actual[i].flowsFromIds != nodes[i].flowsFromIds ||
            !EqualRect(&actual[i].bounds, &nodes[i].bounds)) throw std::runtime_error("Tree content changed");
        hash = Hash(hash, actual[i].name); hash = Hash(hash, actual[i].value);
    }
    Row(mode, sample, std::chrono::duration<double, std::milli>(end - start).count(), count, queries, hash);
}

static void Chapters(const char* mode, int sample, size_t count, bool seeks) {
    std::vector<MpvChapterInfo> chapters(count);
    for (size_t i = 0; i < count; ++i) {
        chapters[i].time = double(i * 30);
        chapters[i].title = L"Chapter with descriptive title " + std::to_wstring(i);
    }
    const size_t queries = seeks ? 4000 : 36000;
    std::vector<double> times(queries);
    uint32_t seed = 20261005;
    for (size_t i = 0; i < queries; ++i) {
        seed = seed * 1664525u + 1013904223u;
        times[i] = double(seeks ? seed % 36000 : i) / 60.0;
    }
    uint64_t expected = 14695981039346656037ull;
    for (size_t i = 0; i < queries; ++i) {
        if (!count) { expected = Hash(expected, L""); continue; }
        const auto& chapter = chapters[i % count];
        expected = Hash(expected ^ (chapter.time <= times[i]), chapter.title);
    }
    ChapterMarkerCache cache;
    cache.Get(chapters, 1, 15000, 0);
    stats = {}; measuring = true;
    const auto start = std::chrono::steady_clock::now();
    uint64_t actual = 14695981039346656037ull;
    for (size_t i = 0; i < queries; ++i) {
        const auto markers = cache.Get(chapters, 1, 15000, times[i]);
        if (markers->size() != count) throw std::runtime_error("Chapter count changed");
        if (!count) actual = Hash(actual, L"");
        else { const auto& marker = (*markers)[i % count]; actual = Hash(actual ^ marker.isPassed, marker.title); }
    }
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    if (actual != expected) throw std::runtime_error("Chapter output differs from reference");
    Row(mode, sample, std::chrono::duration<double, std::milli>(end - start).count(), count, queries, actual);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        DWORD_PTR processMask = 0, systemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) || !processMask ||
            !SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1))) return 3;
        const int sample = std::stoi(argv[1]);
        std::puts("mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,items,queries,result_hash");
        AccessibilityRefresh("controls_64", sample, 64, 200);
        AccessibilityRefresh("selected_rows_4096", sample, 4096, 20);
        Chapters("chapters_empty", sample, 0, false);
        Chapters("chapters_12", sample, 12, false);
        Chapters("chapters_500", sample, 500, false);
        Chapters("chapter_seeks", sample, 500, true);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
