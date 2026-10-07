// Isolated CPU / C++ heap-allocation probe. No media files, profile writes,
// decoders, or GPU resources are involved; this does not measure process RSS.
#include <cstdio>
#include "LibraryQuery.h"
#include "GlobalSearch.h"

#include "AllocationProbe.h"

int main(int argc, char** argv) {
    using namespace Aperture;
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    MediaLibrary library;
    library.mediaItems.reserve(25000);
    for (int i = 0; i < 25000; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Benchmark\\Series" + std::to_wstring(i / 50) +
            L"\\Series" + std::to_wstring(i / 50) + L" S01E" + std::to_wstring(i % 50 + 1) + L".mkv";
        item.title = GetFileTitleFromPath(item.filePath);
        item.durationSec = 1200;
        library.mediaItems.push_back(std::move(item));
    }
    std::puts("mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,items,groups,order_hash");
    if (argc > 1 && std::string(argv[1]) == "--extended") {
        for (int workload : {19, 20, 21, 22, 23, 24, 25}) {
            auto items = library.mediaItems;
            if (workload >= 22 && workload <= 24) {
                items.resize(4000);
                for (size_t i = 0; i < items.size(); ++i) {
                    auto& item = items[i];
                    item.fileSizeBytes = 1000000;
                    item.title = L"Group " + std::to_wstring(i % (workload == 24 ? 16 : 100));
                    item.durationSec = workload == 24 ? 4.5 + (i % 5) * 0.25 : 1200.0;
                    if (workload == 23 || (workload == 24 && i % 7 != 0)) {
                        item.videoWidth = 1280 + (i % 4) * 640;
                        item.videoHeight = 720 + (i % 4) * 360;
                    }
                }
            }
            MediaLibrary extended; extended.mediaItems = std::move(items);
            if (workload == 21) {
                for (size_t i = 0; i < 1000; ++i) {
                    HistoryRecord history;
                    history.filePath = i < 500 ? extended.mediaItems[i * 40].filePath : L"C:\\History\\Series " + std::to_wstring(i) + L".mkv";
                    history.title = L"Series history " + std::to_wstring(i);
                    history.durationSec = 1200; history.watchedTimestamp = i + 1;
                    extended.history.push_back(std::move(history));
                }
            }
            LibraryQueryOptions options;
            if (workload == 19 || workload == 20) options.groupMode = LibraryGroupMode::Folder;
            if (workload == 19) {
                auto collapsed = std::make_shared<std::unordered_set<std::wstring>>();
                for (int i = 1; i < 500; ++i) collapsed->insert(L"c:\\benchmark\\series" + std::to_wstring(i));
                options.collapsedGroups = std::move(collapsed);
            }
            if (workload == 20) options.maxItems = 17;
            if (workload == 25) options.searchQuery = L"absent query";
            for (int sample = 0; sample < 8; ++sample) {
                LibraryQueryEngine engine; GlobalSearchIndex search;
                if (stats.live != 0) return 2;
                stats = {}; measuring = sample > 0;
                size_t count = 0, groups = 0;
                uint64_t hash = 14695981039346656037ull;
                const auto start = std::chrono::steady_clock::now();
                if (workload >= 22 && workload <= 24) {
                    const auto result = LibraryQueryEngine::DetectDuplicates(extended.mediaItems);
                    const auto end = std::chrono::steady_clock::now(); measuring = false;
                    count = result.totalDuplicateEntries; groups = result.totalDuplicateGroups;
                    for (const auto& group : result.groups) {
                        hash ^= static_cast<uint64_t>(group.matchKind); hash *= 1099511628211ull;
                        for (const auto i : group.originalIndices) { hash ^= i; hash *= 1099511628211ull; }
                    }
                    if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                        std::chrono::duration<double, std::milli>(end - start).count(), stats.calls, stats.bytes, stats.peak, count, groups, hash);
                } else if (workload == 21) {
                    search.RebuildIndex(extended);
                    const auto end = std::chrono::steady_clock::now(); measuring = false;
                    const auto measured = stats;
                    GlobalSearchQueryOptions query; query.checkFileExists = false;
                    const auto results = search.Query(extended, L"series", query, groups);
                    for (const auto& result : results) for (const wchar_t c : result.filePath) { hash ^= c; hash *= 1099511628211ull; }
                    if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                        std::chrono::duration<double, std::milli>(end - start).count(), measured.calls, measured.bytes, measured.peak, search.GetEntryCount(), groups, hash);
                } else {
                    const auto& result = engine.Query(extended, options);
                    const auto end = std::chrono::steady_clock::now(); measuring = false;
                    for (const auto& item : result.items) { hash ^= item.originalLibraryIndex; hash *= 1099511628211ull; }
                    if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                        std::chrono::duration<double, std::milli>(end - start).count(), stats.calls, stats.bytes, stats.peak, result.items.size(), result.groups.size(), hash);
                }
            }
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--duplicates") {
        for (int workload : {15, 16, 17, 18}) {
            const size_t count = workload == 16 ? 4000 : 25000;
            auto items = library.mediaItems;
            items.resize(count);
            for (size_t i = 0; i < count; ++i) {
                auto& item = items[i];
                item.fileSizeBytes = 1000000;
                item.durationSec = workload == 15 || workload == 18 ? 1200.0 + i / 2 : 1200.0;
                item.videoWidth = 1920; item.videoHeight = 1080;
                if (workload == 18) {
                    item.videoWidth = item.videoHeight = 0;
                    item.title = L"Pair " + std::to_wstring(i / 2);
                }
                if (workload == 17) {
                    item.volumeSerialNumber = 1;
                    item.fileIndex = i / 50 + 1;
                }
            }
            for (int sample = 0; sample < 8; ++sample) {
                if (stats.live != 0) return 2;
                stats = {}; measuring = sample > 0;
                const auto start = std::chrono::steady_clock::now();
                const auto result = LibraryQueryEngine::DetectDuplicates(items);
                const auto end = std::chrono::steady_clock::now();
                measuring = false;
                const auto measured = stats;
                uint64_t hash = 14695981039346656037ull;
                for (const auto& group : result.groups) {
                    hash ^= static_cast<uint64_t>(group.matchKind); hash *= 1099511628211ull;
                    for (const auto index : group.originalIndices) { hash ^= index; hash *= 1099511628211ull; }
                }
                if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    measured.calls, measured.bytes, measured.peak, result.totalDuplicateEntries,
                    result.totalDuplicateGroups, hash);
            }
        }
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--progress") {
        // Compare the former full text rebuild after progress (mode 14) with
        // the metadata refresh (mode 13), using identical ranking/results.
        library.RebuildPathIndex();
        GlobalSearchQueryOptions options; options.maxResults = 50; options.checkFileExists = false;
        for (int workload : {13, 14}) {
            GlobalSearchIndex progressIndex; progressIndex.RebuildIndex(library);
            for (int sample = 0; sample < 8; ++sample) {
                library.UpdateProgress(library.mediaItems.front().filePath, 25 + sample, 1200);
                if (workload == 14) progressIndex.Invalidate();
                if (stats.live != 0) return 2;
                stats = {}; measuring = sample > 0;
                size_t total = 0;
                const auto start = std::chrono::steady_clock::now();
                const auto results = progressIndex.Query(library, L"series", options, total);
                const auto end = std::chrono::steady_clock::now();
                measuring = false;
                const auto measured = stats;
                uint64_t hash = 14695981039346656037ull;
                for (const auto& result : results) for (wchar_t c : result.filePath) { hash ^= c; hash *= 1099511628211ull; }
                if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                    std::chrono::duration<double, std::milli>(end - start).count(),
                    measured.calls, measured.bytes, measured.peak, results.size(), total, hash);
                if (workload == 14) progressIndex.Invalidate();
            }
        }
        return 0;
    }
    // Modes 0/1/2 retain the original flat/folder/series workloads; mode 3
    // exercises a capped flat projection, mode 4 manual library ordering,
    // and mode 7 a nearly unlimited projection (the full-sort fallback).
    for (int workload : {0, 1, 2, 3, 4, 7}) {
        auto mode = workload < 3 ? static_cast<LibraryGroupMode>(workload) : LibraryGroupMode::Flat;
        LibraryQueryOptions options;
        options.groupMode = mode;
        if (workload == 3) options.maxItems = 50;
        if (workload == 4) options.sortField = LibrarySortField::Manual;
        if (workload == 7) options.maxItems = 24000;
        if (mode == LibraryGroupMode::Folder) {
            auto order = std::make_shared<std::vector<std::wstring>>();
            for (int i = 499; i >= 0; --i)
                order->push_back(L"c:/benchmark/series" + std::to_wstring(i) + L"/");
            options.customFolderOrder = order;
        }
        for (int sample = 0; sample < 8; ++sample) {
            LibraryQueryEngine engine;
            // Previous engine and all tracked allocations have been destroyed.
            if (stats.live != 0) return 2;
            stats = {};
            // Warmup also initializes process-lifetime caches (executable
            // paths, parsers); their allocations are outside query ownership.
            measuring = sample > 0;
            const auto start = std::chrono::steady_clock::now();
            const auto& projection = engine.Query(library, options);
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            uint64_t hash = 14695981039346656037ull;
            for (const auto& item : projection.items) { hash ^= item.originalLibraryIndex; hash *= 1099511628211ull; }
            const double ms = std::chrono::duration<double, std::milli>(end - start).count();
            if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                ms, stats.calls, stats.bytes, stats.peak, projection.items.size(), projection.groups.size(), hash);
        }
    }
    // Mode 5: a limited smart playlist, with the same 25,000 candidates.
    for (int sample = 0; sample < 8; ++sample) {
        SmartPlaylistCriteria criteria;
        criteria.limit = 50;
        if (stats.live != 0) return 2;
        stats = {};
        measuring = sample > 0;
        const auto start = std::chrono::steady_clock::now();
        const auto paths = library.EvaluateSmartPlaylist(criteria);
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        uint64_t hash = 14695981039346656037ull;
        for (const auto& path : paths) for (wchar_t c : path) { hash ^= c; hash *= 1099511628211ull; }
        if (sample > 0) std::printf("5,%d,%.3f,%zu,%zu,%zu,%zu,0,%llu\n", sample,
            std::chrono::duration<double, std::milli>(end - start).count(),
            stats.calls, stats.bytes, stats.peak, paths.size(), hash);
    }
    // Mode 6: broad warm-index search. Index construction is deliberately
    // excluded so ranking latency and per-keystroke heap use are isolated.
    GlobalSearchIndex index;
    index.RebuildIndex(library);
    for (int sample = 0; sample < 8; ++sample) {
        GlobalSearchQueryOptions options;
        options.maxResults = 50;
        options.checkFileExists = false;
        if (stats.live != 0) return 2;
        stats = {};
        measuring = sample > 0;
        size_t total = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto results = index.Query(library, L"series", options, total);
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        uint64_t hash = 14695981039346656037ull;
        for (const auto& item : results) for (wchar_t c : item.filePath) { hash ^= c; hash *= 1099511628211ull; }
        if (sample > 0) std::printf("6,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", sample,
            std::chrono::duration<double, std::milli>(end - start).count(),
            stats.calls, stats.bytes, stats.peak, results.size(), total, hash);
    }
    // Mode 8 isolates the hot extension predicate used by recursive media
    // scans. Paths are prepared before timing so only classification is counted.
    for (int sample = 0; sample < 8; ++sample) {
        if (stats.live != 0) return 2;
        stats = {};
        measuring = sample > 0;
        size_t supported = 0;
        uint64_t hash = 14695981039346656037ull;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& item : library.mediaItems) {
            const bool accepted = MediaLibrary::IsSupportedMediaExtension(item.filePath);
            supported += accepted ? 1 : 0;
            hash ^= accepted ? 1u : 0u;
            hash *= 1099511628211ull;
        }
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        if (sample > 0) std::printf("8,%d,%.3f,%zu,%zu,%zu,%zu,0,%llu\n", sample,
            std::chrono::duration<double, std::milli>(end - start).count(),
            stats.calls, stats.bytes, stats.peak, supported, hash);
    }
    // Mode 9 includes cold index construction, independently of warm ranking.
    for (int sample = 0; sample < 8; ++sample) {
        GlobalSearchIndex coldIndex;
        if (stats.live != 0) return 2;
        stats = {};
        measuring = sample > 0;
        const auto start = std::chrono::steady_clock::now();
        coldIndex.RebuildIndex(library);
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        const auto measured = stats;
        GlobalSearchQueryOptions options;
        options.maxResults = 50;
        options.checkFileExists = false;
        size_t total = 0;
        const auto results = coldIndex.Query(library, L"series", options, total);
        uint64_t hash = 14695981039346656037ull;
        for (const auto& item : results) for (wchar_t c : item.filePath) { hash ^= c; hash *= 1099511628211ull; }
        if (sample > 0) std::printf("9,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", sample,
            std::chrono::duration<double, std::milli>(end - start).count(),
            measured.calls, measured.bytes, measured.peak, results.size(), total, hash);
    }
    // Multi-token and rejected searches cover the per-candidate scoring paths.
    int workload = 10;
    for (const wchar_t* query : {L"series benchmark", L"series s01e1", L"absent query"}) {
        for (int sample = 0; sample < 8; ++sample) {
            GlobalSearchQueryOptions options;
            options.maxResults = 50;
            options.checkFileExists = false;
            if (stats.live != 0) return 2;
            stats = {};
            measuring = sample > 0;
            size_t total = 0;
            const auto start = std::chrono::steady_clock::now();
            const auto results = index.Query(library, query, options, total);
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            uint64_t hash = 14695981039346656037ull;
            for (const auto& item : results) for (wchar_t c : item.filePath) { hash ^= c; hash *= 1099511628211ull; }
            if (sample > 0) std::printf("%d,%d,%.3f,%zu,%zu,%zu,%zu,%zu,%llu\n", workload, sample,
                std::chrono::duration<double, std::milli>(end - start).count(),
                stats.calls, stats.bytes, stats.peak, results.size(), total, hash);
        }
        ++workload;
    }
}
