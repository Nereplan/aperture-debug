#pragma once

#include "../support/TestFramework.h"
#include "../../src/LibraryQuery.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Common.h"

#include <vector>
#include <string>
#include <algorithm>
#include <random>

namespace Aperture::Test {
static void step_DuplicateDetection_ContentClustersMatchIndependentPairwiseOracle() {
    std::mt19937 random(0xA9E274);
    const double durations[] = {0.0, 0.5, 0.75, 1.0, 1.25, 4.5, 4.75, 4.999, 5.0, 5.25, 5.5, 5.5001, 8.0, 8.5, 9.0};
    for (int sample = 0; sample < 128; ++sample) {
        std::vector<MediaItem> items(96);
        for (size_t i = 0; i < items.size(); ++i) {
            auto& item = items[i];
            item.id = std::to_string(i);
            item.filePath = L"C:\\Oracle\\" + std::to_wstring(i) + L"\\clip" + std::to_wstring(random() % 7) + L".mkv";
            item.title = L"Title " + std::to_wstring(random() % 5);
            item.fileSizeBytes = random() % 5 == 0 ? 0 : 1000 + random() % 3;
            item.durationSec = durations[random() % std::size(durations)];
            if (sample % 4 == 0) item.durationSec = 4.0 + (random() % 5) * 0.25;
            if (sample % 4 == 1) item.durationSec = 1200.0 + (random() % 5) * 0.25;
            const unsigned resolution = random() % 5;
            item.videoWidth = resolution == 0 ? -1 : (resolution == 1 ? 1280 : 1920);
            item.videoHeight = resolution < 2 ? 0 : (resolution == 2 ? 1080 : 720);
            if (i % 11 == 0) item.title.clear();
        }
        struct Keys { std::wstring stem, title, file; };
        std::vector<Keys> keys;
        for (const auto& item : items) {
            const auto name = GetFileNameFromPath(item.filePath);
            const auto dot = name.find_last_of(L'.');
            keys.push_back({LibraryQueryEngine::ExtractSeriesStem(name),
                LibraryQueryEngine::NormalizeSearchText(item.title.empty() ? name : item.title),
                LibraryQueryEngine::NormalizeSearchText(dot != std::wstring::npos && dot > 0 ? name.substr(0, dot) : name)});
        }
        // Brute-force edges followed by graph traversal are independent of the
        // production duration windows and disjoint-set implementation.
        std::vector<std::vector<size_t>> edges(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                const auto& a = items[i]; const auto& b = items[j];
                if (!a.fileSizeBytes || a.fileSizeBytes != b.fileSizeBytes || a.durationSec <= 0.5 || b.durationSec <= 0.5 ||
                    std::abs(a.durationSec - b.durationSec) > 0.5) continue;
                const bool known = a.videoWidth > 0 && a.videoHeight > 0 && b.videoWidth > 0 && b.videoHeight > 0;
                if (known && (a.videoWidth != b.videoWidth || a.videoHeight != b.videoHeight)) continue;
                const bool matches = (known && a.durationSec >= 5.0) ||
                    (!keys[i].stem.empty() && keys[i].stem == keys[j].stem) ||
                    (!keys[i].title.empty() && keys[i].title == keys[j].title) ||
                    (!keys[i].file.empty() && keys[i].file == keys[j].file);
                if (matches) { edges[i].push_back(j); edges[j].push_back(i); }
            }
        }
        std::vector<bool> visited(items.size());
        std::vector<std::vector<size_t>> expected;
        for (size_t i = 0; i < items.size(); ++i) {
            if (visited[i]) continue;
            std::vector<size_t> component{i}; visited[i] = true;
            for (size_t next = 0; next < component.size(); ++next) {
                for (const size_t neighbor : edges[component[next]]) {
                    if (!visited[neighbor]) { visited[neighbor] = true; component.push_back(neighbor); }
                }
            }
            if (component.size() > 1) {
                std::sort(component.begin(), component.end());
                expected.push_back(std::move(component));
            }
        }
        const auto result = LibraryQueryEngine::DetectDuplicates(items);
        std::vector<std::vector<size_t>> actual;
        size_t duplicateEntries = 0;
        uint64_t duplicateBytes = 0;
        for (const auto& group : result.groups) {
            TEST_ASSERT(group.matchKind == DuplicateMatchKind::LikelyContent);
            for (size_t i = 0; i < group.items.size(); ++i) TEST_ASSERT(group.items[i] == &items[group.originalIndices[i]]);
            auto indices = group.originalIndices;
            std::sort(indices.begin(), indices.end()); actual.push_back(std::move(indices));
            duplicateEntries += group.items.size() - 1;
            duplicateBytes += (group.items.size() - 1) * group.items.front()->fileSizeBytes;
        }
        std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
        TEST_ASSERT(actual == expected);
        TEST_ASSERT_EQ(result.totalDuplicateEntries, duplicateEntries);
        TEST_ASSERT_EQ(result.totalReclaimableBytes, duplicateBytes);
    }
}
static void step_DuplicateDetection_UnknownResolutionBridgesBothDurationNeighbors() {
    std::vector<MediaItem> items(4);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].id = std::to_string(i);
        items[i].filePath = LR"(C:\Bridge\)" + std::to_wstring(i) + L".mkv";
        items[i].title = L"Shared"; items[i].fileSizeBytes = 1000;
        items[i].durationSec = i == 0 ? 6.0 : (i == 1 ? 6.9 : 6.45);
        items[i].videoWidth = i < 2 ? 0 : (i == 2 ? 1920 : 1280);
        items[i].videoHeight = i < 2 ? 0 : (i == 2 ? 1080 : 720);
    }
    const auto bridged = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(bridged.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(bridged.totalDuplicateEntries, 3);
    // With no wildcard records, equal text cannot bridge different known dimensions.
    items.erase(items.begin(), items.begin() + 2);
    TEST_ASSERT_EQ(LibraryQueryEngine::DetectDuplicates(items).totalDuplicateGroups, 0);
}
static void step_DuplicateDetection_MetadataChainsAndFiveSecondInputOrder() {
    std::vector<MediaItem> items(3);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].id = std::to_string(i);
        items[i].filePath = L"C:\\Chain\\" + std::to_wstring(i) + L".mkv";
        items[i].title = L"Distinct " + std::to_wstring(i);
        items[i].fileSizeBytes = 1000;
        items[i].videoWidth = 1920; items[i].videoHeight = 1080;
        items[i].durationSec = 8.0 - i * 0.5;
    }
    auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 2);
    items.resize(2);
    items[0].durationSec = 4.75; items[1].durationSec = 5.0;
    TEST_ASSERT_EQ(LibraryQueryEngine::DetectDuplicates(items).totalDuplicateGroups, 0);
    std::swap(items[0], items[1]);
    TEST_ASSERT_EQ(LibraryQueryEngine::DetectDuplicates(items).totalDuplicateGroups, 1);

    // Equal titles/keys must not let union order choose the group display order.
    items.resize(4);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].id = std::to_string(i);
        items[i].title = L"Same title";
        items[i].filePath = L"C:\\Collision\\" + std::to_wstring(3 - i) + L".mkv";
        items[i].durationSec = 60.0; items[i].fileSizeBytes = 1000;
        items[i].videoWidth = i < 2 ? 1920 : 1280;
        items[i].videoHeight = i < 2 ? 1080 : 720;
    }
    result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 2);
    const auto firstKeeper = result.groups.front().items.front()->filePath;
    std::reverse(items.begin(), items.end());
    result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 2);
    TEST_ASSERT_EQ(result.groups.front().items.front()->filePath, firstKeeper);
}
static void step_DuplicateDetection_RepeatedComparisonsAndMetadataChanges() {
    std::vector<MediaItem> items(24);
    for (size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        item.id = std::to_string(i);
        item.filePath = L"C:\\Synthetic\\clip" + std::to_wstring(i) + L".mkv";
        item.title = i % 2 == 0 ? L" Shared   Title " : L"shared title";
        item.fileSizeBytes = 100000;
        item.durationSec = 4.0;
    }

    auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, items.size() - 1);
    TEST_ASSERT_EQ(result.groups[0].items.size(), items.size());

    // A new scan must use edited titles, even when paths and sizes are unchanged.
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].title = L"Distinct title " + std::to_wstring(i);
    }
    result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 0);
}
static void step_DuplicateDetection_LargeDistinctSizeLibraryKeepsOnlyMatchingPair() {
    std::vector<MediaItem> items(25000);
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].id = std::to_string(i);
        items[i].filePath = L"C:\\Synthetic\\" + std::to_wstring(i) + L".mkv";
        items[i].title = L"Same title";
        items[i].fileSizeBytes = 100000 + i;
        items[i].durationSec = 60.0;
    }
    items.back().fileSizeBytes = items.front().fileSizeBytes;
    const auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 1);
    TEST_ASSERT_EQ(result.groups[0].items.size(), 2);
}

// ============================================================================
// 1. Exact Windows File Identity Tests
// ============================================================================
static void step_DuplicateDetection_ExactFileIdentityDetection() {
    std::vector<MediaItem> items;
    
    // Item 1: Original played movie
    MediaItem m1;
    m1.id = "id_movie_1";
    m1.filePath = L"C:\\Media\\Inception (2010).mkv";
    m1.title = L"Inception";
    m1.volumeSerialNumber = 0x12345678;
    m1.fileIndex = 0x000100000000002A;
    m1.fileSizeBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    m1.durationSec = 8880.0;
    m1.lastPosSec = 4000.0;
    m1.status = "in_progress";
    m1.addedTimestamp = 1000;
    items.push_back(m1);

    // Item 2: Hard link or secondary path to exact same file identity
    MediaItem m2;
    m2.id = "id_movie_2";
    m2.filePath = L"D:\\Hardlinks\\Inception_Link.mkv";
    m2.title = L"Inception Link";
    m2.volumeSerialNumber = 0x12345678;
    m2.fileIndex = 0x000100000000002A;
    m2.fileSizeBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    m2.durationSec = 8880.0;
    m2.lastPosSec = 0.0;
    m2.status = "new";
    m2.addedTimestamp = 2000;
    items.push_back(m2);

    // Item 3: Independent unrelated file
    MediaItem m3;
    m3.id = "id_other";
    m3.filePath = L"C:\\Media\\Interstellar.mkv";
    m3.title = L"Interstellar";
    m3.volumeSerialNumber = 0x12345678;
    m3.fileIndex = 0x0001000000000099;
    m3.fileSizeBytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
    m3.durationSec = 10140.0;
    items.push_back(m3);

    auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 1);
    TEST_ASSERT_EQ(result.groups.size(), 1);

    const auto& grp = result.groups[0];
    TEST_ASSERT_EQ(static_cast<int>(grp.matchKind), static_cast<int>(DuplicateMatchKind::ExactFileIdentity));
    TEST_ASSERT_EQ(grp.items.size(), 2);
    // Suggested keeper should be m1 (in_progress with higher watch progress)
    TEST_ASSERT_EQ(grp.items[0]->id, std::string("id_movie_1"));
    TEST_ASSERT_EQ(grp.items[1]->id, std::string("id_movie_2"));
    TEST_ASSERT_EQ(grp.totalDuplicateSizeBytes, 4ULL * 1024ULL * 1024ULL * 1024ULL);
}

// ============================================================================
// 2. Exact Path Alias Duplicate Detection Tests
// ============================================================================
static void step_DuplicateDetection_ExactPathAliasDetection() {
    std::vector<MediaItem> items;

    // Item 1: Backslash path
    MediaItem m1;
    m1.id = "id_path_1";
    m1.filePath = L"C:\\Media\\Anime\\Frieren_01.mkv";
    m1.title = L"Frieren 01";
    m1.volumeSerialNumber = 0; // Missing or offline identity
    m1.fileIndex = 0;
    m1.fileSizeBytes = 700ULL * 1024ULL * 1024ULL;
    m1.durationSec = 1440.0;
    m1.addedTimestamp = 500;
    items.push_back(m1);

    // Item 2: Forward-slash lowercase alias to exact same normalized location
    MediaItem m2;
    m2.id = "id_path_2";
    m2.filePath = L"c:/media/anime/frieren_01.mkv";
    m2.title = L"frieren_01";
    m2.volumeSerialNumber = 0;
    m2.fileIndex = 0;
    m2.fileSizeBytes = 700ULL * 1024ULL * 1024ULL;
    m2.durationSec = 1440.0;
    m2.addedTimestamp = 1500;
    items.push_back(m2);

    auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 1);

    const auto& grp = result.groups[0];
    TEST_ASSERT_EQ(static_cast<int>(grp.matchKind), static_cast<int>(DuplicateMatchKind::ExactPathAlias));
    TEST_ASSERT_EQ(grp.items.size(), 2);
    // Earlier added item kept as suggested primary
    TEST_ASSERT_EQ(grp.items[0]->id, std::string("id_path_1"));
}

// ============================================================================
// 3. Conservative Content Duplicate Detection Tests
// ============================================================================
static void step_DuplicateDetection_ConservativeContentMatching() {
    std::vector<MediaItem> items;

    // Item 1: Video file in Downloads
    MediaItem m1;
    m1.id = "id_dl";
    m1.filePath = L"C:\\Users\\User\\Downloads\\Spirited.Away.1080p.mkv";
    m1.title = L"Spirited Away";
    m1.volumeSerialNumber = 0x1111;
    m1.fileIndex = 0xAAAA;
    m1.fileSizeBytes = 2500000000ULL;
    m1.durationSec = 7500.2;
    m1.videoWidth = 1920;
    m1.videoHeight = 1080;
    m1.addedTimestamp = 3000;
    items.push_back(m1);

    // Item 2: Same video file in Movies folder (different volume/index, e.g. copied to separate drive)
    MediaItem m2;
    m2.id = "id_mov";
    m2.filePath = L"E:\\Movies\\Studio Ghibli\\Spirited.Away.mkv";
    m2.title = L"Spirited Away (2001)";
    m2.volumeSerialNumber = 0x2222;
    m2.fileIndex = 0xBBBB;
    m2.fileSizeBytes = 2500000000ULL;
    m2.durationSec = 7500.0;
    m2.videoWidth = 1920;
    m2.videoHeight = 1080;
    m2.status = "done";
    m2.lastPosSec = 7500.0;
    m2.addedTimestamp = 1000;
    items.push_back(m2);

    auto result = LibraryQueryEngine::DetectDuplicates(items);
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 1);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 1);

    const auto& grp = result.groups[0];
    TEST_ASSERT_EQ(static_cast<int>(grp.matchKind), static_cast<int>(DuplicateMatchKind::LikelyContent));
    TEST_ASSERT_EQ(grp.items.size(), 2);
    // Watched movie in Movies folder preferred as keeper
    TEST_ASSERT_EQ(grp.items[0]->id, std::string("id_mov"));
}

// ============================================================================
// 4. Ambiguity Safety & Rejection Tests
// ============================================================================
static void step_DuplicateDetection_AmbiguitySafetyAndRejection() {
    std::vector<MediaItem> items;

    // Two zero-byte / unprobed items with distinct paths must NOT be grouped as content duplicates
    MediaItem m1;
    m1.id = "id_zero_1";
    m1.filePath = L"C:\\Media\\Missing1.mkv";
    m1.fileSizeBytes = 0;
    m1.durationSec = 0.0;
    items.push_back(m1);

    MediaItem m2;
    m2.id = "id_zero_2";
    m2.filePath = L"C:\\Media\\Missing2.mkv";
    m2.fileSizeBytes = 0;
    m2.durationSec = 0.0;
    items.push_back(m2);

    // Two items with same size but different duration (> 0.5s) must NOT match
    MediaItem m3;
    m3.id = "id_size_same_1";
    m3.filePath = L"C:\\Media\\Show_S01E01.mkv";
    m3.fileSizeBytes = 1000000000ULL;
    m3.durationSec = 1200.0;
    items.push_back(m3);

    MediaItem m4;
    m4.id = "id_size_same_2";
    m4.filePath = L"C:\\Media\\Show_S01E02.mkv";
    m4.fileSizeBytes = 1000000000ULL; // Same size by coincidence
    m4.durationSec = 1350.0;          // Different duration
    items.push_back(m4);

    // Two items with same duration but different size must NOT match
    MediaItem m5;
    m5.id = "id_dur_same_1";
    m5.filePath = L"C:\\Media\\ClipA.mp4";
    m5.fileSizeBytes = 50000000ULL;
    m5.durationSec = 60.0;
    items.push_back(m5);

    MediaItem m6;
    m6.id = "id_dur_same_2";
    m6.filePath = L"C:\\Media\\ClipB.mp4";
    m6.fileSizeBytes = 80000000ULL;
    m6.durationSec = 60.0;
    items.push_back(m6);

    auto result = LibraryQueryEngine::DetectDuplicates(items);
    // None of these should match as duplicates
    TEST_ASSERT_EQ(result.totalDuplicateGroups, 0);
    TEST_ASSERT_EQ(result.totalDuplicateEntries, 0);
    TEST_ASSERT(result.groups.empty());
}

// ============================================================================
// 5. LibraryQueryEngine Query with LibraryGroupMode::Duplicates
// ============================================================================
static void step_DuplicateDetection_LibraryQueryProjectionDuplicatesMode() {
    MediaLibrary lib;
    LibraryQueryEngine engine;

    // Add 3 items: 2 exact duplicates, 1 independent
    const std::wstring p1 = L"C:\\Media\\VideoA.mp4";
    const std::wstring p2 = L"C:\\Media\\Copies\\VideoA_Copy.mp4";
    const std::wstring p3 = L"C:\\Media\\VideoB.mp4";

    lib.AddOrUpdateMedia(p1);
    lib.AddOrUpdateMedia(p2);
    lib.AddOrUpdateMedia(p3);

    auto* m1 = lib.FindByPath(p1);
    auto* m2 = lib.FindByPath(p2);
    auto* m3 = lib.FindByPath(p3);

    m1->volumeSerialNumber = 0x9999;
    m1->fileIndex = 0x1234;
    m1->fileSizeBytes = 1024 * 1024 * 50;
    m1->durationSec = 300.0;

    m2->volumeSerialNumber = 0x9999;
    m2->fileIndex = 0x1234;
    m2->fileSizeBytes = 1024 * 1024 * 50;
    m2->durationSec = 300.0;

    m3->volumeSerialNumber = 0x9999;
    m3->fileIndex = 0x5678;
    m3->fileSizeBytes = 1024 * 1024 * 80;
    m3->durationSec = 450.0;

    LibraryQueryOptions opt;
    opt.groupMode = LibraryGroupMode::Duplicates;

    auto proj = engine.Query(lib, opt, [](const std::wstring&) { return true; });
    TEST_ASSERT_EQ(proj.groups.size(), 1);
    TEST_ASSERT_EQ(proj.items.size(), 2);
    TEST_ASSERT_EQ(proj.groups[0].count, 2);
    TEST_ASSERT_EQ(proj.groups[0].startIndex, 0);

    // Verify displayName helper
    TEST_ASSERT(GetGroupModeDisplayName(LibraryGroupMode::Duplicates) == std::wstring(L"Duplicates"));
}

// ============================================================================
// 6. Safe Removal Preserves Disk Files and Reference Integrity
// ============================================================================
static void step_DuplicateDetection_SafeRemovalPreservesIntegrity() {
    MediaLibrary lib;

    const std::wstring pathOriginal = L"C:\\Media\\Original.mp4";
    const std::wstring pathDup = L"C:\\Media\\Duplicate.mp4";

    lib.AddOrUpdateMedia(pathOriginal);
    lib.AddOrUpdateMedia(pathDup);

    std::string origId = lib.FindByPath(pathOriginal)->id;
    std::string dupId = lib.FindByPath(pathDup)->id;

    // Create a playlist containing both
    Playlist* pl = lib.CreatePlaylist("MyPlaylist");
    TEST_ASSERT(pl != nullptr);
    lib.AddToPlaylist(pl->id, { pathOriginal, pathDup });

    // Add original to history
    lib.AddToHistory(pathOriginal, 120.0, 300.0);

    // Remove only the duplicate record
    size_t skipped = 0;
    size_t removed = lib.RemoveMedia({ dupId }, &skipped);
    TEST_ASSERT_EQ(removed, 1);
    TEST_ASSERT_EQ(skipped, 0);

    // Verify original is still intact in library
    TEST_ASSERT(lib.FindById(origId) != nullptr);
    TEST_ASSERT(lib.FindById(dupId) == nullptr);

    // Verify playlist still holds original, and duplicate reference was safely cleaned up
    Playlist* plCheck = lib.FindPlaylist(pl->id);
    TEST_ASSERT(plCheck != nullptr);
    TEST_ASSERT_EQ(plCheck->itemPaths.size(), 1);
    TEST_ASSERT_EQ(plCheck->itemPaths[0], pathOriginal);

    // Verify history retained
    TEST_ASSERT_EQ(lib.history.size(), 1);
    TEST_ASSERT_EQ(lib.history[0].filePath, pathOriginal);
}


TEST_CASE(DuplicateDetection, ClusteringOracleAndSafeRemoval) {
    step_DuplicateDetection_ContentClustersMatchIndependentPairwiseOracle();
    step_DuplicateDetection_UnknownResolutionBridgesBothDurationNeighbors();
    step_DuplicateDetection_MetadataChainsAndFiveSecondInputOrder();
    step_DuplicateDetection_RepeatedComparisonsAndMetadataChanges();
    step_DuplicateDetection_LargeDistinctSizeLibraryKeepsOnlyMatchingPair();
    step_DuplicateDetection_ExactFileIdentityDetection();
    step_DuplicateDetection_ExactPathAliasDetection();
    step_DuplicateDetection_ConservativeContentMatching();
    step_DuplicateDetection_AmbiguitySafetyAndRejection();
    step_DuplicateDetection_LibraryQueryProjectionDuplicatesMode();
    step_DuplicateDetection_SafeRemovalPreservesIntegrity();
}

} // namespace Aperture::Test
