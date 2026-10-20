#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/RenderViewModel.h"
#include "../../src/CollectionAlgorithms.h"
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(BulkOperations, BatchSelectionRemovalAndWatchedState) {
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

    MediaLibrary lib;
    const std::wstring path1 = L"C:\\media\\bulk_test_1.mp4";
    const std::wstring path2 = L"C:\\media\\bulk_test_2.mp4";
    const std::wstring path3 = L"C:\\media\\bulk_test_3.mp4";
    lib.AddOrUpdateMedia(path1)->durationSec = 200.0;
    lib.AddOrUpdateMedia(path2)->durationSec = 300.0;
    lib.AddOrUpdateMedia(path3)->durationSec = 400.0;

    std::string id1 = lib.FindByPath(path1)->id;
    std::string id2 = lib.FindByPath(path2)->id;
    std::string id3 = lib.FindByPath(path3)->id;

    Playlist* pl = lib.CreatePlaylist("Favorites");
    TEST_ASSERT(pl != nullptr);
    std::string plId = pl->id;
    size_t skippedAdd = 0;
    TEST_ASSERT_EQ(lib.AddToPlaylist(plId, { path1, L"c:/media/BULK_TEST_1.mp4", path2, L"", path3 }, &skippedAdd), size_t(3));
    TEST_ASSERT_EQ(skippedAdd, size_t(2));

    size_t skippedWatched = 0;
    TEST_ASSERT_EQ(lib.SetWatched({ path1, path2, L"" }, true, &skippedWatched), size_t(2));
    TEST_ASSERT_EQ(skippedWatched, size_t(1));
    TEST_ASSERT_EQ(lib.FindByPath(path1)->status, "done");
    TEST_ASSERT_EQ(lib.FindByPath(path1)->lastPosSec, 200.0);
    TEST_ASSERT_EQ(lib.FindByPath(path2)->status, "done");

    size_t skippedUnwatched = 0;
    TEST_ASSERT_EQ(lib.SetWatched({ path1, path2 }, false, &skippedUnwatched), size_t(2));
    TEST_ASSERT_EQ(skippedUnwatched, size_t(0));
    TEST_ASSERT_EQ(lib.FindByPath(path1)->status, "new");
    TEST_ASSERT_EQ(lib.FindByPath(path1)->lastPosSec, 0.0);

    lib.AddToHistory(path1, 42.0, 100.0);
    size_t skippedRemove = 0;
    TEST_ASSERT_EQ(lib.RemoveMedia({ id1, id2, "non_existent_id" }, &skippedRemove, false), size_t(2));
    TEST_ASSERT_EQ(skippedRemove, size_t(1));
    TEST_ASSERT(lib.FindById(id1) == nullptr);
    TEST_ASSERT(lib.FindById(id2) == nullptr);
    TEST_ASSERT(lib.FindById(id3) != nullptr);

    Playlist* plCheck = lib.FindPlaylist(plId);
    TEST_ASSERT(plCheck != nullptr);
    TEST_ASSERT_EQ(plCheck->itemPaths.size(), size_t(1));
    TEST_ASSERT_EQ(plCheck->itemPaths[0], path3);
    TEST_ASSERT_EQ(lib.history.size(), size_t(1));
    TEST_ASSERT_EQ(lib.history[0].filePath, path1);
}

} // namespace Aperture::Test
