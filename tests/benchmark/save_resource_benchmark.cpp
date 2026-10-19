// Isolated save / C++ heap-allocation probe with real temporary profile writes
// and backups. No media or GPU resources; this does not measure process RSS.
#include <cstdio>
#include "LibraryQuery.h"
#include "GlobalSearch.h"
#include <stdexcept>

#include "AllocationProbe.h"

static void Check(bool condition) {
    if (!condition) throw std::runtime_error("Save benchmark verification failed");
}

template<class Library, class Snapshot>
auto PlaybackCapture(Library& library, const Snapshot& previous, int)
    -> decltype(library.CapturePlaybackSnapshot(previous, library.mediaItems.front().filePath)) {
    return library.CapturePlaybackSnapshot(previous, library.mediaItems.front().filePath);
}
template<class Library, class Snapshot>
auto PlaybackCapture(Library& library, const Snapshot& previous, long) {
    return library.CaptureSaveSnapshot(previous); // Same harness builds against the baseline.
}

int main(int argc, char** argv) {
    using namespace Aperture;
    // The runner places this executable in a unique temporary directory.
    // Every save, backup and reload stays beside it; no user profile is read.
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    std::puts("items,mode,sample,ms,allocations,allocated_bytes,peak_live_cpp_bytes,json_bytes,content_hash,retained_cpp_bytes");
    bool captureOnly = false, playbackCapture = false;
    for (int i = 1; i < argc; ++i) {
        captureOnly |= std::string(argv[i]) == "--capture-only";
        playbackCapture |= std::string(argv[i]) == "--playback-capture";
    }
    const std::vector<int> modes = captureOnly ? std::vector<int>{4, 5} : std::vector<int>{0, 1, 2, 3};
    for (int count : {500, 25000}) for (int mode : modes) {
        for (int sample = 0; sample < 8; ++sample) {
            Check(stats.live == 0);
            stats = {};
            MediaLibrary library;
            library.mediaItems.reserve(count);
            for (int i = 0; i < count; ++i) {
                MediaItem item;
                item.id = std::to_string(i);
                item.filePath = L"C:\\Benchmark\\Series\\Episode " + std::to_wstring(i) + L".mkv";
                item.title = L"日本語 Café \"Episode\"\n" + std::to_wstring(i);
                item.durationSec = 1200.125;
                item.lastPosSec = i % 100;
                item.fileSizeBytes = 123456789;
                item.fileIndex = 18446744073709550000ull + i % 100;
                if (i % 10 == 0) {
                    item.extraFields["plugin"]["escaped"] = "tab\tline\ncontrol\x01";
                    item.preferences.emplace();
                    item.preferences->playbackSpeed = 1.25;
                    item.preferences->extraFields["future"] = true;
                    item.overrides.emplace();
                    item.overrides->title = L"Override 日本語";
                }
                library.mediaItems.push_back(std::move(item));
            }
            for (int i = 0; i < count / 50; ++i) {
                Playlist list;
                list.id = std::to_string(i);
                list.name = "Playlist " + list.id;
                for (int j = 0; j < 50; ++j) list.itemPaths.push_back(library.mediaItems[i * 50 + j].filePath);
                library.playlists.push_back(std::move(list));
            }
            for (int i = 0; i < 100; ++i) {
                HistoryRecord record;
                record.filePath = library.mediaItems[i].filePath;
                record.title = library.mediaItems[i].title;
                record.lastPosSec = i;
                record.durationSec = 1200.125;
                record.watchedTimestamp = 1000 + i;
                library.history.push_back(std::move(record));
            }
            library.watchedFolders.push_back(L"C:\\Benchmark\\Series");
            // 0 first save; 1 unchanged save; 2 periodic progress; 3 load.
            if (mode != 0) Check(library.Save().is_ok());
            // Production loads publish a built path index. Preparing it outside
            // the capture isolates capture cost equally in both versions.
            if (playbackCapture) library.RebuildPathIndex();
            auto baseline = mode >= 4 ? library.CaptureSaveSnapshot() : std::shared_ptr<const MediaLibrarySaveSnapshot>{};
            if (mode == 2 || mode == 5) library.mediaItems.front().lastPosSec = 42.125;
            stats = {};
            measuring = sample > 0 && mode != 3;
            const auto start = std::chrono::steady_clock::now();
            MediaLibrarySaveResult saved;
            std::shared_ptr<const MediaLibrarySaveSnapshot> captured;
            if (mode >= 4) captured = playbackCapture ? PlaybackCapture(library, baseline, 0) : library.CaptureSaveSnapshot(baseline);
            else saved = library.Save();
            const auto end = std::chrono::steady_clock::now();
            measuring = false;
            if (mode >= 4) {
                MediaLibrarySaveState state;
                Check(captured && WriteLibrarySaveSnapshot(*captured, state).is_ok());
            }
            Check(saved.is_ok());
            if (mode < 4) {
            Check(saved.librarySaved == (mode == 0 || mode == 2));
            Check(saved.playlistsSaved == (mode == 0));
            Check(saved.historySaved == (mode == 0));
            }
            const auto measured = stats;
            MediaLibrary loadedProbe;
            AllocationStats loadStats;
            double loadMs = 0;
            if (mode == 3) {
                stats = {};
                measuring = sample > 0;
                const auto loadStart = std::chrono::steady_clock::now();
                Check(loadedProbe.Load().is_ok());
                loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
                measuring = false;
                loadStats = stats;
                Check(loadedProbe.mediaItems.size() == size_t(count));
            }
            uint64_t hash = 14695981039346656037ull;
            size_t bytes = 0;
            for (const wchar_t* name : {L"library", L"playlists", L"history", L"watched_folders"}) {
                std::string json;
                Check(ReadTextFile(GetDataDir() + L"\\" + name + L".json", json).is_ok());
                JsonValue parsed;
                Check(JsonValue::parse(json, parsed));
                Check(parsed.stringify(false) == json);
                bytes += json.size();
                for (unsigned char c : json) { hash ^= c; hash *= 1099511628211ull; }
            }
            MediaLibrary reloaded;
            Check(reloaded.Load().is_ok());
            Check(reloaded.mediaItems.size() == size_t(count));
            Check(reloaded.mediaItems.front().lastPosSec == library.mediaItems.front().lastPosSec);
            Check(reloaded.mediaItems.front().extraFields.get("plugin").get("escaped").as_string() == "tab\tline\ncontrol\x01");
            const auto& reported = mode == 3 ? loadStats : measured;
            if (sample > 0) std::printf("%d,%d,%d,%.3f,%zu,%zu,%zu,%zu,%llu,%zu\n", count, mode, sample,
                mode == 3 ? loadMs : std::chrono::duration<double, std::milli>(end - start).count(), reported.calls,
                reported.bytes, reported.peak, bytes, hash, reported.live);
        }
    }
}


