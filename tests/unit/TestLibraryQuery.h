#pragma once

#include "../support/TestFramework.h"
#include "../../src/LibraryQuery.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Settings.h"

#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <cwctype>

namespace Aperture::Test {
static void step_LibraryQuery_BorrowedSearchMatchesOwningReference() {
    // Frozen owning traversal: display title, raw title, filename, full path, series.
    const auto reference = [](const MediaItem& item, const std::wstring& query) {
        if (query.empty()) return true;
        const auto contains = [&](const std::wstring& text) {
            return LibraryQueryEngine::NormalizeSearchText(text).find(query) != std::wstring::npos;
        };
        return contains(item.GetDisplayTitle()) || contains(item.title) || contains(GetFileNameFromPath(item.filePath)) ||
            contains(item.filePath) || (item.overrides && item.overrides->series && contains(*item.overrides->series));
    };
    const std::vector<std::wstring> words = {L"  RawTitle  ", L"A/B", L"媒体 字幕", L"İstanbul I", L"", std::wstring(330, L'a') + L" end tail"};
    const std::vector<std::wstring> queries = {L"", L"viewtitle", L"rawtitle", L"a/b", L"媒体", L"ı", L"series",
        L"end tail", L"end\ttail", L"hidden", L"mkv", std::wstring(300, L'a'), std::wstring(1, wchar_t{0})};
    for (size_t i = 0; i < 96; ++i) {
        MediaItem item;
        item.title = words[i % words.size()];
        item.filePath = LR"(C:\Hidden\)" + words[(i / 3) % words.size()] + L"  .mkv";
        if (i % 4 == 0) item.filePath += std::wstring(1, wchar_t{0});
        if (i % 3 == 0) { MediaMetadataOverrides overrides; overrides.title = L"ViewTitle"; overrides.series = L"Series name"; item.overrides = overrides; }
        for (const auto& query : queries) {
            const auto normalized = LibraryQueryEngine::NormalizeSearchText(query);
            TEST_ASSERT_EQ(LibraryQueryEngine::MatchesSearchQuery(item, normalized), reference(item, normalized));
        }
    }
}
static void step_LibraryQuery_CappedGroupedProjectionKeepsVisiblePrefixAndSummaries() {
    MediaLibrary library;
    for (size_t i = 0; i < 320; ++i) {
        const size_t ordinal = 319 - i;
        const auto group = L"Group " + std::to_wstring(ordinal / 40);
        MediaItem item; item.id = std::to_string(ordinal);
        item.filePath = LR"(C:\Grouped\)" + group + LR"(\)" + group + L".S01E" + std::to_wstring(ordinal % 40 + 1) + L".mkv";
        item.title = L"Display " + std::to_wstring(ordinal % 13);
        item.durationSec = 10 + ordinal; item.fileSizeBytes = 1000 + ordinal;
        library.mediaItems.push_back(std::move(item));
    }
    for (const auto mode : {LibraryGroupMode::Folder, LibraryGroupMode::Series}) {
        for (const auto direction : {SortDirection::Ascending, SortDirection::Descending}) {
            LibraryQueryOptions options; options.groupMode = mode; options.sortDirection = direction;
            LibraryQueryEngine seed;
            const auto collapseKey = seed.Query(library, options).groups[1].key;
            options.collapsedGroups = std::make_shared<const std::unordered_set<std::wstring>>(std::unordered_set<std::wstring>{collapseKey});
            LibraryQueryEngine reference;
            const auto& full = reference.Query(library, options);
            for (const size_t cap : {size_t(1), size_t(17), size_t(43), size_t(80), size_t(500)}) {
                options.maxItems = cap;
                LibraryQueryEngine bounded;
                const auto& actual = bounded.Query(library, options);
                TEST_ASSERT_EQ(actual.filteredCount, full.filteredCount);
                TEST_ASSERT_EQ(actual.items.size(), (std::min)(cap, full.items.size()));
                for (size_t i = 0; i < actual.items.size(); ++i) TEST_ASSERT_EQ(actual.items[i].id, full.items[i].id);
                for (size_t i = 0; i < actual.groups.size(); ++i) {
                    const auto& group = actual.groups[i]; const auto& expected = full.groups[i];
                    TEST_ASSERT_EQ(group.key, expected.key); TEST_ASSERT_EQ(group.title, expected.title);
                    TEST_ASSERT_EQ(group.count, expected.count); TEST_ASSERT_EQ(group.startIndex, expected.startIndex);
                    TEST_ASSERT_EQ(group.isCollapsed, expected.isCollapsed);
                    if (group.isCollapsed || group.startIndex + group.count <= actual.items.size()) {
                        TEST_ASSERT_EQ(group.totalDurationSec, expected.totalDurationSec);
                        TEST_ASSERT_EQ(group.totalSizeBytes, expected.totalSizeBytes);
                    }
                }
            }
        }
    }
}
static void step_LibraryQuery_GroupingParserRetainsEpisodeKeysAndPublicSourceFields() {
    const std::vector<std::wstring> paths = {
        LR"(C:\Shows\Dr.Who\S01E03.mkv)", LR"(C:\Shows\Dr.Who\S01E01.mkv)",
        L"C:/Shows/Dr.Who/S02E02.MKV", LR"(C:\Shows\[Subs] New Show.2x04 [1080p].mp4)",
        LR"(C:\Shows\[Subs] New Show.Episode 02.mkv)", LR"(C:\Shows\[1080p].mkv)",
        LR"(C:\Shows\Show.S01E01.srt)", LR"(C:\Shows\Movie.2024.mkv)"
    };
    MediaLibrary library;
    std::vector<EpisodeInfo> episodes;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto info = LibraryQueryEngine::ParseEpisodeInfo(paths[i]);
        TEST_ASSERT_EQ(info.rawPath, paths[i]);
        TEST_ASSERT_EQ(info.rawFileName, GetFileNameFromPath(paths[i]));
        TEST_ASSERT_EQ(info.directoryPath, ExtractDirectory(paths[i]));
        episodes.push_back(info);
        MediaItem item; item.id = std::to_string(i); item.filePath = paths[i]; item.title = L"Title " + std::to_wstring(i);
        library.mediaItems.push_back(std::move(item));
    }
    LibraryQueryEngine engine; LibraryQueryOptions options; options.groupMode = LibraryGroupMode::Series;
    const auto& projection = engine.Query(library, options);
    TEST_ASSERT_EQ(projection.items.size(), paths.size());
    for (const auto& group : projection.groups) {
        int season = -1, episode = -1;
        for (size_t i = group.startIndex; i < group.startIndex + group.count; ++i) {
            const auto& info = episodes[projection.items[i].originalLibraryIndex];
            TEST_ASSERT_EQ(group.key, info.isValid ? info.seriesStem : std::wstring(L"__standalone__"));
            if (!group.isSpecial) {
                TEST_ASSERT(info.seasonNumber > season || (info.seasonNumber == season && info.episodeNumber >= episode));
                season = info.seasonNumber; episode = info.episodeNumber;
            }
        }
    }
}

// ============================================================================
// 1. Episode Token & Pattern Extraction Tests
// ============================================================================
static void step_LibraryQuery_SupportedMediaExtensionPathSemantics() {
    const auto supported = [](const std::wstring& path) {
        return MediaLibrary::IsSupportedMediaExtension(path);
    };
    for (const auto& extension : MediaLibrary::GetSupportedExtensions()) {
        TEST_ASSERT(supported(L"C:\\媒体\\folder.with.dots\\clip" + extension));
        std::wstring upper = extension;
        std::transform(upper.begin(), upper.end(), upper.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
        TEST_ASSERT(supported(L"/媒体/folder.with.dots/clip" + upper));
    }

    TEST_ASSERT(supported(L"C:\\媒体\\clip.profile.MKV"));
    TEST_ASSERT(!supported(L"C:\\媒体\\clip.mkv.backup"));
    TEST_ASSERT(!supported(L"C:\\媒体\\clip.mkv."));
    TEST_ASSERT(!supported(L"C:\\媒体\\.mkv"));
    TEST_ASSERT(!supported(L"C:\\媒体\\.profile"));
    TEST_ASSERT(!supported(L"C:\\媒体\\folder.mkv\\"));
    TEST_ASSERT(!supported(L"C:\\媒体\\clip"));

    // Keep edge behavior tied to the old std::filesystem implementation,
    // including UNC roots and drive-relative paths.
    const std::vector<std::wstring> paths = {
        L"C:.mp4", L"C:clip.mp4", L"C:\\folder\\clip.mkv",
        L"\\\\server.mkv", L"\\\\server.mkv\\", L"\\\\server.mkv\\share\\clip.mp4",
        L"\\\\?\\C:\\folder\\clip.webm", L"\\\\.\\device\\clip.avi",
        L"//server.mkv", L"//server.mkv/share/clip.MP4",
        L".mp4", L"..mp4", L"...mp4", L"folder..\\clip.mkv",
        L"C:\\媒体\\сериал.ogv", L"C:\\媒体\\clip.mkv:stream"
    };
    const std::vector<std::wstring> prefixes = {
        L"", L"C:", L"C:\\", L"C:/folder/", L"folder.with.dots\\",
        L"\\\\server.mkv", L"\\\\server.mkv\\", L"\\\\server.mkv\\share\\",
        L"//server.mkv", L"//server.mkv/share/", L"\\\\?\\C:\\folder\\",
        L"\\\\.\\device\\"
    };
    const std::vector<std::wstring> names = {
        L".", L"..", L".mp4", L"..mp4", L"...mp4", L"clip", L"clip.",
        L"clip.mkv", L"clip.MP4", L"clip.mkv.backup", L"clip..webm",
        L"movie.mkv:stream", L"сериал.ogv", L"音楽.FLAC"
    };
    auto assertMatchesFilesystem = [&](const std::wstring& path) {
        std::wstring oldExtension = std::filesystem::path(path).extension().wstring();
        std::transform(oldExtension.begin(), oldExtension.end(), oldExtension.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        const bool oldResult = MediaLibrary::GetSupportedExtensions().find(oldExtension) !=
            MediaLibrary::GetSupportedExtensions().end();
        TEST_ASSERT_EQ(supported(path), oldResult);
    };
    for (const auto& path : paths) assertMatchesFilesystem(path);
    for (const auto& prefix : prefixes) {
        for (const auto& name : names) assertMatchesFilesystem(prefix + name);
    }
}
static void step_LibraryQuery_EpisodeParsingPatterns() {
    // Standard SxxExx
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Anime\\Steins Gate\\Steins.Gate.S01E05.1080p.mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT(info.hasSeason);
        TEST_ASSERT(info.hasEpisode);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 5);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"steins gate"));
    }

    // s02e12 lowercase with underscores
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"D:\\Series\\Breaking_Bad\\breaking_bad_s02e12_bluray.mp4");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT(info.hasSeason);
        TEST_ASSERT(info.hasEpisode);
        TEST_ASSERT_EQ(info.seasonNumber, 2);
        TEST_ASSERT_EQ(info.episodeNumber, 12);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"breaking bad"));
    }

    // 1x04 format
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\TV\\Friends\\Friends.1x04.The.One.With.George.Stephanopoulos.mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT(info.hasSeason);
        TEST_ASSERT(info.hasEpisode);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 4);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"friends"));
    }

    // "Episode 07" format
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Videos\\Chernobyl\\Chernobyl Episode 03.mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 3);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"chernobyl"));
    }

    // "Ep. 09" format
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Videos\\Frieren\\Frieren Ep. 09 [1080p].mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 9);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"frieren"));
    }

    // Hyphen-separated number "Show Name - 02"
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Anime\\Attack on Titan - 02.mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 2);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"attack on titan"));
    }

    // Release group prefix "[Group] Show Name - 05 [1080p].mkv"
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Downloads\\[SubsPlease] Jujutsu Kaisen - 05 (1080p) [A1B2C3].mkv");
        TEST_ASSERT(info.isValid);
        TEST_ASSERT_EQ(info.seasonNumber, 1);
        TEST_ASSERT_EQ(info.episodeNumber, 5);
        TEST_ASSERT_EQ(info.seriesStem, std::wstring(L"jujutsu kaisen"));
    }

    // Non-episode movie with year
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"D:\\Movies\\Inception (2010)\\Inception.2010.1080p.mkv");
        // Should not be recognized as an episode
        TEST_ASSERT(!info.hasEpisode || !info.isValid || info.episodeNumber > 500);
    }

    // Unsupported extension (.txt, .srt, .exe)
    {
        auto info = LibraryQueryEngine::ParseEpisodeInfo(L"C:\\Series\\Show.S01E02.srt");
        TEST_ASSERT(!info.isValid);
    }
}

// ============================================================================
// 2. Deterministic Sorting & Stable Ties Tests
// ============================================================================
static void step_LibraryQuery_SortingByAllFields() {
    MediaLibrary lib;

    MediaItem itemA;
    itemA.id = "id_a";
    itemA.filePath = L"C:\\Media\\Alpha.mkv";
    itemA.title = L"Alpha";
    itemA.addedTimestamp = 1000;
    itemA.lastWatchedTimestamp = 5000;
    itemA.durationSec = 600.0;
    itemA.lastPosSec = 300.0;
    itemA.status = "in_progress";
    lib.mediaItems.push_back(itemA);

    MediaItem itemB;
    itemB.id = "id_b";
    itemB.filePath = L"C:\\Media\\Beta.mkv";
    itemB.title = L"Beta";
    itemB.addedTimestamp = 3000;
    itemB.lastWatchedTimestamp = 1000;
    itemB.durationSec = 1200.0;
    itemB.lastPosSec = 1200.0;
    itemB.status = "done";
    lib.mediaItems.push_back(itemB);

    MediaItem itemC;
    itemC.id = "id_c";
    itemC.filePath = L"C:\\Media\\Gamma.mkv";
    itemC.title = L"Gamma";
    itemC.addedTimestamp = 2000;
    itemC.lastWatchedTimestamp = 0; // Unplayed
    itemC.durationSec = 300.0;
    itemC.lastPosSec = 0.0;
    itemC.status = "new";
    lib.mediaItems.push_back(itemC);

    LibraryQueryEngine engine;

    // 1. Title Ascending: Alpha, Beta, Gamma
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::Title;
        opt.sortDirection = SortDirection::Ascending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items.size(), 3);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Alpha"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Gamma"));
    }

    // 2. Title Descending: Gamma, Beta, Alpha
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::Title;
        opt.sortDirection = SortDirection::Descending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Gamma"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Alpha"));
    }

    // 3. Date Added Descending: Beta (3000), Gamma (2000), Alpha (1000)
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::DateAdded;
        opt.sortDirection = SortDirection::Descending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Gamma"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Alpha"));
    }

    // 4. Last Played Descending: Alpha (5000), Beta (1000), Gamma (0/unplayed)
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::LastPlayed;
        opt.sortDirection = SortDirection::Descending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Alpha"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Gamma"));
    }

    // 5. Duration Descending: Beta (1200), Alpha (600), Gamma (300)
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::Duration;
        opt.sortDirection = SortDirection::Descending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Alpha"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Gamma"));
    }

    // 6. Progress Descending: Beta (100%), Alpha (50%), Gamma (0%)
    {
        LibraryQueryOptions opt;
        opt.sortField = LibrarySortField::Progress;
        opt.sortDirection = SortDirection::Descending;
        auto proj = engine.Query(lib, opt);
        TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Beta"));
        TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Alpha"));
        TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Gamma"));
    }
}
static void step_LibraryQuery_StableDeterministicTieBreaking() {
    MediaLibrary lib;

    // Create 3 items with identical duration and date added
    MediaItem item1;
    item1.id = "id_3";
    item1.filePath = L"C:\\Media\\Zeta.mkv";
    item1.title = L"Tie Title";
    item1.durationSec = 500.0;
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "id_1";
    item2.filePath = L"C:\\Media\\Alpha.mkv";
    item2.title = L"Tie Title";
    item2.durationSec = 500.0;
    lib.mediaItems.push_back(item2);

    MediaItem item3;
    item3.id = "id_2";
    item3.filePath = L"C:\\Media\\Beta.mkv";
    item3.title = L"Tie Title";
    item3.durationSec = 500.0;
    lib.mediaItems.push_back(item3);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.sortField = LibrarySortField::Duration;
    opt.sortDirection = SortDirection::Ascending;

    auto proj1 = engine.Query(lib, opt);

    // Reorder library mediaItems in reverse
    std::reverse(lib.mediaItems.begin(), lib.mediaItems.end());
    engine.InvalidateCache();
    auto proj2 = engine.Query(lib, opt);

    // Both queries MUST produce the exact same deterministic ordering by secondary tie-breaker (filePath: Alpha, Beta, Zeta)
    TEST_ASSERT_EQ(proj1.items.size(), 3);
    TEST_ASSERT_EQ(proj2.items.size(), 3);
    for (size_t i = 0; i < 3; ++i) {
        TEST_ASSERT_EQ(proj1.items[i].filePath, proj2.items[i].filePath);
    }
    TEST_ASSERT_EQ(proj1.items[0].filePath, std::wstring(L"C:\\Media\\Alpha.mkv"));
    TEST_ASSERT_EQ(proj1.items[1].filePath, std::wstring(L"C:\\Media\\Beta.mkv"));
    TEST_ASSERT_EQ(proj1.items[2].filePath, std::wstring(L"C:\\Media\\Zeta.mkv"));
}

// ============================================================================
// 3. Status Filters Tests
// ============================================================================
static void step_LibraryQuery_StatusFilters() {
    MediaLibrary lib;

    MediaItem itemNew;
    itemNew.id = "item_new";
    itemNew.filePath = L"C:\\Media\\New.mkv";
    itemNew.title = L"New Item";
    itemNew.status = "new";
    itemNew.lastPosSec = 0.0;
    itemNew.durationSec = 100.0;
    lib.mediaItems.push_back(itemNew);

    MediaItem itemProgress;
    itemProgress.id = "item_prog";
    itemProgress.filePath = L"C:\\Media\\Progress.mkv";
    itemProgress.title = L"In Progress Item";
    itemProgress.status = "in_progress";
    itemProgress.lastPosSec = 45.0;
    itemProgress.durationSec = 100.0;
    lib.mediaItems.push_back(itemProgress);

    MediaItem itemDone;
    itemDone.id = "item_done";
    itemDone.filePath = L"C:\\Media\\Done.mkv";
    itemDone.title = L"Completed Item";
    itemDone.status = "done";
    itemDone.lastPosSec = 100.0;
    itemDone.durationSec = 100.0;
    lib.mediaItems.push_back(itemDone);

    MediaItem itemMissing;
    itemMissing.id = "item_missing";
    itemMissing.filePath = L"C:\\Media\\Missing.mkv";
    itemMissing.title = L"Missing Item";
    itemMissing.status = "new";
    lib.mediaItems.push_back(itemMissing);

    // Mock fileExists function: Missing.mkv does not exist, all others do
    auto mockFileExists = [](const std::wstring& path) -> bool {
        return path.find(L"Missing.mkv") == std::wstring::npos;
    };

    LibraryQueryEngine engine;

    // Filter: All (4 items)
    {
        LibraryQueryOptions opt;
        opt.statusFilter = LibraryStatusFilter::All;
        auto proj = engine.Query(lib, opt, mockFileExists);
        TEST_ASSERT_EQ(proj.filteredCount, 4);
        TEST_ASSERT_EQ(proj.items.size(), 4);
    }

    // Filter: Unwatched (New and Missing items)
    {
        LibraryQueryOptions opt;
        opt.statusFilter = LibraryStatusFilter::Unwatched;
        auto proj = engine.Query(lib, opt, mockFileExists);
        TEST_ASSERT_EQ(proj.filteredCount, 2);
    }

    // Filter: InProgress (1 item)
    {
        LibraryQueryOptions opt;
        opt.statusFilter = LibraryStatusFilter::InProgress;
        auto proj = engine.Query(lib, opt, mockFileExists);
        TEST_ASSERT_EQ(proj.filteredCount, 1);
        TEST_ASSERT_EQ(proj.items[0].id, "item_prog");
    }

    // Filter: Completed (1 item)
    {
        LibraryQueryOptions opt;
        opt.statusFilter = LibraryStatusFilter::Completed;
        auto proj = engine.Query(lib, opt, mockFileExists);
        TEST_ASSERT_EQ(proj.filteredCount, 1);
        TEST_ASSERT_EQ(proj.items[0].id, "item_done");
    }

    // Filter: Missing (1 item)
    {
        LibraryQueryOptions opt;
        opt.statusFilter = LibraryStatusFilter::Missing;
        auto proj = engine.Query(lib, opt, mockFileExists);
        TEST_ASSERT_EQ(proj.filteredCount, 1);
        TEST_ASSERT_EQ(proj.items[0].id, "item_missing");
        TEST_ASSERT(!proj.items[0].existsOnDisk);
    }
}

// ============================================================================
// 4. Bounded Projections & Cache Invalidation Tests
// ============================================================================
static void step_LibraryQuery_BoundedProjectionAndCache() {
    MediaLibrary lib;
    for (int i = 0; i < 20; ++i) {
        MediaItem item;
        item.id = "id_" + std::to_string(i);
        item.filePath = L"C:\\Media\\Item_" + std::to_wstring(i) + L".mkv";
        item.title = L"Item " + std::to_wstring(i);
        item.addedTimestamp = i * 10;
        lib.mediaItems.push_back(item);
    }
    lib.RebuildPathIndex();
    LibraryQueryEngine engine;

    // Query with bounded cap = 5
    LibraryQueryOptions opt;
    opt.maxItems = 5;
    opt.sortField = LibrarySortField::DateAdded;
    opt.sortDirection = SortDirection::Descending;

    auto proj1 = engine.Query(lib, opt);
    TEST_ASSERT_EQ(proj1.totalLibraryCount, 20);
    TEST_ASSERT_EQ(proj1.filteredCount, 20);
    TEST_ASSERT_EQ(proj1.items.size(), 5);
    TEST_ASSERT(proj1.isCapped);
    TEST_ASSERT_EQ(proj1.items[0].title, std::wstring(L"Item 19"));

    // Verify cache hit
    TEST_ASSERT(engine.IsCacheValid(lib, opt));

    // Changing options invalidates cache
    opt.maxItems = 10;
    TEST_ASSERT(!engine.IsCacheValid(lib, opt));
    auto proj2 = engine.Query(lib, opt);
    TEST_ASSERT_EQ(proj2.items.size(), 10);
    TEST_ASSERT(proj2.isCapped);

    // Modifying library updates version and invalidates cache
    lib.UpdateProgress(L"C:\\Media\\Item_0.mkv", 50.0, 100.0);
    TEST_ASSERT(!engine.IsCacheValid(lib, opt));

    auto proj3 = engine.Query(lib, opt);
    TEST_ASSERT(engine.IsCacheValid(lib, opt));
}

// ============================================================================
// 5. Conservative Next-Episode Sequence Tests
// ============================================================================
static void step_LibraryQuery_NextEpisodeDetection() {
    std::wstring ep1 = L"C:\\Videos\\Anime\\Frieren\\Frieren.S01E01.1080p.mkv";
    std::wstring ep2 = L"C:\\Videos\\Anime\\Frieren\\Frieren.S01E02.1080p.mkv";
    std::wstring ep3 = L"C:\\Videos\\Anime\\Frieren\\Frieren.S01E03.1080p.mkv";
    std::wstring epCrossDir = L"D:\\OtherFolder\\Frieren.S01E02.1080p.mkv";
    std::wstring epNonMedia = L"C:\\Videos\\Anime\\Frieren\\Frieren.S01E02.1080p.txt";

    // 1. Successful exact adjacent match (E01 -> E02)
    {
        std::vector<std::wstring> candidates = { ep1, ep2, ep3 };
        auto next = LibraryQueryEngine::FindNextEpisode(ep1, candidates);
        TEST_ASSERT(next.has_value());
        TEST_ASSERT_EQ(next->rawPath, ep2);
        TEST_ASSERT_EQ(next->episodeNumber, 2);
    }

    // 2. Strict Gap Detection (E01 -> E03 without E02)
    {
        std::vector<std::wstring> candidatesWithGap = { ep1, ep3 }; // E02 missing
        auto next = LibraryQueryEngine::FindNextEpisode(ep1, candidatesWithGap);
        TEST_ASSERT(!next.has_value()); // Conservative: does not jump over gaps!
    }

    // 3. Strict Cross-Directory Rejection
    {
        std::vector<std::wstring> crossDirCandidates = { ep1, epCrossDir };
        auto next = LibraryQueryEngine::FindNextEpisode(ep1, crossDirCandidates);
        TEST_ASSERT(!next.has_value()); // Different directory rejected!
    }

    // 4. Unsupported Extension Rejection
    {
        std::vector<std::wstring> nonMediaCandidates = { ep1, epNonMedia };
        auto next = LibraryQueryEngine::FindNextEpisode(ep1, nonMediaCandidates);
        TEST_ASSERT(!next.has_value()); // .txt rejected!
    }

    // 5. Ambiguity Rejection (multiple files match S01E02)
    {
        std::wstring ep2Duplicate = L"C:\\Videos\\Anime\\Frieren\\Frieren.S01E02.720p.mkv";
        std::vector<std::wstring> ambiguousCandidates = { ep1, ep2, ep2Duplicate };
        std::wstring ambiguityDetails;
        auto next = LibraryQueryEngine::FindNextEpisode(ep1, ambiguousCandidates, &ambiguityDetails);
        TEST_ASSERT(!next.has_value()); // Ambiguity must return nullopt!
        TEST_ASSERT(!ambiguityDetails.empty());
        TEST_ASSERT(ambiguityDetails.find(L"Ambiguous") != std::wstring::npos);
    }
}

// ============================================================================
// 6. Safe Action Projection & Autoplay Integration Tests
// ============================================================================
static void step_LibraryQuery_SafeActionProjection() {
    std::wstring ep1 = L"C:\\Shows\\Dark\\Dark.S01E01.mkv";
    std::wstring ep2 = L"C:\\Shows\\Dark\\Dark.S01E02.mkv";
    std::vector<std::wstring> candidates = { ep1, ep2 };

    auto mockFileExists = [](const std::wstring&) -> bool { return true; };

    // Case 1: Autoplay Enabled
    {
        Settings settings;
        settings.autoplay = true;

        auto action = LibraryQueryEngine::ProjectNextEpisodeAction(ep1, candidates, settings, mockFileExists);
        TEST_ASSERT_EQ(action.actionType, NextEpisodeActionType::PlayNextEpisode);
        TEST_ASSERT_EQ(action.nextFilePath, ep2);
        TEST_ASSERT(action.isAutoPlayAllowed);
        TEST_ASSERT(action.canExecuteExplicitly);
        TEST_ASSERT(action.canExecuteAuto);
    }

    // Case 2: Autoplay Disabled
    {
        Settings settings;
        settings.autoplay = false;

        auto action = LibraryQueryEngine::ProjectNextEpisodeAction(ep1, candidates, settings, mockFileExists);
        TEST_ASSERT_EQ(action.actionType, NextEpisodeActionType::PlayNextEpisode);
        TEST_ASSERT_EQ(action.nextFilePath, ep2);
        TEST_ASSERT(!action.isAutoPlayAllowed);
        TEST_ASSERT(action.canExecuteExplicitly); // Explicit button is still ready
        TEST_ASSERT(!action.canExecuteAuto);     // Auto execution blocked
    }

    // Case 3: Gap Detected
    {
        std::wstring ep3 = L"C:\\Shows\\Dark\\Dark.S01E03.mkv";
        std::vector<std::wstring> gapCandidates = { ep1, ep3 };
        Settings settings;
        settings.autoplay = true;

        auto action = LibraryQueryEngine::ProjectNextEpisodeAction(ep1, gapCandidates, settings, mockFileExists);
        TEST_ASSERT_EQ(action.actionType, NextEpisodeActionType::GapDetected);
        TEST_ASSERT(!action.canExecuteExplicitly);
        TEST_ASSERT(!action.canExecuteAuto);
    }

    // Case 4: Ambiguous
    {
        std::wstring ep2Dup = L"C:\\Shows\\Dark\\Dark.S01E02.4k.mkv";
        std::vector<std::wstring> ambigCandidates = { ep1, ep2, ep2Dup };
        Settings settings;

        auto action = LibraryQueryEngine::ProjectNextEpisodeAction(ep1, ambigCandidates, settings, mockFileExists);
        TEST_ASSERT_EQ(action.actionType, NextEpisodeActionType::Ambiguous);
        TEST_ASSERT(!action.canExecuteExplicitly);
        TEST_ASSERT(!action.canExecuteAuto);
    }

    // Case 5: Target file missing on disk
    {
        Settings settings;
        settings.autoplay = true;
        auto mockMissing = [](const std::wstring& path) -> bool {
            return path.find(L"Dark.S01E01.mkv") != std::wstring::npos;
        };

        auto action = LibraryQueryEngine::ProjectNextEpisodeAction(ep1, candidates, settings, mockMissing);
        TEST_ASSERT_EQ(action.actionType, NextEpisodeActionType::Disabled);
        TEST_ASSERT(!action.canExecuteExplicitly);
        TEST_ASSERT(!action.canExecuteAuto);
    }
}
static void step_LibraryQuery_FolderGrouping() {
    MediaLibrary lib;

    MediaItem item1;
    item1.id = "1";
    item1.filePath = L"C:\\Anime\\Frieren\\ep01.mkv";
    item1.title = L"Episode 1";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "2";
    item2.filePath = L"C:\\Anime\\Frieren\\ep02.mkv";
    item2.title = L"Episode 2";
    lib.mediaItems.push_back(item2);

    MediaItem item3;
    item3.id = "3";
    item3.filePath = L"C:\\Movies\\Inception.mkv";
    item3.title = L"Inception";
    lib.mediaItems.push_back(item3);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Folder;

    auto proj = engine.Query(lib, opt);
    TEST_ASSERT_EQ(proj.items.size(), 3);
    TEST_ASSERT_EQ(proj.groups.size(), 2);

    // Folders should be sorted alphabetically: "Frieren" then "Movies"
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Frieren"));
    TEST_ASSERT_EQ(proj.groups[0].count, 2);
    TEST_ASSERT_EQ(proj.groups[0].startIndex, 0);

    TEST_ASSERT_EQ(proj.groups[1].title, std::wstring(L"Movies"));
    TEST_ASSERT_EQ(proj.groups[1].count, 1);
    TEST_ASSERT_EQ(proj.groups[1].startIndex, 2);
}
static void step_LibraryQuery_SeriesGrouping() {
    MediaLibrary lib;

    MediaItem ep2;
    ep2.id = "ep2";
    ep2.filePath = L"C:\\Shows\\Dark\\Dark.S01E02.mkv";
    ep2.title = L"Dark S01E02";
    lib.mediaItems.push_back(ep2);

    MediaItem ep1;
    ep1.id = "ep1";
    ep1.filePath = L"C:\\Shows\\Dark\\Dark.S01E01.mkv";
    ep1.title = L"Dark S01E01";
    lib.mediaItems.push_back(ep1);

    MediaItem movie;
    movie.id = "movie1";
    movie.filePath = L"C:\\Movies\\Interstellar.mkv";
    movie.title = L"Interstellar";
    lib.mediaItems.push_back(movie);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Series;

    auto proj = engine.Query(lib, opt);
    TEST_ASSERT_EQ(proj.items.size(), 3);
    TEST_ASSERT_EQ(proj.groups.size(), 2);

    // Series group "Dark" should come first and episodes should be sorted S01E01, S01E02
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Dark"));
    TEST_ASSERT_EQ(proj.groups[0].count, 2);
    TEST_ASSERT_EQ(proj.groups[0].startIndex, 0);
    TEST_ASSERT(!proj.groups[0].isSpecial);
    TEST_ASSERT_EQ(proj.items[0].id, "ep1");
    TEST_ASSERT_EQ(proj.items[1].id, "ep2");

    // Standalone group should be second
    TEST_ASSERT(proj.groups[1].isSpecial);
    TEST_ASSERT_EQ(proj.groups[1].count, 1);
    TEST_ASSERT_EQ(proj.groups[1].startIndex, 2);
    TEST_ASSERT_EQ(proj.items[2].id, "movie1");
}
static void step_LibraryQuery_PinLastWatchedToTop() {
    MediaLibrary lib;
    MediaItem a;
    a.id = "itemA";
    a.filePath = L"C:\\Videos\\Alpha.mkv";
    a.title = L"Alpha";
    a.lastWatchedTimestamp = 100;
    lib.mediaItems.push_back(a);

    MediaItem b;
    b.id = "itemB";
    b.filePath = L"C:\\Videos\\Beta.mkv";
    b.title = L"Beta";
    b.lastWatchedTimestamp = 500;
    lib.mediaItems.push_back(b);

    MediaItem c;
    c.id = "itemC";
    c.filePath = L"C:\\Videos\\Charlie.mkv";
    c.title = L"Charlie";
    c.lastWatchedTimestamp = 0;
    lib.mediaItems.push_back(c);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Flat;
    opt.sortField = LibrarySortField::Title;
    opt.sortDirection = SortDirection::Ascending;

    // Without pinning: sorted alphabetically: Alpha, Beta, Charlie
    opt.pinLastWatched = false;
    auto projNormal = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projNormal.items.size(), 3);
    TEST_ASSERT_EQ(projNormal.items[0].id, "itemA");
    TEST_ASSERT_EQ(projNormal.items[1].id, "itemB");
    TEST_ASSERT_EQ(projNormal.items[2].id, "itemC");

    // With pinning: Beta has highest lastWatchedTimestamp, so it must be at index 0, followed by Alpha and Charlie
    opt.pinLastWatched = true;
    auto projPinned = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projPinned.items.size(), 3);
    TEST_ASSERT_EQ(projPinned.items[0].id, "itemB");
    TEST_ASSERT_EQ(projPinned.items[1].id, "itemA");
    TEST_ASSERT_EQ(projPinned.items[2].id, "itemC");
}
static void step_LibraryQuery_CollapsedFolderGroups() {
    MediaLibrary lib;
    MediaItem item1;
    item1.id = "f1_item1";
    item1.filePath = L"C:\\Movies\\SciFi\\Movie1.mkv";
    item1.title = L"Movie1";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "f2_item1";
    item2.filePath = L"C:\\Movies\\Action\\Movie2.mkv";
    item2.title = L"Movie2";
    lib.mediaItems.push_back(item2);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Folder;

    // Normal query (expanded): 2 groups, 2 items
    auto projExp = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projExp.groups.size(), 2);
    TEST_ASSERT_EQ(projExp.items.size(), 2);
    TEST_ASSERT(!projExp.groups[0].isCollapsed);
    TEST_ASSERT(!projExp.groups[1].isCollapsed);

    // Collapse first group (shared snapshot)
    auto collapsed = std::make_shared<std::unordered_set<std::wstring>>();
    collapsed->insert(projExp.groups[0].key);
    opt.collapsedGroups = std::const_pointer_cast<const std::unordered_set<std::wstring>>(collapsed);
    auto projCol = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projCol.groups.size(), 2);
    TEST_ASSERT(projCol.groups[0].isCollapsed);
    TEST_ASSERT(!projCol.groups[1].isCollapsed);
    // Group 0 count is still 1, but items in projection only has group 1's item
    TEST_ASSERT_EQ(projCol.groups[0].count, 1);
    TEST_ASSERT_EQ(projCol.items.size(), 1);
    TEST_ASSERT_EQ(projCol.items[0].id, "f1_item1");
}
static void step_LibraryQuery_SearchQueryFiltering() {
    MediaLibrary lib;
    MediaItem item1;
    item1.id = "item1";
    item1.filePath = L"C:\\Videos\\The.Matrix.1999.1080p.mkv";
    item1.title = L"The Matrix";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "item2";
    item2.filePath = L"C:\\Videos\\Interstellar.2014.mp4";
    item2.title = L"Interstellar";
    lib.mediaItems.push_back(item2);

    MediaItem item3;
    item3.id = "item3";
    item3.filePath = L"C:\\Videos\\Inception.2010.mkv";
    item3.title = L"Inception";
    lib.mediaItems.push_back(item3);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Flat;

    // 1. Empty query returns all items
    auto projAll = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projAll.items.size(), 3);

    // 2. Query matching title case-insensitively
    opt.searchQuery = L"matrix";
    auto projMatrix = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projMatrix.items.size(), 1);
    TEST_ASSERT_EQ(projMatrix.items[0].id, "item1");

    // 3. Query matching partial filename
    opt.searchQuery = L"2014";
    auto projYear = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projYear.items.size(), 1);
    TEST_ASSERT_EQ(projYear.items[0].id, "item2");

    // 4. Query with no matches returns 0 items
    opt.searchQuery = L"nonexistent_movie_query_xyz";
    auto projNone = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projNone.items.size(), 0);
    TEST_ASSERT_EQ(projNone.filteredCount, 0);

    // 5. Query matching multiple items ("in")
    opt.searchQuery = L"in"; // Matches "Interstellar" and "Inception"
    auto projMultiple = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projMultiple.items.size(), 2);
}
static void step_LibraryQuery_SearchQueryInSeriesAndFolders() {
    MediaLibrary lib;
    MediaItem item1;
    item1.id = "ep1";
    item1.filePath = L"C:\\Anime\\Steins Gate\\Steins.Gate.S01E01.mkv";
    item1.title = L"Steins;Gate Episode 1";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "ep2";
    item2.filePath = L"C:\\Anime\\Attack on Titan\\AoT.S01E01.mkv";
    item2.title = L"Attack on Titan Episode 1";
    lib.mediaItems.push_back(item2);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Series;

    // Filter by series title
    opt.searchQuery = L"steins";
    auto projSeries = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projSeries.items.size(), 1);
    TEST_ASSERT_EQ(projSeries.groups.size(), 1);
    TEST_ASSERT_EQ(projSeries.items[0].id, "ep1");

    // Clear search query restores all series
    opt.searchQuery = L"";
    auto projAllSeries = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projAllSeries.items.size(), 2);
    TEST_ASSERT_EQ(projAllSeries.groups.size(), 2);
}
static void step_LibraryQuery_StandaloneFiltersOutPlaylistItems() {
    MediaLibrary lib;

    MediaItem item1;
    item1.id = "pl_item_1";
    item1.filePath = L"C:\\Videos\\InPlaylist.mp4";
    item1.title = L"In Playlist Video";
    lib.mediaItems.push_back(item1);

    MediaItem item2;
    item2.id = "standalone_item";
    item2.filePath = L"C:\\Videos\\StandaloneMovie.mp4";
    item2.title = L"Standalone Movie";
    lib.mediaItems.push_back(item2);

    MediaItem item3;
    item3.id = "pl_item_2";
    item3.filePath = L"C:\\Videos\\Favorites\\FavVideo.mp4";
    item3.title = L"Favorite Video";
    lib.mediaItems.push_back(item3);

    // Create manual playlists containing item1 and item3
    Playlist pl1;
    pl1.id = "pl1";
    pl1.name = "My Playlist";
    pl1.itemPaths.push_back(L"C:\\Videos\\InPlaylist.mp4");
    lib.playlists.push_back(pl1);

    Playlist pl2;
    pl2.id = "pl2";
    pl2.name = "Favorites";
    pl2.itemPaths.push_back(L"C:\\Videos\\Favorites\\FavVideo.mp4");
    lib.playlists.push_back(pl2);

    LibraryQueryEngine engine;
    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Flat; // Standalone mode

    // In Standalone (Flat) mode, only item2 should be returned because item1 and item3 are in playlists
    auto projStandalone = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projStandalone.items.size(), 1);
    TEST_ASSERT_EQ(projStandalone.items[0].id, "standalone_item");
    TEST_ASSERT_EQ(projStandalone.filteredCount, 1);

    // In Series mode, grouping by series does not filter out playlist items
    opt.groupMode = LibraryGroupMode::Series;
    auto projSeries = engine.Query(lib, opt);
    TEST_ASSERT_EQ(projSeries.items.size(), 3);
}
static void step_LibraryQuery_DuplicateFilteringProbesEachFileOnce() {
    MediaLibrary library;
    for (int i = 0; i < 3; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Media\\copy" + std::to_wstring(i) + L".mkv";
        item.title = L"Same film";
        item.volumeSerialNumber = 10;
        item.fileIndex = 20;
        library.mediaItems.push_back(item);
    }
    LibraryQueryOptions options;
    options.groupMode = LibraryGroupMode::Duplicates;
    options.checkFileExists = true;
    LibraryQueryEngine engine;
    int probes = 0;
    const auto& projection = engine.Query(library, options, [&](const std::wstring&) { ++probes; return true; });
    TEST_ASSERT_EQ(probes, 3);
    TEST_ASSERT_EQ(projection.items.size(), size_t(3));
    TEST_ASSERT_EQ(projection.filteredCount, size_t(3));
    TEST_ASSERT_EQ(projection.groups.size(), size_t(1));
}
static void step_LibraryQuery_FolderOrderNormalizesOnceAndKeepsFirstDuplicate() {
    MediaLibrary library;
    for (int i = 0; i < 500; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = L"C:\\Media\\Folder" + std::to_wstring(i) + L"\\video.mkv";
        item.title = L"Video";
        library.mediaItems.push_back(item);
    }
    LibraryQueryOptions options;
    options.groupMode = LibraryGroupMode::Folder;
    auto folderOrder = std::make_shared<std::vector<std::wstring>>();
    for (int i = 499; i >= 0; --i) folderOrder->push_back(L"c:/MEDIA/Folder" + std::to_wstring(i) + L"/");
    folderOrder->push_back(L"C:\\Media\\Folder499");
    options.customFolderOrder = folderOrder;
    LibraryQueryEngine engine;
    const auto& projection = engine.Query(library, options);
    TEST_ASSERT_EQ(projection.groups.size(), size_t(500));
    for (size_t i = 0; i < projection.items.size(); ++i) {
        TEST_ASSERT_EQ(projection.items[i].originalLibraryIndex, size_t(499) - i);
    }
}
static void step_LibraryQuery_CappedSortPreservesStablePrefixAndPinning() {
    MediaLibrary library;
    for (int i = 199; i >= 0; --i) {
        MediaItem item;
        // Shared sort keys exercise secondary comparisons and recency ties.
        item.id = std::to_string(i / 2);
        item.filePath = L"C:\\Media\\Video" + std::to_wstring(i / 2) + L".mkv";
        item.title = L"Video " + std::to_wstring(i % 9);
        item.addedTimestamp = i / 4;
        item.lastWatchedTimestamp = i / 6;
        item.durationSec = 100 + i / 8;
        item.lastPosSec = i / 10;
        library.mediaItems.push_back(std::move(item));
    }
    library.mediaItems[103] = library.mediaItems[102];
    for (auto field : {LibrarySortField::Manual, LibrarySortField::Title, LibrarySortField::DateAdded,
                      LibrarySortField::LastPlayed, LibrarySortField::Duration, LibrarySortField::Progress}) {
        for (auto direction : {SortDirection::Ascending, SortDirection::Descending}) {
            for (bool pin : {false, true}) {
                LibraryQueryOptions options;
                options.sortField = field;
                options.sortDirection = direction;
                options.pinLastWatched = pin;
                LibraryQueryEngine engine;
                const auto full = engine.Query(library, options);
                for (size_t cap : {size_t(1), size_t(2), size_t(50), size_t(199), size_t(200), size_t(201)}) {
                    options.maxItems = cap;
                    const auto& limited = engine.Query(library, options);
                    TEST_ASSERT_EQ(limited.items.size(), (std::min)(cap, full.items.size()));
                    TEST_ASSERT_EQ(limited.filteredCount, full.filteredCount);
                    TEST_ASSERT_EQ(limited.isCapped, cap < full.items.size());
                    for (size_t i = 0; i < limited.items.size(); ++i)
                        TEST_ASSERT_EQ(limited.items[i].originalLibraryIndex, full.items[i].originalLibraryIndex);
                }
            }
        }
    }
}
static void step_LibraryQuery_SharedGroupLabelsRetainFirstSpellingAndOverrides() {
    MediaLibrary library;
    for (const auto& path : {L"C:\\Media\\Mixed\\Show S01E02.mkv", L"c:/media/mixed/Show S01E01.mkv",
                            L"Root.mkv", L"C:\\Other\\Standalone.mkv"}) {
        MediaItem item;
        item.filePath = path;
        library.mediaItems.push_back(std::move(item));
    }
    LibraryQueryEngine engine;
    LibraryQueryOptions options;
    options.groupMode = LibraryGroupMode::Folder;
    const auto folder = engine.Query(library, options);
    TEST_ASSERT_EQ(folder.groups.size(), size_t(3));
    TEST_ASSERT_EQ(folder.groups[0].title, std::wstring(L"Mixed"));
    TEST_ASSERT_EQ(folder.groups[0].subtitle, std::wstring(L"C:\\Media\\Mixed"));
    TEST_ASSERT_EQ(folder.groups[0].count, size_t(2));
    options.groupMode = LibraryGroupMode::Series;
    const auto series = engine.Query(library, options);
    TEST_ASSERT_EQ(series.groups.size(), size_t(2));
    TEST_ASSERT_EQ(series.groups[0].title, std::wstring(L"Show"));
    TEST_ASSERT_EQ(series.groups[1].title, std::wstring(L"Standalone Videos"));
    TEST_ASSERT_EQ(series.items[0].originalLibraryIndex, size_t(1));
    auto collapsed = std::make_shared<std::unordered_set<std::wstring>>();
    collapsed->insert(series.groups[0].key);
    options.collapsedGroups = collapsed;
    const auto& hidden = engine.Query(library, options);
    TEST_ASSERT_EQ(hidden.items.size(), size_t(2));
    TEST_ASSERT_EQ(hidden.groups[0].count, size_t(2));
    TEST_ASSERT(hidden.groups[0].isCollapsed);
    library.mediaItems[0].overrides.emplace();
    library.mediaItems[0].overrides->series = L"Custom Show";
    library.mediaItems[1].overrides.emplace();
    library.mediaItems[1].overrides->series = L"custom show";
    engine.InvalidateCache();
    options.collapsedGroups.reset();
    const auto& overridden = engine.Query(library, options);
    TEST_ASSERT_EQ(overridden.groups[0].title, std::wstring(L"Custom Show"));
    TEST_ASSERT_EQ(overridden.groups[0].key, std::wstring(L"custom show"));
    TEST_ASSERT_EQ(overridden.groups[0].subtitle, std::wstring(L"Series"));
    TEST_ASSERT_EQ(overridden.groups[0].count, size_t(2));
}
static void step_LibraryQuery_ViewReusesOrderingAndSearchAcrossInteractionAndUnrelatedEdits() {
    MediaLibrary library;
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    for (int i = 0; i < 6; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = LR"(C:\Media\Show S01E0)" + std::to_wstring(i + 1) + L".mkv";
        item.title = L"Episode " + std::to_wstring(i + 1);
        item.durationSec = 100;
        library.mediaItems.push_back(item);
    }
    library.RebuildPathIndex();
    LibraryQueryEngine engine;
    LibraryQueryOptions options;
    options.groupMode = LibraryGroupMode::Series;
    const auto first = engine.QueryView(library, options);
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 1ull);
    auto collapsed = std::make_shared<std::unordered_set<std::wstring>>();
    collapsed->insert(first.groups.front().key);
    options.collapsedGroups = collapsed;
    const auto& hidden = engine.QueryView(library, options);
    TEST_ASSERT(hidden.indices.empty());
    TEST_ASSERT_EQ(hidden.groups.front().count, size_t(6));
    TEST_ASSERT_EQ(hidden.groups.front().totalDurationSec, 600.0);
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 1ull);
    options.collapsedGroups.reset();
    options.searchQuery = L"EPISODE 2";
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.front(), size_t(1));
    options.searchQuery = L"Episode";
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.size(), size_t(6));
    TEST_ASSERT_EQ(engine.GetSearchTextBuildCount(), 1ull);
    const auto builds = engine.GetViewBuildCount();
    const auto generation = engine.QueryView(library, options).generation;
    library.NotifyPlaylistEdited();
    library.AddToHistory(library.mediaItems.front().filePath, 0, 100);
    library.UpdateProgress(library.mediaItems.front().filePath, 20, 100);
    TEST_ASSERT_EQ(engine.QueryView(library, options).generation, generation);
    TEST_ASSERT_EQ(engine.GetViewBuildCount(), builds);
    // The owned API still materializes the current progress from live records.
    TEST_ASSERT_EQ(engine.Query(library, options).items.front().lastPosSec, 20.0);
    options.statusFilter = LibraryStatusFilter::InProgress;
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.size(), size_t(1));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 1ull);
    TEST_ASSERT_EQ(engine.GetSearchTextBuildCount(), 1ull);
    MediaMetadataOverrides renamed; renamed.title = L"Renamed";
    TEST_ASSERT(library.SetMetadataOverrides(library.mediaItems.front().filePath, renamed));
    options.searchQuery = L"Renamed";
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.size(), size_t(1));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 2ull);
    TEST_ASSERT_EQ(engine.GetSearchTextBuildCount(), 2ull);
}
static void step_LibraryQuery_PreparedViewsMatchDirectProjectionWithFilteringGroupsAndCaps() {
    MediaLibrary library;
    for (int i = 0; i < 12; ++i) {
        MediaItem item;
        item.id = std::to_string(i);
        item.filePath = LR"(C:\Media\Folder)" + std::to_wstring(i % 3) + LR"(\Show S01E)" + std::to_wstring(i + 1) + L".mkv";
        item.title = L"Episode " + std::to_wstring(i);
        item.durationSec = 100; item.fileSizeBytes = 1024;
        item.lastPosSec = i % 3 == 0 ? 100 : (i % 3 == 1 ? 25 : 0);
        item.status = i % 3 == 0 ? "done" : (i % 3 == 1 ? "in_progress" : "new");
        item.lastWatchedTimestamp = i % 3 == 2 ? 0 : i + 1;
        item.volumeSerialNumber = 10; item.fileIndex = i / 3 + 1;
        if (i % 4 == 0) { item.overrides.emplace(); item.overrides->title = L"Custom title"; item.overrides->series = L"Custom Series"; }
        library.mediaItems.push_back(item);
    }
    Playlist playlist; playlist.id = "manual"; playlist.itemPaths.push_back(library.mediaItems[0].filePath);
    library.playlists.push_back(playlist);
    library.RebuildPathIndex();
    for (auto mode : {LibraryGroupMode::Flat, LibraryGroupMode::Folder, LibraryGroupMode::Series, LibraryGroupMode::Duplicates}) {
        LibraryQueryEngine engine;
        LibraryQueryOptions options; options.groupMode = mode;
        const auto all = engine.QueryView(library, options);
        auto collapsed = std::make_shared<std::unordered_set<std::wstring>>();
        if (!all.groups.empty()) collapsed->insert(all.groups.front().key);
        for (int scenario = 0; scenario < 24; ++scenario) {
            options.sortField = static_cast<LibrarySortField>(scenario % 6);
            options.sortDirection = scenario % 2 ? SortDirection::Descending : SortDirection::Ascending;
            options.searchQuery = scenario % 4 == 0 ? L"episode" : (scenario % 4 == 1 ? L"custom" : L"");
            options.statusFilter = static_cast<LibraryStatusFilter>(scenario % 4);
            options.maxItems = scenario % 3 == 0 ? 2 : 0;
            options.pinLastWatched = scenario % 2 != 0;
            options.collapsedGroups = scenario % 3 == 1 ? collapsed : nullptr;
            const auto actual = engine.QueryView(library, options);
            // Disk-sensitive execution uses the direct builder without a plan.
            auto directOptions = options; directOptions.checkFileExists = true;
            LibraryQueryEngine direct;
            const auto& expected = direct.QueryView(library, directOptions, [](const std::wstring&) { return true; });
            TEST_ASSERT(actual.indices == expected.indices);
            TEST_ASSERT_EQ(actual.filteredCount, expected.filteredCount);
            TEST_ASSERT_EQ(actual.isCapped, expected.isCapped);
            TEST_ASSERT_EQ(actual.groups.size(), expected.groups.size());
            for (size_t i = 0; i < actual.groups.size(); ++i) {
                const auto& a = actual.groups[i]; const auto& b = expected.groups[i];
                TEST_ASSERT_EQ(a.key, b.key); TEST_ASSERT_EQ(a.title, b.title); TEST_ASSERT_EQ(a.subtitle, b.subtitle);
                TEST_ASSERT_EQ(a.startIndex, b.startIndex); TEST_ASSERT_EQ(a.count, b.count);
                TEST_ASSERT_EQ(a.isCollapsed, b.isCollapsed);
                TEST_ASSERT_EQ(a.totalSizeBytes, b.totalSizeBytes); TEST_ASSERT_EQ(a.totalDurationSec, b.totalDurationSec);
            }
        }
    }
}
static void step_LibraryQuery_RelevantChangesInvalidateAndUnchangedOrderRetainsLayoutGeneration() {
    MediaLibrary library;
    library.SetSaveRequestHandler([] { return MediaLibrarySaveResult{}; });
    for (int i = 0; i < 2; ++i) {
        MediaItem item; item.id = std::to_string(i); item.title = std::to_wstring(i);
        item.filePath = LR"(C:\Media\)" + item.title + L".mkv"; item.durationSec = 100;
        library.mediaItems.push_back(item);
    }
    library.RebuildPathIndex();
    LibraryQueryEngine engine; LibraryQueryOptions options; options.pinLastWatched = true;
    const auto initial = engine.QueryView(library, options);
    library.UpdateProgress(library.mediaItems[0].filePath, 10, 100);
    TEST_ASSERT_EQ(engine.QueryView(library, options).generation, initial.generation);
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 1ull);
    Playlist playlist; playlist.id = "manual"; playlist.itemPaths.push_back(library.mediaItems[0].filePath);
    library.playlists.push_back(playlist); library.NotifyPlaylistEdited();
    TEST_ASSERT((engine.QueryView(library, options).indices == std::vector<size_t>{1}));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 2ull);
    options.groupMode = LibraryGroupMode::Folder; options.sortField = LibrarySortField::Progress;
    engine.QueryView(library, options);
    library.UpdateProgress(library.mediaItems[1].filePath, 50, 100);
    const auto& sorted = engine.QueryView(library, options);
    TEST_ASSERT((sorted.indices == std::vector<size_t>{0, 1}));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 4ull);
    const auto oldGeneration = sorted.generation;
    std::swap(library.mediaItems[0], library.mediaItems[1]); library.RebuildPathIndex();
    const auto& reordered = engine.QueryView(library, options);
    TEST_ASSERT((reordered.indices == std::vector<size_t>{1, 0}));
    TEST_ASSERT(reordered.generation != oldGeneration);
}
static void step_LibraryQuery_ColdCappedViewDoesNotPrepareUnlimitedOrder() {
    MediaLibrary library;
    for (int i = 9; i >= 0; --i) {
        MediaItem item; item.id = std::to_string(i); item.title = std::to_wstring(i);
        library.mediaItems.push_back(item);
    }
    LibraryQueryEngine engine; LibraryQueryOptions options; options.maxItems = 1;
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.front(), size_t(9));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 0ull);
    options.maxItems = 0; engine.QueryView(library, options);
    options.maxItems = 2;
    TEST_ASSERT_EQ(engine.QueryView(library, options).indices.size(), size_t(2));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 1ull);
}
static void step_LibraryQuery_SeriesOverrideCanShareStandaloneKeyWhenFiltering() {
    MediaLibrary library;
    MediaItem ordinary; ordinary.id = "ordinary"; ordinary.title = L"Ordinary"; ordinary.filePath = LR"(C:\Media\film.mkv)";
    library.mediaItems.push_back(ordinary);
    MediaItem custom; custom.id = "custom"; custom.title = L"Custom"; custom.filePath = LR"(C:\Media\Show S01E01.mkv)";
    custom.overrides.emplace(); custom.overrides->series = L"__standalone__"; library.mediaItems.push_back(custom);
    LibraryQueryOptions options; options.groupMode = LibraryGroupMode::Series;
    LibraryQueryEngine engine; engine.QueryView(library, options);
    options.searchQuery = L"Custom";
    const auto& filtered = engine.QueryView(library, options);
    TEST_ASSERT_EQ(filtered.groups.size(), size_t(1));
    TEST_ASSERT(!filtered.groups.front().isSpecial);
    TEST_ASSERT_EQ(filtered.groups.front().title, std::wstring(L"__standalone__"));
    TEST_ASSERT_EQ(filtered.indices.front(), size_t(1));
}


TEST_CASE(LibraryQuery, SortingGroupingAndCappedProjection) {
    step_LibraryQuery_BorrowedSearchMatchesOwningReference();
    step_LibraryQuery_CappedGroupedProjectionKeepsVisiblePrefixAndSummaries();
    step_LibraryQuery_GroupingParserRetainsEpisodeKeysAndPublicSourceFields();
    step_LibraryQuery_SupportedMediaExtensionPathSemantics();
    step_LibraryQuery_EpisodeParsingPatterns();
    step_LibraryQuery_SortingByAllFields();
    step_LibraryQuery_StableDeterministicTieBreaking();
    step_LibraryQuery_StatusFilters();
    step_LibraryQuery_BoundedProjectionAndCache();
    step_LibraryQuery_NextEpisodeDetection();
    step_LibraryQuery_SafeActionProjection();
    step_LibraryQuery_FolderGrouping();
    step_LibraryQuery_SeriesGrouping();
    step_LibraryQuery_PinLastWatchedToTop();
    step_LibraryQuery_CollapsedFolderGroups();
    step_LibraryQuery_SearchQueryFiltering();
    step_LibraryQuery_SearchQueryInSeriesAndFolders();
    step_LibraryQuery_StandaloneFiltersOutPlaylistItems();
    step_LibraryQuery_DuplicateFilteringProbesEachFileOnce();
    step_LibraryQuery_FolderOrderNormalizesOnceAndKeepsFirstDuplicate();
    step_LibraryQuery_CappedSortPreservesStablePrefixAndPinning();
    step_LibraryQuery_SharedGroupLabelsRetainFirstSpellingAndOverrides();
    step_LibraryQuery_ViewReusesOrderingAndSearchAcrossInteractionAndUnrelatedEdits();
    step_LibraryQuery_PreparedViewsMatchDirectProjectionWithFilteringGroupsAndCaps();
    step_LibraryQuery_RelevantChangesInvalidateAndUnchangedOrderRetainsLayoutGeneration();
    step_LibraryQuery_ColdCappedViewDoesNotPrepareUnlimitedOrder();
    step_LibraryQuery_SeriesOverrideCanShareStandaloneKeyWhenFiltering();
}

} // namespace Aperture::Test
