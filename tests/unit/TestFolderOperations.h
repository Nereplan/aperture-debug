#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/LibraryQuery.h"
#include "../../src/Settings.h"
#include <windows.h>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>

namespace Aperture::Test {

// 1. Tests for RemoveFolder and RemoveFolders
TEST_CASE(FolderOperations, RemoveSingleAndMultipleFolders) {
    MediaLibrary lib;
    const std::wstring f1_item1 = L"C:\\Anime\\Season 1\\ep01.mkv";
    const std::wstring f1_item2 = L"C:\\Anime\\Season 1\\ep02.mkv";
    const std::wstring f2_item1 = L"C:\\Anime\\Season 2\\ep01.mkv";
    const std::wstring f3_item1 = L"C:\\Movies\\film.mkv";

    lib.AddOrUpdateMedia(f1_item1);
    lib.AddOrUpdateMedia(f1_item2);
    lib.AddOrUpdateMedia(f2_item1);
    lib.AddOrUpdateMedia(f3_item1);

    TEST_ASSERT_EQ(lib.mediaItems.size(), 4);

    Playlist* pl = lib.CreatePlaylist("All Watchlist");
    TEST_ASSERT(pl != nullptr);
    lib.AddToPlaylist(pl->id, { f1_item1, f1_item2, f2_item1, f3_item1 });
    TEST_ASSERT_EQ(pl->itemPaths.size(), 4);

    // Remove single folder (Season 1)
    size_t removed = lib.RemoveFolder(L"C:\\Anime\\Season 1", false);
    TEST_ASSERT_EQ(removed, 2);
    TEST_ASSERT_EQ(lib.mediaItems.size(), 2);
    TEST_ASSERT(lib.FindByPath(f1_item1) == nullptr);
    TEST_ASSERT(lib.FindByPath(f1_item2) == nullptr);
    TEST_ASSERT(lib.FindByPath(f2_item1) != nullptr);
    TEST_ASSERT(lib.FindByPath(f3_item1) != nullptr);

    // Verify playlist itemPaths pruned Season 1 items
    Playlist* plCheck = lib.FindPlaylist(pl->id);
    TEST_ASSERT(plCheck != nullptr);
    TEST_ASSERT_EQ(plCheck->itemPaths.size(), 2);
    TEST_ASSERT(plCheck->itemPaths[0] == f2_item1);
    TEST_ASSERT(plCheck->itemPaths[1] == f3_item1);

    // Bulk remove Season 2 and Movies
    size_t bulkRemoved = lib.RemoveFolders({ L"C:\\Anime\\Season 2", L"C:\\Movies" }, false);
    TEST_ASSERT_EQ(bulkRemoved, 2);
    TEST_ASSERT_EQ(lib.mediaItems.size(), 0);
    TEST_ASSERT_EQ(plCheck->itemPaths.size(), 0);
}

// 2. Tests for custom folder reordering in LibraryQueryEngine
TEST_CASE(FolderOperations, CustomFolderOrderQuery) {
    MediaLibrary lib;
    const std::wstring c_item = L"C:\\Anime\\Folder C\\video.mp4";
    const std::wstring a_item = L"C:\\Anime\\Folder A\\video.mp4";
    const std::wstring b_item = L"C:\\Anime\\Folder B\\video.mp4";

    lib.AddOrUpdateMedia(c_item);
    lib.AddOrUpdateMedia(a_item);
    lib.AddOrUpdateMedia(b_item);

    LibraryQueryEngine engine;

    // Default alphabetical sort: Folder A, Folder B, Folder C
    {
        LibraryQueryOptions opt;
        opt.groupMode = LibraryGroupMode::Folder;
        auto res = engine.Query(lib, opt);
        TEST_ASSERT_EQ(res.groups.size(), 3);
        TEST_ASSERT_EQ(res.groups[0].title, std::wstring(L"Folder A"));
        TEST_ASSERT_EQ(res.groups[1].title, std::wstring(L"Folder B"));
        TEST_ASSERT_EQ(res.groups[2].title, std::wstring(L"Folder C"));
    }

    // Custom order: Folder C first, then Folder A (Folder B should fall to end)
    {
        LibraryQueryOptions opt;
        opt.groupMode = LibraryGroupMode::Folder;
        opt.customFolderOrder = std::make_shared<const std::vector<std::wstring>>(std::initializer_list<std::wstring>{ L"C:\\Anime\\Folder C", L"C:\\Anime\\Folder A" });
        auto res = engine.Query(lib, opt);
        TEST_ASSERT_EQ(res.groups.size(), 3);
        TEST_ASSERT_EQ(res.groups[0].title, std::wstring(L"Folder C"));
        TEST_ASSERT_EQ(res.groups[1].title, std::wstring(L"Folder A"));
        TEST_ASSERT_EQ(res.groups[2].title, std::wstring(L"Folder B"));
    }
}

// 3. Tests for RediscoverFolderChanges on disk rename
TEST_CASE(FolderOperations, DiskFolderRenameRediscovery) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring baseDir = std::wstring(tempPath) + L"Aperture_TestFolderRediscovery_" + std::to_wstring(GetTickCount64());
    std::wstring oldDir = baseDir + L"\\Season 1";
    std::wstring newDir = baseDir + L"\\Re-Zero Season 1";

    std::filesystem::create_directories(oldDir);

    // Create 2 test dummy video files
    std::wstring file1 = oldDir + L"\\Episode_01.mp4";
    std::wstring file2 = oldDir + L"\\Episode_02.mp4";
    {
        std::ofstream f1(file1, std::ios::binary);
        f1 << "dummy video content 12345";
        std::ofstream f2(file2, std::ios::binary);
        f2 << "dummy video content 67890 abcdef";
    }

    MediaLibrary lib;
    lib.AddOrUpdateMedia(file1);
    lib.AddOrUpdateMedia(file2);
    TEST_ASSERT_EQ(lib.mediaItems.size(), 2);

    // Set some progress and bookmark on item 1
    MediaItem* item1 = lib.FindByPath(file1);
    TEST_ASSERT(item1 != nullptr);
    item1->lastPosSec = 120.0;
    item1->durationSec = 1400.0;
    item1->status = "in_progress";
    MediaBookmark bm;
    bm.id = "bm1";
    bm.timeSec = 50.0;
    bm.label = L"Exciting opening";
    item1->bookmarks.push_back(bm);

    // Rename folder on disk from 'Season 1' to 'Re-Zero Season 1'
    std::error_code ec;
    std::filesystem::rename(oldDir, newDir, ec);
    TEST_ASSERT(!ec);

    // Check that original files are now missing on disk
    TEST_ASSERT(!std::filesystem::exists(file1));
    TEST_ASSERT(!std::filesystem::exists(file2));

    // Run folder rediscovery
    auto res = lib.RediscoverFolderChanges(false);
    TEST_ASSERT_EQ(res.renamedFoldersCount, 1);
    TEST_ASSERT(res.relocatedItemsCount >= 2 || res.mergedItemsCount >= 2);

    // Verify paths updated in library
    std::wstring newFile1 = newDir + L"\\Episode_01.mp4";
    std::wstring newFile2 = newDir + L"\\Episode_02.mp4";
    MediaItem* updatedItem1 = lib.FindByPath(newFile1);
    MediaItem* updatedItem2 = lib.FindByPath(newFile2);
    TEST_ASSERT(updatedItem1 != nullptr);
    TEST_ASSERT(updatedItem2 != nullptr);

    // Verify progress and bookmarks were preserved
    TEST_ASSERT_EQ(updatedItem1->lastPosSec, 120.0);
    TEST_ASSERT_EQ(updatedItem1->bookmarks.size(), 1);
    TEST_ASSERT_EQ(updatedItem1->bookmarks[0].label, std::wstring(L"Exciting opening"));

    // Cleanup
    std::filesystem::remove_all(baseDir, ec);
}

// 4. Tests for duplicate ghost merge when newly scanned files already exist
TEST_CASE(FolderOperations, DuplicateGhostMergeOnRename) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring baseDir = std::wstring(tempPath) + L"Aperture_GhostMerge_" + std::to_wstring(GetTickCount64());
    std::wstring oldDir = baseDir + L"\\Original";
    std::wstring newDir = baseDir + L"\\Renamed";

    std::filesystem::create_directories(newDir);

    std::wstring oldFile = oldDir + L"\\movie.mkv";
    std::wstring newFile = newDir + L"\\movie.mkv";

    // Create file at new location
    {
        std::ofstream nf(newFile, std::ios::binary);
        nf << "movie file content 123456789";
    }

    MediaLibrary lib;
    // Simulate old missing item with user watch data
    lib.AddOrUpdateMedia(oldFile);
    MediaItem* ghost = lib.FindByPath(oldFile);
    TEST_ASSERT(ghost != nullptr);
    ghost->lastPosSec = 450.0;
    ghost->durationSec = 7200.0;
    ghost->status = "in_progress";

    // Simulate new item found by scanner (with default/fresh metadata)
    lib.AddOrUpdateMedia(newFile);
    TEST_ASSERT_EQ(lib.mediaItems.size(), 2);

    // Run rediscovery
    auto res = lib.RediscoverFolderChanges(false);
    TEST_ASSERT_EQ(res.renamedFoldersCount, 1);
    TEST_ASSERT_EQ(res.mergedItemsCount, 1);
    TEST_ASSERT_EQ(res.removedGhostCount, 1);

    // The duplicate ghost should have been merged and removed
    TEST_ASSERT_EQ(lib.mediaItems.size(), 1);
    MediaItem* remaining = lib.FindByPath(newFile);
    TEST_ASSERT(remaining != nullptr);
    TEST_ASSERT_EQ(remaining->lastPosSec, 450.0);
    TEST_ASSERT_EQ(remaining->status, std::string("in_progress"));

    // Old file path should not exist in library
    TEST_ASSERT(lib.FindByPath(oldFile) == nullptr);

    // Cleanup
    std::error_code ec;
    std::filesystem::remove_all(baseDir, ec);
}

} // namespace Aperture::Test
