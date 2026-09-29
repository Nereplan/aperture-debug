#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/MediaLibrary.h"

namespace Aperture::Test {

TEST_CASE(BulkRelocation, UniqueMatchesUpdateEveryReference) {
    ScopedTempDir tempDir(L"Aperture_BulkRelocate_");
    const std::wstring searchRoot = tempDir.SubPath(L"moved");
    TEST_ASSERT(tempDir.WriteFile(L"moved\\season1\\episode01.mkv", "one"));
    TEST_ASSERT(tempDir.WriteFile(L"moved\\season1\\episode02.mp4", "two"));

    const std::wstring oldOne = tempDir.SubPath(L"old\\episode01.mkv");
    const std::wstring oldTwo = tempDir.SubPath(L"old\\episode02.mp4");
    const std::wstring newOne = tempDir.SubPath(L"moved\\season1\\episode01.mkv");
    const std::wstring newTwo = tempDir.SubPath(L"moved\\season1\\episode02.mp4");

    MediaLibrary library;
    MediaItem first;
    first.id = "one";
    first.filePath = oldOne;
    first.title = L"Episode 1";
    MediaItem second;
    second.id = "two";
    second.filePath = oldTwo;
    second.title = L"Episode 2";
    library.mediaItems = { first, second };

    Playlist playlist;
    playlist.id = "playlist";
    playlist.name = "Season";
    playlist.itemPaths = { oldOne, oldTwo };
    library.playlists.push_back(playlist);

    HistoryRecord history;
    history.filePath = oldTwo;
    history.title = L"Episode 2";
    library.history.push_back(history);
    library.RebuildPathIndex();

    const MediaRelocationResult result = library.RelocateMissingMediaToRoot(searchRoot);
    TEST_ASSERT_EQ(result.missingCount, static_cast<size_t>(2));
    TEST_ASSERT_EQ(result.relocatedCount, static_cast<size_t>(2));
    TEST_ASSERT_EQ(result.ambiguousCount, static_cast<size_t>(0));
    TEST_ASSERT_EQ(result.notFoundCount, static_cast<size_t>(0));
    TEST_ASSERT_EQ(result.relocatedPaths.size(), static_cast<size_t>(2));
    TEST_ASSERT(library.FindByPath(newOne) != nullptr);
    TEST_ASSERT(library.FindByPath(newTwo) != nullptr);
    TEST_ASSERT_EQ(library.playlists[0].itemPaths[0], newOne);
    TEST_ASSERT_EQ(library.playlists[0].itemPaths[1], newTwo);
    TEST_ASSERT_EQ(library.history[0].filePath, newTwo);
}

TEST_CASE(BulkRelocation, AmbiguousAndMissingMatchesRemainUntouched) {
    ScopedTempDir tempDir(L"Aperture_BulkRelocate_Ambiguous_");
    const std::wstring searchRoot = tempDir.SubPath(L"moved");
    TEST_ASSERT(tempDir.WriteFile(L"moved\\a\\duplicate.mkv", "a"));
    TEST_ASSERT(tempDir.WriteFile(L"moved\\b\\duplicate.mkv", "b"));

    const std::wstring duplicate = tempDir.SubPath(L"old\\duplicate.mkv");
    const std::wstring absent = tempDir.SubPath(L"old\\absent.mp4");

    MediaLibrary library;
    MediaItem first;
    first.id = "duplicate";
    first.filePath = duplicate;
    MediaItem second;
    second.id = "absent";
    second.filePath = absent;
    library.mediaItems = { first, second };
    library.RebuildPathIndex();

    const MediaRelocationResult result = library.RelocateMissingMediaToRoot(searchRoot);
    TEST_ASSERT_EQ(result.missingCount, static_cast<size_t>(2));
    TEST_ASSERT_EQ(result.relocatedCount, static_cast<size_t>(0));
    TEST_ASSERT_EQ(result.ambiguousCount, static_cast<size_t>(1));
    TEST_ASSERT_EQ(result.notFoundCount, static_cast<size_t>(1));
    TEST_ASSERT_EQ(library.mediaItems[0].filePath, duplicate);
    TEST_ASSERT_EQ(library.mediaItems[1].filePath, absent);
}

TEST_CASE(BulkRelocation, InvalidSearchRootChangesNothing) {
    MediaLibrary library;
    MediaItem item;
    item.id = "missing";
    item.filePath = L"Z:\\missing\\video.mkv";
    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    const MediaRelocationResult result = library.RelocateMissingMediaToRoot(L"Z:\\definitely-missing-root");
    TEST_ASSERT_EQ(result.missingCount, static_cast<size_t>(0));
    TEST_ASSERT_EQ(result.relocatedCount, static_cast<size_t>(0));
    TEST_ASSERT_EQ(library.mediaItems[0].filePath, L"Z:\\missing\\video.mkv");
}

} // namespace Aperture::Test
