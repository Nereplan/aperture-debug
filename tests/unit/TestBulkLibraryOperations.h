#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/RenderViewModel.h"
#include "../../src/CollectionAlgorithms.h"
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(BulkOperations, SelectedDeletionPreservesOrderAndHandlesStaleIndices) {
    std::vector<std::unique_ptr<int>> items;
    for (int i = 0; i < 10; ++i) items.push_back(std::make_unique<int>(i));
    TEST_ASSERT_EQ(EraseSelectedIndices(items, {0, 2, 3, 8, 99}), size_t(4));
    const int expected[] = {1, 4, 5, 6, 7, 9};
    TEST_ASSERT_EQ(items.size(), size_t(6));
    for (size_t i = 0; i < items.size(); ++i) TEST_ASSERT_EQ(*items[i], expected[i]);
    TEST_ASSERT_EQ(EraseSelectedIndices(items, {}), size_t(0));
    TEST_ASSERT_EQ(EraseSelectedIndices(items, {99}), size_t(0));
    TEST_ASSERT_EQ(EraseSelectedIndices(items, {0, 1, 2, 3, 4, 5}), size_t(6));
    TEST_ASSERT(items.empty());
    TEST_ASSERT_EQ(EraseSelectedIndices(items, {0}), size_t(0));
}

TEST_CASE(BulkOperations, PlaylistBatchKeepsFirstSpellingAndInsertionOrder) {
    MediaLibrary lib;
    Playlist playlist;
    playlist.id = "batch-paths";
    playlist.itemPaths = {L"C:\\Media\\Existing.mp4"};
    lib.playlists.push_back(playlist);
    size_t skipped = 0;
    const size_t added = lib.AddToPlaylist(playlist.id, {
        L"c:/media/EXISTING.mp4", L"C:\\Media\\New.mp4",
        L"c:/media/new.mp4", L"", L"C:\\Media\\Last.mp4"
    }, &skipped);
    TEST_ASSERT_EQ(added, size_t(2));
    TEST_ASSERT_EQ(skipped, size_t(3));
    const auto* result = lib.FindPlaylist(playlist.id);
    TEST_ASSERT_EQ(result->itemPaths.size(), size_t(3));
    TEST_ASSERT_EQ(result->itemPaths[0], playlist.itemPaths[0]);
    TEST_ASSERT_EQ(result->itemPaths[1], std::wstring(L"C:\\Media\\New.mp4"));
    TEST_ASSERT_EQ(result->itemPaths[2], std::wstring(L"C:\\Media\\Last.mp4"));
}

TEST_CASE(BulkOperations, BulkRemoveMediaFromLibraryAndPlaylists) {
    MediaLibrary lib;
    const std::wstring path1 = L"C:\\media\\bulk_test_1.mp4";
    const std::wstring path2 = L"C:\\media\\bulk_test_2.mp4";
    const std::wstring path3 = L"C:\\media\\bulk_test_3.mp4";
    lib.AddOrUpdateMedia(path1);
    lib.AddOrUpdateMedia(path2);
    lib.AddOrUpdateMedia(path3);

    std::string id1 = lib.FindByPath(path1)->id;
    std::string id2 = lib.FindByPath(path2)->id;
    std::string id3 = lib.FindByPath(path3)->id;

    // Create a playlist and add all 3 items
    Playlist* pl = lib.CreatePlaylist("Favorites");
    TEST_ASSERT(pl != nullptr);
    std::string plId = pl->id;
    size_t added = lib.AddToPlaylist(plId, { path1, path2, path3 });
    TEST_ASSERT_EQ(added, 3);

    // Also add item1 to history
    lib.AddToHistory(path1, 42.0, 100.0);
    TEST_ASSERT_EQ(lib.history.size(), 1);

    // Bulk remove item1 and item2 (and include a non-existent id)
    size_t skipped = 0;
    size_t removed = lib.RemoveMedia({ id1, id2, "non_existent_id" }, &skipped);
    TEST_ASSERT_EQ(removed, 2);
    TEST_ASSERT_EQ(skipped, 1);

    // Verify library items
    TEST_ASSERT(lib.FindById(id1) == nullptr);
    TEST_ASSERT(lib.FindById(id2) == nullptr);
    TEST_ASSERT(lib.FindById(id3) != nullptr);

    // Verify playlist references were cleaned up for removed items, while item3 is preserved
    Playlist* plCheck = lib.FindPlaylist(plId);
    TEST_ASSERT(plCheck != nullptr);
    TEST_ASSERT(plCheck->itemPaths[0] == path3);

    // Verify history reference is preserved (external watch log preserved)
    TEST_ASSERT_EQ(lib.history.size(), 1);
    TEST_ASSERT(lib.history[0].filePath == path1);
}

TEST_CASE(BulkOperations, BulkSetWatchedAndUnwatched) {
    MediaLibrary lib;
    const std::wstring path1 = L"C:\\media\\watched_1.mp4";
    const std::wstring path2 = L"C:\\media\\watched_2.mp4";
    const std::wstring path3 = L"C:\\media\\watched_3.mp4";
    lib.AddOrUpdateMedia(path1);
    lib.AddOrUpdateMedia(path2);
    lib.FindByPath(path1)->durationSec = 200.0;
    lib.FindByPath(path2)->durationSec = 300.0;

    // Mark multiple as watched (including a non-existent/empty path to test skipped count)
    size_t skippedWatched = 0;
    size_t watchedCount = lib.SetWatched({ path1, path2, L"" }, true, &skippedWatched);
    TEST_ASSERT_EQ(watchedCount, 2);
    TEST_ASSERT_EQ(skippedWatched, 1);

    TEST_ASSERT_EQ(lib.FindByPath(path1)->status, "done");
    TEST_ASSERT_EQ(lib.FindByPath(path1)->lastPosSec, 200.0);
    TEST_ASSERT_EQ(lib.FindByPath(path2)->status, "done");
    TEST_ASSERT_EQ(lib.FindByPath(path2)->lastPosSec, 300.0);

    // Mark multiple as unwatched
    size_t skippedUnwatched = 0;
    size_t unwatchedCount = lib.SetWatched({ path1, path2 }, false, &skippedUnwatched);
    TEST_ASSERT_EQ(unwatchedCount, 2);
    TEST_ASSERT_EQ(skippedUnwatched, 0);

    TEST_ASSERT_EQ(lib.FindByPath(path1)->status, "new");
    TEST_ASSERT_EQ(lib.FindByPath(path1)->lastPosSec, 0.0);
    TEST_ASSERT_EQ(lib.FindByPath(path2)->status, "new");
    TEST_ASSERT_EQ(lib.FindByPath(path2)->lastPosSec, 0.0);
}

TEST_CASE(BulkOperations, BulkAddToPlaylistWithDuplicateSkipping) {
    MediaLibrary lib;
    const std::wstring path1 = L"C:\\media\\video_a.mp4";
    const std::wstring path2 = L"C:\\media\\video_b.mp4";
    const std::wstring path3 = L"C:\\media\\video_c.mp4";

    Playlist* pl = lib.CreatePlaylist("Action Movies");
    TEST_ASSERT(pl != nullptr);
    std::string plId = pl->id;

    // First batch: add video_a and video_b
    size_t skipped1 = 0;
    size_t added1 = lib.AddToPlaylist(plId, { path1, path2 }, &skipped1);
    TEST_ASSERT_EQ(added1, 2);
    TEST_ASSERT_EQ(skipped1, 0);

    // Second batch: add video_b (duplicate) and video_c (new)
    size_t skipped2 = 0;
    size_t added2 = lib.AddToPlaylist(plId, { path2, path3 }, &skipped2);
    TEST_ASSERT_EQ(added2, 1);
    TEST_ASSERT_EQ(skipped2, 1);

    // Verify playlist contents
    Playlist* plCheck = lib.FindPlaylist(plId);
    TEST_ASSERT(plCheck != nullptr);
    TEST_ASSERT(plCheck->itemPaths[0] == path1);
    TEST_ASSERT(plCheck->itemPaths[1] == path2);
    TEST_ASSERT(plCheck->itemPaths[2] == path3);

    // Verify smart playlist rejects manual bulk addition
    SmartPlaylistCriteria crit;
    Playlist* smartPl = lib.CreateSmartPlaylist("Smart List", crit);
    TEST_ASSERT(smartPl != nullptr);
    size_t smartSkipped = 0;
    size_t smartAdded = lib.AddToPlaylist(smartPl->id, { path1, path2 }, &smartSkipped);
    TEST_ASSERT_EQ(smartAdded, 0);
    TEST_ASSERT_EQ(smartSkipped, 2);
}

TEST_CASE(BulkOperations, ContextMenuItemDisabledStateInvariants) {
    ContextMenuItem itemNormal;
    itemNormal.id = "test_normal";
    itemNormal.label = L"Select All";
    TEST_ASSERT(!itemNormal.isDisabled);

    ContextMenuItem itemDisabled;
    itemDisabled.id = "test_disabled";
    itemDisabled.label = L"Mark Selected as Watched";
    itemDisabled.isDisabled = true;
    TEST_ASSERT(itemDisabled.isDisabled);
}

TEST_CASE(BulkOperations, EmptySelectionAndZeroOpInvariants) {
    MediaLibrary lib;
    size_t skippedRemove = 0;
    size_t removed = lib.RemoveMedia(std::vector<std::string>{}, &skippedRemove);
    TEST_ASSERT_EQ(removed, 0);
    TEST_ASSERT_EQ(skippedRemove, 0);

    size_t skippedWatch = 0;
    size_t watched = lib.SetWatched(std::vector<std::wstring>{}, true, &skippedWatch);
    TEST_ASSERT_EQ(watched, 0);
    TEST_ASSERT_EQ(skippedWatch, 0);

    size_t skippedAdd = 0;
    size_t added = lib.AddToPlaylist("non_existent", std::vector<std::wstring>{}, &skippedAdd);
    TEST_ASSERT_EQ(added, 0);
    TEST_ASSERT_EQ(skippedAdd, 0);
}

} // namespace Aperture::Test
