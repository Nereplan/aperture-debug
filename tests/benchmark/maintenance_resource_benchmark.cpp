// Measures bulk-removal cleanup preparation and real indexed eviction. No
// playback/profile writes. Missing-file eviction uses an isolated cache root.
#include "MediaLibrary.h"
#include "CacheStorage.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <type_traits>

using namespace Aperture;
namespace fs = std::filesystem;

static uint64_t Hash(std::wstring_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : text) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}

// The baseline runs the former AppSelection caller's scans around the actual
// library removal. The current build gets cleanup metadata from that removal.
// SFINAE keeps the same harness usable with the baseline source revision.
template<typename Library, typename = void> struct RemovalCleanup {
    struct Target { std::string id; std::wstring filePath; double durationSec; };
    static std::vector<Target> Run(Library& library, const std::vector<std::string>& ids, size_t& removed, size_t& skipped) {
        std::vector<Target> targets;
        targets.reserve(ids.size());
        for (const auto& id : ids) {
            for (const auto& item : library.mediaItems) {
                if (item.id == id) { targets.push_back({item.id, item.filePath, item.durationSec}); break; }
            }
        }
        removed = library.RemoveMedia(ids, &skipped, false);
        targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const Target& target) {
            return std::any_of(library.mediaItems.begin(), library.mediaItems.end(),
                [&](const MediaItem& item) { return item.id == target.id; });
        }), targets.end());
        return targets;
    }
};

template<typename Library> struct RemovalCleanup<Library, std::void_t<typename Library::RemovedMediaInfo>> {
    static std::vector<typename Library::RemovedMediaInfo> Run(Library& library, const std::vector<std::string>& ids,
                                                              size_t& removed, size_t& skipped) {
        std::vector<typename Library::RemovedMediaInfo> targets;
        removed = library.RemoveMedia(ids, &skipped, false, &targets);
        return targets;
    }
};

static void Row(const char* mode, int sample, double ms, size_t items, size_t changed, uint64_t hash) {
    std::printf("%s,%d,%.6f,%zu,%zu,%zu,%zu,%zu,%llu\n", mode, sample, ms, stats.calls, stats.bytes, stats.peak,
                items, changed, static_cast<unsigned long long>(hash));
    std::fflush(stdout);
}

static void Removal(const char* mode, int count, int stride, int sample) {
    if (stats.live) throw std::runtime_error("Leaked measured allocation");
    stats = {};
    MediaLibrary library;
    std::vector<std::string> ids;
    library.mediaItems.reserve(count);
    Playlist playlist; playlist.id = "fixture";
    uint64_t expectedCleanupHash = 0;
    for (int i = 0; i < count; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Benchmark\\" + std::wstring(80, L'x') + L"\\Episode-" + std::to_wstring(i) + L".mkv";
        item.durationSec = 100.5 + i;
        playlist.itemPaths.push_back(item.filePath);
        if (i % stride == 0) {
            ids.push_back(item.id);
            expectedCleanupHash += Hash(item.filePath) ^ static_cast<uint64_t>(item.durationSec * 2);
        }
        library.mediaItems.push_back(std::move(item));
    }
    library.playlists.push_back(std::move(playlist));
    library.RebuildPathIndex();
    std::reverse(ids.begin(), ids.end());
    size_t removed = 0, skipped = 0;
    measuring = true;
    const auto start = std::chrono::steady_clock::now();
    const auto targets = RemovalCleanup<MediaLibrary>::Run(library, ids, removed, skipped);
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    uint64_t cleanupHash = 0, orderedHash = 14695981039346656037ull;
    for (const auto& target : targets) cleanupHash += Hash(target.filePath) ^ static_cast<uint64_t>(target.durationSec * 2);
    if (removed != ids.size() || skipped || targets.size() != removed || cleanupHash != expectedCleanupHash ||
        library.mediaItems.size() != count - removed || library.playlists[0].itemPaths.size() != count - removed)
        throw std::runtime_error("Removal result mismatch");
    size_t index = 0;
    for (int i = 0; i < count; ++i) if (i % stride != 0) {
        const auto& item = library.mediaItems.at(index);
        if (item.id != std::to_string(i) || library.playlists[0].itemPaths.at(index) != item.filePath)
            throw std::runtime_error("Survivor ordering mismatch");
        orderedHash ^= Hash(item.filePath); orderedHash *= 1099511628211ull;
        ++index;
    }
    Row(mode, sample, std::chrono::duration<double, std::milli>(end - start).count(), count, removed, orderedHash ^ cleanupHash);
}

static void IndexResources(int sample) {
    if (stats.live) throw std::runtime_error("Leaked measured allocation");
    stats = {};
    measuring = true;
    const auto start = std::chrono::steady_clock::now();
    Detail::CacheIndex index;
    for (size_t i = 0; i < 25000; ++i) {
        CacheFileInfo info;
        info.path = L"C:\\Benchmark\\cache\\generic\\" + std::wstring(100, L'x') + L"-" + std::to_wstring(i) + L".bin";
        info.sizeBytes = 100;
        // Production hands each enumerated record to accounting once. Include
        // input ownership in both measurements, so moves cannot hide memory.
        Detail::AddIndexFile(index, std::move(info));
    }
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    if (index.total != 2500000 || index.files.size() != 25000) throw std::runtime_error("Index accounting mismatch");
    Row("index-25000", sample, std::chrono::duration<double, std::milli>(end - start).count(), 25000, 0, index.total);
}

static void Eviction(const char* mode, size_t count, size_t evict, int sample, const fs::path& root) {
    if (stats.live) throw std::runtime_error("Leaked measured allocation");
    stats = {};
    Detail::CacheIndex index;
    const auto directory = GetCacheDir(CacheClass::Generic, root.wstring());
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        CacheFileInfo info;
        info.path = directory + L"\\" + std::wstring(100, L'x') + L"-" + std::to_wstring(i) + L".bin";
        info.sizeBytes = 100; info.cacheClass = CacheClass::Generic;
        info.lastAccessTime = {static_cast<DWORD>(i + 1), 1}; info.lastWriteTime = info.lastAccessTime;
        paths.push_back(info.path);
        Detail::AddIndexFile(index, info);
    }
    CacheBudgetConfig config; config.totalCeilingBytes = (count - evict) * 100;
    std::lock_guard<std::recursive_mutex> lock(Detail::CacheIOMutex());
    measuring = true;
    const auto start = std::chrono::steady_clock::now();
    const auto result = Detail::EvictIndexed(index, config, root.wstring(), std::nullopt, 0);
    const auto end = std::chrono::steady_clock::now();
    measuring = false;
    if (result.filesEvicted != evict || result.filesAttempted != evict || result.filesLocked ||
        index.files.size() != count - evict || index.total != (count - evict) * 100)
        throw std::runtime_error("Eviction accounting mismatch");
    uint64_t hash = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        const bool exists = index.files.find(paths[i]) != index.files.end();
        if (exists != (i >= evict)) throw std::runtime_error("Eviction order mismatch");
        // Use fixture indices so random temporary-root names cannot affect equivalence.
        if (exists) { hash ^= i + 1; hash *= 1099511628211ull; }
    }
    Row(mode, sample, std::chrono::duration<double, std::milli>(end - start).count(), count, evict, hash);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const int sample = _wtoi(argv[1]);
    const fs::path root = argv[2];
    try {
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
            SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
        std::puts("mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,items,changed,result_hash");
        Removal("remove-500-half", 500, 2, sample);
        Removal("remove-25000-sparse", 25000, 500, sample);
        Removal("remove-25000-half", 25000, 2, sample);
        IndexResources(sample);
        Eviction("evict-25000-one", 25000, 1, sample, root);
        Eviction("evict-25000-100", 25000, 100, sample, root);
        if (stats.live) throw std::runtime_error("Leaked measured allocation");
        return 0;
    } catch (const std::exception& error) {
        measuring = false;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
