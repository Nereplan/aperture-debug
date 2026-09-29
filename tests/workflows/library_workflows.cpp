#include "LibraryQuery.h"
#include "GlobalSearch.h"
#include "TestMedia.h"
#include <random>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace Aperture;
namespace fs = std::filesystem;
static uint32_t activeSeed;
static size_t checks;
static void Check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) throw std::runtime_error(message + " (seed=" + std::to_string(activeSeed) + ")");
}

static void QueryWorkflows(std::mt19937& rng, size_t count) {
    MediaLibrary library;
    const std::wstring labels[] = {L"Film", L"İstanbul", L"日本語", L"Café", L"العربية"};
    for (size_t i = 0; i < count; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.title = labels[rng() % 5] + std::wstring(L" episode ") + std::to_wstring(i);
        item.filePath = GetExeDir() + L"\\media\\Group " + std::to_wstring(rng() % 37) + L"\\" + item.title + L".mkv";
        item.durationSec = 30 + rng() % 3000 + double(i) / 100000;
        item.lastPosSec = item.durationSec * (rng() % 100) / 100.0;
        item.lastWatchedTimestamp = rng() % 10000;
        item.addedTimestamp = rng() % 10000;
        library.mediaItems.push_back(std::move(item));
    }
    std::shuffle(library.mediaItems.begin(), library.mediaItems.end(), rng);
    library.RebuildPathIndex();
    LibraryQueryEngine engine;
    GlobalSearchIndex search;
    // Oracle uses generated numeric values only, independent of the product
    // comparator, filtering helpers, caches and result projection.
    for (int direction = 0; direction < 2; ++direction) {
        std::vector<const MediaItem*> oracle;
        for (const auto& item : library.mediaItems) oracle.push_back(&item);
        std::sort(oracle.begin(), oracle.end(), [=](auto a, auto b) {
            return direction ? a->durationSec > b->durationSec : a->durationSec < b->durationSec;
        });
        for (size_t limit : {size_t(0), size_t(1), size_t(17), count / 4, count > 0 ? count - 1 : 0, count + 1}) {
            LibraryQueryOptions options;
            options.sortField = LibrarySortField::Duration;
            options.sortDirection = static_cast<SortDirection>(direction);
            options.maxItems = limit;
            const auto& result = engine.Query(library, options);
            const size_t expected = limit ? (std::min)(limit, count) : count;
            Check(result.items.size() == expected, "Query lost or added results");
            for (size_t i = 0; i < expected; ++i) Check(result.items[i].id == oracle[i]->id, "Duration order differs from oracle");
            SmartPlaylistCriteria criteria;
            criteria.sortField = int(LibrarySortField::Duration);
            criteria.sortDirection = direction;
            criteria.limit = limit;
            const auto paths = library.EvaluateSmartPlaylist(criteria);
            Check(paths.size() == expected, "Smart playlist count differs from oracle");
            for (size_t i = 0; i < expected; ++i) Check(paths[i] == oracle[i]->filePath, "Smart playlist order differs from oracle");
        }
    }
    if (!count) return;
    // Replay user edits against a warm search cache, then search the new title.
    for (unsigned step = 0; step < 12; ++step) {
        const size_t index = rng() % count;
        const auto path = library.mediaItems[index].filePath;
        const std::wstring title = L"Unique edited title " + std::to_wstring(activeSeed) + L" " + std::to_wstring(step);
        GlobalSearchQueryOptions options;
        options.checkFileExists = false;
        search.Query(library, L"episode", options);
        MediaMetadataOverrides metadata;
        metadata.title = title;
        Check(library.SetMetadataOverrides(path, metadata), "Title edit rejected");
        const auto found = search.Query(library, title, options);
        Check(!found.empty() && found.front().filePath == path && found.front().title == title, "Search served stale edited metadata");
    }
    // Grouping must preserve every member even with Unicode and varied input order.
    LibraryQueryOptions grouped;
    grouped.groupMode = LibraryGroupMode::Folder;
    const auto projection = engine.Query(library, grouped);
    std::unordered_set<std::string> ids;
    size_t total = 0;
    for (const auto& group : projection.groups) total += group.count;
    for (const auto& item : projection.items) Check(ids.insert(item.id).second, "Grouping duplicated an item");
    Check(total == count && ids.size() == count, "Grouping lost members");
}

static void PersistenceWorkflow(std::mt19937& rng) {
    MediaLibrary library;
    std::vector<std::wstring> paths;
    for (unsigned i = 0; i < 12; ++i) {
        auto path = fs::path(GetExeDir()) / L"media" / (L"Café sample " + std::to_wstring(i) + L".wav");
        WriteWorkflowWave(path, 1 + rng() % 4);
        paths.push_back(path.wstring());
    }
    const auto imported = library.ImportPaths(paths);
    Check(imported.added == paths.size(), "Real-file import count mismatch");
    library.ImportPaths(paths);
    Check(library.mediaItems.size() == paths.size(), "Repeated import created duplicates");
    const auto playlistId = library.CreatePlaylist("Evening queue")->id;
    Check(library.AddToPlaylist(playlistId, paths) == paths.size(), "Playlist import failed");
    std::unordered_map<std::wstring, double> positions;
    for (unsigned step = 0; step < 60; ++step) {
        const auto& path = paths[rng() % paths.size()];
        const double position = double(rng() % 250) / 100;
        library.UpdateProgress(path, position, 300);
        positions[path] = position;
        if (step % 10 == 0) {
            Check(library.Save().is_ok(), "Production Save failed");
            MediaLibrary reopened;
            Check(reopened.Load().is_ok(), "Production Load failed");
            for (const auto& expected : positions) {
                const auto* item = reopened.FindByPath(expected.first);
                Check(item && std::abs(item->lastPosSec - expected.second) < 0.0001, "Resume position lost after reopen");
            }
            library = std::move(reopened);
        }
    }
    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.25;
    prefs.audioDelayMs = -125;
    prefs.subtitlesVisible = false;
    Check(library.SetMediaPreferences(paths[0], prefs), "Playback preferences rejected");
    MediaBookmark bookmark;
    bookmark.id = "review"; bookmark.timeSec = 1.5; bookmark.label = L"Review 日本語";
    Check(library.AddBookmark(paths[0], bookmark), "Bookmark rejected");
    library.FindByPath(paths[0])->extraFields["futureField"] = "must-survive";
    const auto removedId = library.FindByPath(paths.back())->id;
    Check(library.RemoveMedia(removedId), "Library removal failed");
    Check(library.Save().is_ok(), "Final Save failed");
    MediaLibrary reopened;
    Check(reopened.Load().is_ok(), "Final Load failed");
    Check(!reopened.FindById(removedId), "Removed item reappeared after restart");
    Check(reopened.FindPlaylist(playlistId)->itemPaths.size() == paths.size() - 1, "Removed item remained in queue");
    const auto* item = reopened.FindByPath(paths[0]);
    Check(item && item->preferences && item->preferences->playbackSpeed == 1.25 &&
          item->preferences->audioDelayMs == -125 && item->preferences->subtitlesVisible == false, "Preferences lost in real round trip");
    Check(item->bookmarks.size() == 1 && item->bookmarks[0].label == bookmark.label, "Bookmark lost in real round trip");
    Check(item->extraFields.get("futureField").as_string() == "must-survive", "Unknown fields lost in real round trip");
    // Move a portable profile together with its real media and resolve paths
    // from that root, independent of the process working directory.
    const auto relocated = fs::path(GetExeDir()) / (L"relocated-" + std::to_wstring(activeSeed));
    fs::create_directories(relocated);
    fs::copy(fs::path(GetExeDir()) / L"data", relocated / L"data", fs::copy_options::recursive);
    fs::copy(fs::path(GetExeDir()) / L"media", relocated / L"media", fs::copy_options::recursive);
    MediaLibrary moved;
    Check(moved.LoadFromRoot(relocated.wstring()).is_ok(), "Relocated profile failed to load");
    const auto movedPath = relocated / L"media" / fs::path(paths[0]).filename();
    Check(moved.FindByPath(movedPath.wstring()) != nullptr && fs::exists(movedPath), "Portable relocation broke media links");
}

static void SaveArrayWorkflow() {
    MediaLibrary library;
    auto readStore = [](const wchar_t* name) {
        std::string json;
        Check(ReadTextFile(GetDataDir() + L"\\" + name + L".json", json).is_ok(), "Cannot read saved array");
        return json;
    };
    Check(library.Save().is_ok(), "Empty save failed");
    for (const auto* name : {L"library", L"playlists", L"history"})
        Check(readStore(name) == "[]", "Empty store must be a JSON array");
    MediaItem item;
    item.id = "one";
    item.filePath = L"C:\\Benchmark\\one.mkv";
    item.title = L"Quote\"\n日本語";
    library.mediaItems.push_back(item);
    Check(library.Save().is_ok(), "Singleton save failed");
    const std::string expectedRecord =
        R"({"addedTimestamp":0,"durationSec":0,"filePath":"C:\\Benchmark\\one.mkv","fileSizeBytes":0,"id":"one","lastPosSec":0,"lastWatchedTimestamp":0,"status":"new","title":"Quote\"\n日本語","videoHeight":0,"videoWidth":0})";
    Check(readStore(L"library") == "[" + expectedRecord + "]", "Singleton save changed wire format");
    item.id = "two";
    library.mediaItems.push_back(item);
    Check(library.Save().is_ok(), "Multiple-record save failed");
    std::string secondRecord = expectedRecord;
    secondRecord.replace(secondRecord.find("\"id\":\"one\""), 10, "\"id\":\"two\"");
    Check(readStore(L"library") == "[" + expectedRecord + "," + secondRecord + "]", "Record ordering or separator changed");
    const auto unchanged = library.Save();
    Check(unchanged.is_ok() && !unchanged.librarySaved && !unchanged.playlistsSaved && !unchanged.historySaved,
          "Unchanged arrays must not be rewritten");
    library.mediaItems.clear();
    Check(library.Save().is_ok() && readStore(L"library") == "[]", "Cleared array retained old records");
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        SaveArrayWorkflow();
        for (int arg = 1; arg < argc; ++arg) {
            activeSeed = uint32_t(std::stoul(argv[arg]));
            std::mt19937 rng(activeSeed);
            const auto start = std::chrono::steady_clock::now();
            for (size_t size : {size_t(0), size_t(1), size_t(73), size_t(2048), size_t(25000)}) QueryWorkflows(rng, size);
            PersistenceWorkflow(rng);
            std::cout << "PASS seed=" << activeSeed << " checks=" << checks << " elapsed_ms="
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << std::endl;
        }
        Check(argc > 1, "At least one seed is required");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << std::endl;
        CoUninitialize(); return 1;
    }
    CoUninitialize(); return 0;
}
