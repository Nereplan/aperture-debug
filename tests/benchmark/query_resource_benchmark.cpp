// Isolated CPU / C++ heap-allocation probe. No media files, profile writes,
// decoders, or GPU resources are involved; this does not measure process RSS.
#include <cstddef>
#include <cstdlib>
#include <new>
#include <cstdio>
#include "LibraryQuery.h"
#include "GlobalSearch.h"

namespace {
struct AllocationStats { size_t calls = 0, bytes = 0, live = 0, peak = 0; } stats;
bool measuring = false;
struct alignas(std::max_align_t) AllocationHeader { size_t bytes; bool tracked; };
}

void* operator new(size_t size) {
    if (size > SIZE_MAX - sizeof(AllocationHeader)) throw std::bad_alloc();
    auto* header = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
    if (!header) throw std::bad_alloc();
    header->bytes = size;
    header->tracked = measuring;
    if (measuring) {
        ++stats.calls;
        stats.bytes += size;
        stats.live += size;
        stats.peak = (std::max)(stats.peak, stats.live);
    }
    return header + 1;
}
void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    auto* header = static_cast<AllocationHeader*>(ptr) - 1;
    if (header->tracked) stats.live -= header->bytes;
    std::free(header);
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, size_t) noexcept { ::operator delete(ptr); }

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
            for (int i = 499; i >= 0; --i)
                options.customFolderOrder.push_back(L"c:/benchmark/series" + std::to_wstring(i) + L"/");
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
