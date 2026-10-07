#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/WatchReconcile.h"

namespace Aperture::Test {

TEST_CASE(WatchReconcile, MailboxOverflowBecomesCurrentRootRescan) {
    WatchChangeQueue queue;
    const std::wstring root = LR"(C:\Watch)";
    WatchChangeBatch batch{false, root, {}};
    batch.items.resize(1025);
    queue.Push(batch);
    auto pending = queue.Take({root});
    TEST_ASSERT_EQ(pending.size(), size_t(1));
    TEST_ASSERT(pending.front().overflowOrRescan);
    TEST_ASSERT(pending.front().items.empty());
    TEST_ASSERT(!queue.HasPending());
    for (int i = 0; i < 65; ++i) queue.Push({true, root + std::to_wstring(i), {}});
    pending = queue.Take({root, LR"(D:\Current)"});
    TEST_ASSERT_EQ(pending.size(), size_t(2));
    TEST_ASSERT(pending[0].overflowOrRescan && pending[1].overflowOrRescan);
    queue.Push({true, root, {}});
    TEST_ASSERT(queue.Take({LR"(D:\Current)"}).empty());
}

TEST_CASE(WatchReconcile, BulkAddMovesCatalogAndRebuildsIndexOnce) {
    MediaLibrary library;
    library.watchedFolders = {LR"(C:\Watch)"};
    MediaItem old; old.id = "old"; old.filePath = LR"(C:\Watch\old.mkv)";
    library.mediaItems.push_back(old);
    library.RebuildPathIndex();
    const auto before = library.GetRevisions();
    PreparedWatchDelta delta;
    for (int i = 0; i < 200; ++i) {
        PreparedWatchChange change;
        change.action = WatchAction::Added;
        change.path = LR"(C:\Watch\clip)" + std::to_wstring(i) + L".mkv";
        change.size = i + 1;
        delta.changes.push_back(std::move(change));
    }
    const auto result = library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT_EQ(result.addedCount, size_t(200));
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(201));
    TEST_ASSERT_EQ(library.mediaItems.front().filePath, delta.changes.back().path);
    TEST_ASSERT_EQ(library.mediaItems.back().id, old.id);
    TEST_ASSERT_EQ(library.GetRevisions().catalog, before.catalog + 1);
    for (const auto& change : delta.changes) TEST_ASSERT(library.FindByPath(change.path) != nullptr);
}

TEST_CASE(WatchReconcile, MergeRetainsUserEditsAndRejectsStaleFileFacts) {
    MediaLibrary library;
    const std::wstring root = LR"(C:\Watch)";
    library.watchedFolders = {root};
    MediaItem item; item.id = "stable"; item.filePath = root + LR"(\old.mkv)";
    item.fileSizeBytes = 100; item.fileIndex = 7; item.volumeSerialNumber = 8; item.durationSec = 200;
    library.mediaItems.push_back(item);
    Playlist playlist; playlist.id = "playlist"; playlist.itemPaths = {item.filePath};
    library.playlists.push_back(playlist);
    HistoryRecord history; history.filePath = item.filePath; history.lastPosSec = 25;
    library.history.push_back(history);
    library.RebuildPathIndex();
    PreparedWatchChange rename;
    rename.action = WatchAction::Renamed;
    rename.baseline = {item.id, item.filePath, 100, 7, 8};
    rename.path = root + LR"(\new.mkv)"; rename.size = 100;
    rename.identity = {7, 8, true};
    PreparedWatchDelta delta; delta.changes = {rename};
    // These edits happen after the worker captured its baseline.
    library.mediaItems[0].lastPosSec = 90;
    library.mediaItems[0].overrides.emplace();
    library.mediaItems[0].overrides->title = L"My title";
    library.mediaItems[0].bookmarks.push_back({"bookmark", 40, L"Scene"});
    auto result = library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT_EQ(result.renamedCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].id, item.id);
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 90.0);
    TEST_ASSERT_EQ(library.mediaItems[0].GetDisplayTitle(), std::wstring(L"My title"));
    TEST_ASSERT_EQ(library.mediaItems[0].bookmarks.size(), size_t(1));
    TEST_ASSERT_EQ(library.playlists[0].itemPaths[0], rename.path);
    TEST_ASSERT_EQ(library.history[0].filePath, rename.path);
    PreparedWatchChange removal; removal.action = WatchAction::Removed; removal.baseline = rename.baseline;
    delta.changes = {removal};
    result = library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT(result.needsRescan);
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(1));
    removal.baseline.path = rename.path; delta.changes = {removal};
    result = library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT_EQ(result.removedCount, size_t(1));
    TEST_ASSERT(library.mediaItems.empty());
    TEST_ASSERT(library.playlists[0].itemPaths.empty());
    TEST_ASSERT_EQ(library.history.size(), size_t(1));
}

TEST_CASE(WatchReconcile, RescanRecoversRenameByUniqueFileIdentity) {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"old.mkv", "unchanged media"));
    MediaLibrary library; library.watchedFolders = {temp.GetPath()};
    MediaItem item; item.id = "stable"; item.filePath = temp.SubPath(L"old.mkv");
    const auto identity = GetWindowsFileIdentity(item.filePath);
    TEST_ASSERT(identity.valid);
    item.fileIndex = identity.fileIndex; item.volumeSerialNumber = identity.volumeSerialNumber;
    item.fileSizeBytes = 15; item.lastPosSec = 42;
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    std::vector<WatchChangeBatch> batches{{true, temp.GetPath(), {}}};
    auto baseline = WatchReconcileWorker::Capture(library, batches);
    TEST_ASSERT(MoveFileW(item.filePath.c_str(), temp.SubPath(L"new.mkv").c_str()) != FALSE);
    std::atomic<bool> canceled{false};
    const auto delta = WatchReconcileWorker::Prepare(batches, std::move(baseline), canceled);
    const auto result = library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT_EQ(result.renamedCount, size_t(1));
    TEST_ASSERT_EQ(result.addedCount, size_t(0));
    TEST_ASSERT_EQ(result.removedCount, size_t(0));
    TEST_ASSERT_EQ(library.mediaItems[0].id, item.id);
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 42.0);
    TEST_ASSERT_EQ(library.mediaItems[0].filePath, temp.SubPath(L"new.mkv"));
    TEST_ASSERT(temp.WriteFile(L"new.mkv", "changed content"));
    batches = {{false, temp.GetPath(), {{WatchAction::Modified, temp.SubPath(L"new.mkv"), {}, temp.GetPath()}}}};
    const auto modified = WatchReconcileWorker::Prepare(batches, WatchReconcileWorker::Capture(library, batches), canceled);
    TEST_ASSERT_EQ(library.ApplyPreparedWatchDelta(modified, false).updatedCount, size_t(1));
    TEST_ASSERT(library.mediaItems[0].metadataProbeFailed);
}

TEST_CASE(WatchReconcile, UnavailableRootAndCancellationKeepRecords) {
    ScopedTempDir temp;
    const auto root = temp.SubPath(L"absent");
    MediaLibrary library; library.watchedFolders = {root};
    MediaItem item; item.id = "keep"; item.filePath = root + LR"(\clip.mkv)";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    std::vector<WatchChangeBatch> batches{{true, root, {}}};
    std::atomic<bool> canceled{false};
    auto delta = WatchReconcileWorker::Prepare(batches, WatchReconcileWorker::Capture(library, batches), canceled);
    TEST_ASSERT(!delta.errors.empty());
    TEST_ASSERT(delta.changes.empty());
    library.ApplyPreparedWatchDelta(delta, false);
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(1));
    canceled = true;
    delta = WatchReconcileWorker::Prepare(batches, WatchReconcileWorker::Capture(library, batches), canceled);
    TEST_ASSERT(delta.canceled);
    TEST_ASSERT(delta.changes.empty());
}

} // namespace Aperture::Test
