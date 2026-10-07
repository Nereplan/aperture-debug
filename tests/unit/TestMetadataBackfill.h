#pragma once

#include "../support/TestFramework.h"
#include "../../src/MetadataBackfillCursor.h"

namespace Aperture::Test {

TEST_CASE(MetadataBackfill, ChainedBatchesVisitStableCatalogOnce) {
    MediaLibrary library;
    for (int i = 0; i < 101; ++i) {
        MediaItem item; item.id = std::to_string(i);
        item.filePath = LR"(C:\Metadata\clip)" + std::to_wstring(i) + L".mkv";
        if (i % 3 == 0) item.durationSec = 100;
        library.mediaItems.push_back(item);
    }
    library.RebuildPathIndex();
    MetadataBackfillCursor cursor;
    MetadataBackfillOptions options; options.maxItems = 7; options.nowTimestamp = 1000;
    size_t completed = 0;
    for (;;) {
        auto batch = cursor.Take(library, options);
        if (batch.empty()) break;
        TEST_ASSERT(batch.size() <= size_t(7));
        std::vector<MetadataBackfillBaseline> baseline;
        for (auto& item : batch) {
            baseline.push_back(MetadataBackfillBaseline::Capture(item));
            item.durationSec = 100;
        }
        const auto before = library.GetRevisions().catalog;
        completed += library.MergeMetadataBackfill(baseline, batch).changedCount;
        cursor.AcceptOwnChanges(before, library.GetRevisions().catalog);
    }
    TEST_ASSERT_EQ(completed, size_t(67));
    TEST_ASSERT_EQ(cursor.GetExaminedCount(), library.mediaItems.size());
    // A structural edit must restart the cursor and discover a new front item.
    MediaItem added; added.id = "front"; added.filePath = LR"(C:\Metadata\front.mkv)";
    library.mediaItems.insert(library.mediaItems.begin(), added);
    library.RebuildPathIndex();
    const auto batch = cursor.Take(library, options);
    TEST_ASSERT_EQ(batch.size(), size_t(1));
    TEST_ASSERT_EQ(batch.front().id, added.id);
}

TEST_CASE(MetadataBackfill, MergeKeepsLiveUserStateAndSearchIdentity) {
    MediaLibrary library;
    MediaItem item; item.id = "one"; item.filePath = LR"(C:\Metadata\one.mkv)";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    const auto baseline = MetadataBackfillBaseline::Capture(item);
    MediaItem update = item; update.durationSec = 120; update.videoWidth = 1920; update.videoHeight = 1080;
    library.mediaItems[0].lastPosSec = 75;
    library.mediaItems[0].status = "in_progress";
    library.mediaItems[0].bookmarks.push_back({"marker", 30, L"Keep"});
    library.mediaItems[0].overrides.emplace(); library.mediaItems[0].overrides->title = L"Custom";
    const auto content = library.GetSearchContentVersion();
    const auto revisions = library.GetRevisions();
    auto* const originalAddress = library.FindById(item.id);
    const auto result = library.MergeMetadataBackfill({baseline}, {update});
    TEST_ASSERT_EQ(result.changedCount, size_t(1));
    TEST_ASSERT(result.viewChanged);
    TEST_ASSERT(library.FindById(item.id) == originalAddress);
    TEST_ASSERT_EQ(library.GetSearchContentVersion(), content);
    TEST_ASSERT_EQ(library.GetRevisions().catalog, revisions.catalog + 1);
    TEST_ASSERT_EQ(library.GetRevisions().playlists, revisions.playlists);
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 75.0);
    TEST_ASSERT_EQ(library.mediaItems[0].GetDisplayTitle(), std::wstring(L"Custom"));
    TEST_ASSERT_EQ(library.mediaItems[0].bookmarks.size(), size_t(1));
}

TEST_CASE(MetadataBackfill, RetryBookkeepingAvoidsViewInvalidation) {
    MediaLibrary library;
    MediaItem item; item.id = "one"; item.filePath = LR"(C:\Metadata\one.mkv)";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    MediaItem failed = item; failed.metadataProbeFailed = true; failed.probeAttemptCount = 1; failed.lastProbeAttemptTimestamp = 100;
    const auto before = library.GetRevisions(); const auto version = library.GetVersion();
    const auto result = library.MergeMetadataBackfill({MetadataBackfillBaseline::Capture(item)}, {failed});
    TEST_ASSERT_EQ(result.changedCount, size_t(1));
    TEST_ASSERT(!result.viewChanged);
    TEST_ASSERT_EQ(library.GetRevisions().catalog, before.catalog);
    TEST_ASSERT_EQ(library.GetRevisions().playback, before.playback);
    TEST_ASSERT_EQ(library.GetVersion(), version + 1);
    TEST_ASSERT_EQ(library.mediaItems[0].probeAttemptCount, uint32_t(1));
}

TEST_CASE(MetadataBackfill, ChangedFileAndReplacementRejectStaleProbe) {
    MediaLibrary library;
    MediaItem item; item.id = "one"; item.filePath = LR"(C:\Metadata\one.mkv)"; item.fileSizeBytes = 10;
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    auto update = item; update.durationSec = 90;
    const auto baseline = MetadataBackfillBaseline::Capture(item);
    library.mediaItems[0].fileSizeBytes = 20;
    auto result = library.MergeMetadataBackfill({baseline}, {update});
    TEST_ASSERT_EQ(result.discardedCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].durationSec, 0.0);
    library.mediaItems[0] = item; library.mediaItems[0].id = "replacement";
    library.RebuildPathIndex();
    result = library.MergeMetadataBackfill({baseline}, {update});
    TEST_ASSERT_EQ(result.discardedCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].id, std::string("replacement"));
    TEST_ASSERT_EQ(library.mediaItems[0].durationSec, 0.0);
}

} // namespace Aperture::Test
