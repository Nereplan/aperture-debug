#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/FolderWatcher.h"
#include "../../src/Json.h"
#include <thread>
#include <chrono>

namespace Aperture::Test {

// 1. Root-relative watch config round trip
TEST_CASE(FolderWatcher, RootRelativeWatchConfigRoundTrip) {
    ScopedTempDir tempDir;
    std::wstring baseDir = tempDir.GetPath();

    // Folders inside and outside base directory
    std::wstring localFolder = tempDir.SubPath(L"media\\anime");
    std::wstring externalFolder = L"C:\\ExternalMediaCollection\\Movies";

    // Test ToRelocatablePath / ResolveAppPath behavior
    std::wstring relLocal = ToRelocatablePath(localFolder, baseDir);
    TEST_ASSERT_EQ(relLocal, L".\\media\\anime");

    std::wstring relExternal = ToRelocatablePath(externalFolder, baseDir);
    TEST_ASSERT_EQ(relExternal, externalFolder);

    std::wstring resolvedLocal = ResolveAppPath(relLocal, baseDir);
    TEST_ASSERT_EQ(_wcsicmp(resolvedLocal.c_str(), localFolder.c_str()), 0);

    std::wstring resolvedExternal = ResolveAppPath(relExternal, baseDir);
    TEST_ASSERT_EQ(_wcsicmp(resolvedExternal.c_str(), externalFolder.c_str()), 0);

    // Test MediaLibrary watched folders serialization
    MediaLibrary lib;
    TEST_ASSERT(lib.AddWatchedFolder(localFolder, false));
    TEST_ASSERT(lib.AddWatchedFolder(externalFolder, false));
    TEST_ASSERT_EQ(lib.GetWatchedFolders().size(), 2);

    // Duplicate add must be rejected
    TEST_ASSERT(!lib.AddWatchedFolder(localFolder, false));
    TEST_ASSERT_EQ(lib.GetWatchedFolders().size(), 2);

    // Save watched folders to JSON structure
    JsonValue root = JsonValue::Object();
    root["schemaVersion"] = MediaLibrary::kCurrentWatchedFoldersSchemaVersion;
    JsonValue foldersArr = JsonValue::Array();
    for (const auto& f : lib.GetWatchedFolders()) {
        foldersArr.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(f, baseDir))));
    }
    root["folders"] = foldersArr;

    std::string jsonText = root.stringify(false);
    TEST_ASSERT(jsonText.find(".\\\\media\\\\anime") != std::string::npos);
    TEST_ASSERT(jsonText.find("ExternalMediaCollection") != std::string::npos);

    // Now simulate relocating the portable app to a new root directory
    ScopedTempDir relocatedDir;
    std::wstring newBaseDir = relocatedDir.GetPath();

    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(jsonText, parsed));
    TEST_ASSERT(parsed.is_object());
    TEST_ASSERT_EQ(parsed.get("schemaVersion").as_int(), 1);

    std::vector<std::wstring> loadedFolders;
    const auto& arr = parsed.get("folders").as_array();
    for (const auto& v : arr) {
        loadedFolders.push_back(ResolveAppPath(v.as_wstring(), newBaseDir));
    }

    TEST_ASSERT_EQ(loadedFolders.size(), 2);
    std::wstring expectedRelocatedLocal = relocatedDir.SubPath(L"media\\anime");
    TEST_ASSERT_EQ(_wcsicmp(loadedFolders[0].c_str(), expectedRelocatedLocal.c_str()), 0);
    TEST_ASSERT_EQ(_wcsicmp(loadedFolders[1].c_str(), externalFolder.c_str()), 0);

    // Test schema migration from legacy array form
    JsonValue legacyArray = JsonValue::Array();
    legacyArray.push_back(JsonValue(".\\videos"));
    TEST_ASSERT(MediaLibrary::MigrateWatchedFoldersSchema(legacyArray, 0, 1));
    TEST_ASSERT(legacyArray.is_object());
    TEST_ASSERT_EQ(legacyArray.get("schemaVersion").as_int(), 1);
    TEST_ASSERT(legacyArray.get("folders").is_array());
    TEST_ASSERT_EQ(legacyArray.get("folders").as_array().size(), 1);
}

// 2. Add/delete/rename reconciliation
TEST_CASE(FolderWatcher, AddDeleteRenameReconciliation) {
    ScopedTempDir tempDir;
    std::wstring watchedRoot = tempDir.SubPath(L"watch_root");
    tempDir.CreateSubDir(L"watch_root");

    std::wstring file1 = tempDir.SubPath(L"watch_root\\clip1.mp4");
    std::wstring file2 = tempDir.SubPath(L"watch_root\\clip2.mkv");
    std::wstring file3 = tempDir.SubPath(L"watch_root\\ignore.txt");

    tempDir.WriteFile(L"watch_root\\clip1.mp4", "video data 1");
    tempDir.WriteFile(L"watch_root\\clip2.mkv", "video data 2");
    tempDir.WriteFile(L"watch_root\\ignore.txt", "text data");

    MediaLibrary lib;

    // Test Added reconciliation
    WatchChangeBatch addBatch;
    addBatch.rootFolder = watchedRoot;
    addBatch.items.push_back({ WatchAction::Added, file1, L"", watchedRoot });
    addBatch.items.push_back({ WatchAction::Added, file2, L"", watchedRoot });
    addBatch.items.push_back({ WatchAction::Added, file3, L"", watchedRoot }); // Unsupported, should be ignored

    WatchReconcileResult addRes = lib.ReconcileWatchBatch(addBatch, false);
    TEST_ASSERT_EQ(addRes.addedCount, 2);
    TEST_ASSERT_EQ(lib.mediaItems.size(), 2);
    TEST_ASSERT(lib.FindByPath(file1) != nullptr);
    TEST_ASSERT(lib.FindByPath(file2) != nullptr);
    TEST_ASSERT(lib.FindByPath(file3) == nullptr);

    // Set custom preferences & progress on clip1
    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.5;
    prefs.audioDelayMs = 200;
    prefs.subtitlesVisible = false;
    lib.SetMediaPreferences(file1, prefs);
    lib.UpdateProgress(file1, 45.0, 100.0);

    // Add to playlist
    Playlist* pl = lib.CreatePlaylist("My Playlist");
    TEST_ASSERT(pl != nullptr);
    lib.AddToPlaylist(pl->id, file1);
    lib.AddToPlaylist(pl->id, file2);

    // Test Rename (Single File): clip1.mp4 -> clip1_renamed.mp4
    std::wstring file1Renamed = tempDir.SubPath(L"watch_root\\clip1_renamed.mp4");
    tempDir.WriteFile(L"watch_root\\clip1_renamed.mp4", "video data 1");

    WatchChangeBatch renameBatch;
    renameBatch.rootFolder = watchedRoot;
    renameBatch.items.push_back({ WatchAction::Renamed, file1Renamed, file1, watchedRoot });

    WatchReconcileResult renameRes = lib.ReconcileWatchBatch(renameBatch, false);
    TEST_ASSERT_EQ(renameRes.renamedCount, 1);
    TEST_ASSERT(lib.FindByPath(file1) == nullptr);

    MediaItem* renamedItem = lib.FindByPath(file1Renamed);
    TEST_ASSERT(renamedItem != nullptr);
    TEST_ASSERT_EQ(renamedItem->title, L"clip1_renamed.mp4");
    TEST_ASSERT_EQ(renamedItem->lastPosSec, 45.0);

    // Verify preferences preserved after rename
    const MediaPlaybackPreferences* loadedPrefs = lib.GetMediaPreferences(file1Renamed);
    TEST_ASSERT(loadedPrefs != nullptr);
    TEST_ASSERT(loadedPrefs->playbackSpeed.has_value());
    TEST_ASSERT_EQ(*loadedPrefs->playbackSpeed, 1.5);
    TEST_ASSERT_EQ(*loadedPrefs->audioDelayMs, 200);
    TEST_ASSERT_EQ(*loadedPrefs->subtitlesVisible, false);

    // Verify playlist reference updated after rename
    Playlist* updatedPl = lib.FindPlaylist(pl->id);
    TEST_ASSERT(updatedPl != nullptr);
    TEST_ASSERT_EQ(updatedPl->itemPaths.size(), 2);
    TEST_ASSERT_EQ(_wcsicmp(updatedPl->itemPaths[0].c_str(), file1Renamed.c_str()), 0);

    // Test Directory Rename: watch_root\sub -> watch_root\sub_renamed
    tempDir.CreateSubDir(L"watch_root\\sub");
    std::wstring subFile = tempDir.SubPath(L"watch_root\\sub\\episode.mp4");
    tempDir.WriteFile(L"watch_root\\sub\\episode.mp4", "episode data");

    WatchChangeBatch subAdd;
    subAdd.rootFolder = watchedRoot;
    subAdd.items.push_back({ WatchAction::Added, subFile, L"", watchedRoot });
    lib.ReconcileWatchBatch(subAdd, false);
    TEST_ASSERT(lib.FindByPath(subFile) != nullptr);

    std::wstring oldSubDir = tempDir.SubPath(L"watch_root\\sub");
    std::wstring newSubDir = tempDir.SubPath(L"watch_root\\sub_renamed");
    std::wstring newSubFile = tempDir.SubPath(L"watch_root\\sub_renamed\\episode.mp4");
    tempDir.CreateSubDir(L"watch_root\\sub_renamed");
    tempDir.WriteFile(L"watch_root\\sub_renamed\\episode.mp4", "episode data");

    WatchChangeBatch dirRenameBatch;
    dirRenameBatch.rootFolder = watchedRoot;
    dirRenameBatch.items.push_back({ WatchAction::Renamed, newSubDir, oldSubDir, watchedRoot });

    WatchReconcileResult dirRenameRes = lib.ReconcileWatchBatch(dirRenameBatch, false);
    TEST_ASSERT_EQ(dirRenameRes.renamedCount, 1);
    TEST_ASSERT(lib.FindByPath(subFile) == nullptr);
    TEST_ASSERT(lib.FindByPath(newSubFile) != nullptr);

    // Test Removed
    WatchChangeBatch removeBatch;
    removeBatch.rootFolder = watchedRoot;
    removeBatch.items.push_back({ WatchAction::Removed, file2, L"", watchedRoot });

    WatchReconcileResult removeRes = lib.ReconcileWatchBatch(removeBatch, false);
    TEST_ASSERT_EQ(removeRes.removedCount, 1);
    TEST_ASSERT(lib.FindByPath(file2) == nullptr);

    // Verify playlist item was also removed
    updatedPl = lib.FindPlaylist(pl->id);
    TEST_ASSERT_EQ(updatedPl->itemPaths.size(), 1);
    TEST_ASSERT_EQ(_wcsicmp(updatedPl->itemPaths[0].c_str(), file1Renamed.c_str()), 0);
}

// 3. Burst coalescing and overflow rescan
TEST_CASE(FolderWatcher, BurstCoalescingAndOverflowRescan) {
    ScopedTempDir tempDir;
    std::wstring watchedRoot = tempDir.SubPath(L"burst_watch");
    tempDir.CreateSubDir(L"burst_watch");

    std::wstring fileA = tempDir.SubPath(L"burst_watch\\videoA.mp4");
    std::wstring fileB = tempDir.SubPath(L"burst_watch\\videoB.mp4");
    tempDir.WriteFile(L"burst_watch\\videoA.mp4", "data A");
    tempDir.WriteFile(L"burst_watch\\videoB.mp4", "data B");

    FolderWatcher watcher;
    watcher.SetCoalesceWindowMs(50);

    std::vector<WatchChangeBatch> deliveredBatches;
    std::mutex deliveryMutex;
    watcher.SetCallback([&](const WatchChangeBatch& batch) {
        std::lock_guard<std::mutex> lock(deliveryMutex);
        deliveredBatches.push_back(batch);
    });

    // Test Burst Coalescing: Rapid successive events on the same file
    // Added -> Modified -> Modified -> Modified should coalesce into a single Added event
    watcher.EnqueueEvent({ WatchAction::Added, fileA, L"", watchedRoot });
    watcher.EnqueueEvent({ WatchAction::Modified, fileA, L"", watchedRoot });
    watcher.EnqueueEvent({ WatchAction::Modified, fileA, L"", watchedRoot });

    // Added then Removed on fileB within same window should be completely dropped
    watcher.EnqueueEvent({ WatchAction::Added, fileB, L"", watchedRoot });
    watcher.EnqueueEvent({ WatchAction::Removed, fileB, L"", watchedRoot });

    // Flush coalesced batch
    watcher.FlushCoalescedEvents(true);

    TEST_ASSERT_EQ(deliveredBatches.size(), 1);
    TEST_ASSERT_EQ(deliveredBatches[0].items.size(), 1);
    TEST_ASSERT_EQ(deliveredBatches[0].items[0].action, WatchAction::Added);
    TEST_ASSERT_EQ(_wcsicmp(deliveredBatches[0].items[0].path.c_str(), fileA.c_str()), 0);

    MediaLibrary lib;
    WatchReconcileResult res = lib.ReconcileWatchBatch(deliveredBatches[0], false);
    TEST_ASSERT_EQ(res.addedCount, 1);
    TEST_ASSERT(lib.FindByPath(fileA) != nullptr);

    // Test Overflow Rescan
    std::wstring fileC = tempDir.SubPath(L"burst_watch\\videoC.mkv");
    tempDir.WriteFile(L"burst_watch\\videoC.mkv", "data C");

    // Remove fileA and fileB from disk
    DeleteFileW(fileA.c_str());
    DeleteFileW(fileB.c_str());

    WatchChangeBatch overflowBatch;
    overflowBatch.overflowOrRescan = true;
    overflowBatch.rootFolder = watchedRoot;

    WatchReconcileResult rescanRes = lib.ReconcileWatchBatch(overflowBatch, false);
    TEST_ASSERT_EQ(rescanRes.rescanCount, 1);
    TEST_ASSERT_EQ(rescanRes.addedCount, 1); // fileC added
    TEST_ASSERT_EQ(rescanRes.removedCount, 1); // fileA removed

    TEST_ASSERT(lib.FindByPath(fileA) == nullptr);
    TEST_ASSERT(lib.FindByPath(fileC) != nullptr);
}

// 4. Missing root startup
TEST_CASE(FolderWatcher, MissingRootStartup) {
    ScopedTempDir tempDir;
    std::wstring nonExistentRoot = tempDir.SubPath(L"missing_unplugged_drive\\media");

    MediaLibrary lib;
    TEST_ASSERT(lib.AddWatchedFolder(nonExistentRoot, false));
    TEST_ASSERT_EQ(lib.GetWatchedFolders().size(), 1);

    FolderWatcher watcher;
    // Add missing watch folder
    TEST_ASSERT(watcher.AddWatch(nonExistentRoot));
    // Start watcher: missing root must not crash, fail or block startup
    TEST_ASSERT(watcher.Start());
    TEST_ASSERT(watcher.IsRunning());

    // Library reconciliation of missing root should cleanly complete
    WatchReconcileResult res = lib.ReconcileRescan(nonExistentRoot, false);
    TEST_ASSERT_EQ(res.rescanCount, 1);

    watcher.Stop();
    TEST_ASSERT(!watcher.IsRunning());
}

// 5. Clean shutdown
TEST_CASE(FolderWatcher, CleanShutdown) {
    ScopedTempDir tempDir;
    std::wstring root1 = tempDir.SubPath(L"watch1");
    std::wstring root2 = tempDir.SubPath(L"watch2");
    tempDir.CreateSubDir(L"watch1");
    tempDir.CreateSubDir(L"watch2");

    FolderWatcher watcher;
    TEST_ASSERT(watcher.AddWatch(root1));
    TEST_ASSERT(watcher.AddWatch(root2));
    TEST_ASSERT(watcher.Start());
    TEST_ASSERT(watcher.IsRunning());

    // Rapid start/stop idempotence
    watcher.Stop();
    TEST_ASSERT(!watcher.IsRunning());
    watcher.Stop(); // Calling stop again should be safe and no-op
    TEST_ASSERT(!watcher.IsRunning());

    // Restart and stop again
    TEST_ASSERT(watcher.Start());
    TEST_ASSERT(watcher.IsRunning());
    watcher.Stop();
    TEST_ASSERT(!watcher.IsRunning());
}

} // namespace Aperture::Test
