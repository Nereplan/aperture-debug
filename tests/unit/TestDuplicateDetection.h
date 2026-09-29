#pragma once

#include "../support/TestFramework.h"
#include "../../src/LibraryQuery.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Common.h"

#include <vector>
#include <string>
#include <algorithm>

namespace Aperture::Test {

TEST_CASE(DuplicateDetection, RepeatedComparisonsAndMetadataChanges) {
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

TEST_CASE(DuplicateDetection, LargeDistinctSizeLibraryKeepsOnlyMatchingPair) {
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

TEST_CASE(DuplicateDetection, ExactFileIdentityDetection) {
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

TEST_CASE(DuplicateDetection, ExactPathAliasDetection) {
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

TEST_CASE(DuplicateDetection, ConservativeContentMatching) {
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

TEST_CASE(DuplicateDetection, AmbiguitySafetyAndRejection) {
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

TEST_CASE(DuplicateDetection, LibraryQueryProjectionDuplicatesMode) {
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

TEST_CASE(DuplicateDetection, SafeRemovalPreservesIntegrity) {
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

} // namespace Aperture::Test
