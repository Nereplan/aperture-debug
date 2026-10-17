#pragma once
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/LibrarySnapshot.h"
#include "../../src/LibrarySaveComparison.h"
#include "../../src/ImportPlaybackIntent.h"

namespace Aperture::Test {

TEST_CASE(BackendBalance, ImportStartsOnlyTheFirstSortedTargetAndHonorsNewIntent) {
    ImportPlaybackIntent intent;
    intent.Reset(4, true);
    TEST_ASSERT(!intent.Offer(0, 0, L"unordered.mp4"));
    TEST_ASSERT(!intent.Offer(0, 3, L""));
    TEST_ASSERT(!intent.Offer(1, 3, L"second.mp4"));
    TEST_ASSERT(intent.Offer(0, 3, L"first.mp4"));
    TEST_ASSERT_EQ(intent.Take(4), std::wstring(L"first.mp4"));
    TEST_ASSERT(intent.Take(4).empty());
    TEST_ASSERT(!intent.Offer(0, 3, L"duplicate.mp4"));
    intent.Reset(5, true);
    TEST_ASSERT(intent.Offer(0, 3, L"old-intent.mp4"));
    TEST_ASSERT(intent.Take(6).empty());
    TEST_ASSERT(!intent.Offer(0, 3, L"late.mp4"));
    intent.Reset(7, true); intent.Cancel();
    TEST_ASSERT(!intent.Offer(0, 1, L"canceled.mp4"));
    intent.Reset(8, false);
    TEST_ASSERT(!intent.Offer(0, 1, L"import-only.mp4"));
}

TEST_CASE(BackendBalance, SnapshotBlocksRemainIndependentAcrossBoundaries) {
    SnapshotRecords<MediaItem> original;
    for (size_t i = 0; i < 513; ++i) {
        auto item = std::make_shared<MediaItem>(); item->id = std::to_string(i);
        original.push_back(item);
    }
    auto updated = original;
    for (size_t index : {size_t(0), size_t(127), size_t(128), size_t(512)}) {
        auto item = std::make_shared<MediaItem>(*original[index]); item->lastPosSec = 42;
        updated.Replace(index, item);
        TEST_ASSERT_EQ(original[index]->lastPosSec, 0.0);
        TEST_ASSERT_EQ(updated[index]->lastPosSec, 42.0);
    }
    TEST_ASSERT(original[129] == updated[129]);
    updated.Truncate(129);
    updated.push_back(std::make_shared<MediaItem>());
    TEST_ASSERT_EQ(original.size(), size_t(513));
    TEST_ASSERT_EQ(original[129]->id, std::string("129"));
    TEST_ASSERT(updated[129]->id.empty());
    updated.Truncate(128); updated.Truncate(0);
    TEST_ASSERT(updated.empty());
    TEST_ASSERT_EQ(original[512]->id, std::string("512"));
}

TEST_CASE(BackendBalance, PlaybackCapturePersistsAndPreservesPriorSnapshot) {
    ScopedTempDir root;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(root.GetPath()).is_ok());
    for (size_t i = 0; i < 300; ++i) {
        MediaItem item; item.id = std::to_string(i); item.title = L"Movie";
        item.filePath = root.SubPath(std::to_wstring(i) + L".mp4");
        item.durationSec = 120; library.mediaItems.push_back(std::move(item));
    }
    library.RebuildPathIndex();
    auto before = library.CaptureSaveSnapshot();
    MediaLibrarySaveState writer;
    TEST_ASSERT(WriteLibrarySaveSnapshot(*before, writer).is_ok());
    const auto path = library.mediaItems[128].filePath;
    library.UpdateProgress(path, 45, 120);
    auto after = library.CapturePlaybackSnapshot(before, path);
    TEST_ASSERT_EQ((*before->mediaItems)[128]->lastPosSec, 0.0);
    TEST_ASSERT_EQ((*after->mediaItems)[128]->lastPosSec, 45.0);
    TEST_ASSERT((*before->mediaItems)[127] == (*after->mediaItems)[127]);
    TEST_ASSERT((*before->mediaItems)[129] == (*after->mediaItems)[129]);
    TEST_ASSERT(library.CapturePlaybackSnapshot(after, path) == after);
    TEST_ASSERT(library.CaptureSaveSnapshot(after) == after);
    TEST_ASSERT(WriteLibrarySaveSnapshot(*after, writer).is_ok());
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(root.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.FindByPath(path)->lastPosSec, 45.0);
    // A structural mismatch must use the full capture, not apply a stale slot.
    library.mediaItems.erase(library.mediaItems.begin()); library.RebuildPathIndex();
    auto removed = library.CapturePlaybackSnapshot(after, path);
    TEST_ASSERT_EQ(removed->mediaItems->size(), size_t(299));
    TEST_ASSERT_EQ((*removed->mediaItems)[127]->lastPosSec, 45.0);
    // General save still catches direct public edits outside the progress path.
    library.mediaItems[0].title = L"Edited";
    auto edited = library.CaptureSaveSnapshot(removed);
    TEST_ASSERT_EQ((*edited->mediaItems)[0]->title, std::wstring(L"Edited"));
}

TEST_CASE(BackendBalance, StartupUsesSavedOrderingWithoutRebuildingPlan) {
    ScopedTempDir root;
    root.CreateSubDir(L"data");
    TEST_ASSERT(root.WriteFile(L"data\\library.json",
        "[{\"id\":\"a\",\"filePath\":\"z/a.mp4\",\"title\":\"Alpha\",\"durationSec\":30},"
        "{\"id\":\"b\",\"filePath\":\"a/b.mp4\",\"title\":\"Beta\",\"durationSec\":10}]"));
    LibrarySnapshotBuildOptions options; options.rootDir = root.GetPath();
    options.sortField = LibrarySortField::Duration; options.sortDirection = SortDirection::Descending;
    auto snapshot = BuildLibraryStartupSnapshot(options);
    TEST_ASSERT(snapshot->publishable);
    TEST_ASSERT_EQ(snapshot->initialQuery.sortField, LibrarySortField::Duration);
    TEST_ASSERT_EQ(snapshot->initialQuery.sortDirection, SortDirection::Descending);
    TEST_ASSERT_EQ(snapshot->library.mediaItems[snapshot->initialProjection.indices.front()].id, std::string("a"));
    LibraryQueryEngine engine;
    TEST_ASSERT(engine.PrimeViewCache(snapshot->library, snapshot->initialQuery, std::move(snapshot->initialProjection)));
    auto query = snapshot->initialQuery; query.pinLastWatched = true;
    TEST_ASSERT_EQ(engine.QueryView(snapshot->library, query).indices.size(), size_t(2));
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 0ull);
    options.groupMode = LibraryGroupMode::Folder;
    options.sortField = LibrarySortField::Manual;
    options.customFolderOrder = std::make_shared<const std::vector<std::wstring>>(
        std::vector<std::wstring>{root.SubPath(L"z"), root.SubPath(L"a")});
    snapshot = BuildLibraryStartupSnapshot(options);
    TEST_ASSERT(snapshot->publishable);
    TEST_ASSERT_EQ(snapshot->initialQuery.sortField, LibrarySortField::Title);
    TEST_ASSERT(snapshot->initialQuery.customFolderOrder == options.customFolderOrder);
    TEST_ASSERT(engine.PrimeViewCache(snapshot->library, snapshot->initialQuery, std::move(snapshot->initialProjection)));
    engine.QueryView(snapshot->library, snapshot->initialQuery);
    TEST_ASSERT_EQ(engine.GetPlanBuildCount(), 0ull);
}

} // namespace Aperture::Test
