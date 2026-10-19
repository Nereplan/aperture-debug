#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/MetadataBackfillCursor.h"
#include "../../src/LibraryQuery.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(MetadataOverrides, ValidationAndMutationPreservation) {
    TEST_ASSERT(!MediaMetadataOverrides::IsValidTitle(L""));
    TEST_ASSERT(MediaMetadataOverrides::IsValidTitle(std::wstring(512, L'A')));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidTitle(std::wstring(513, L'A')));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidYear(1800));
    TEST_ASSERT(MediaMetadataOverrides::IsValidYear(1888));
    TEST_ASSERT(MediaMetadataOverrides::IsValidYear(2100));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidYear(2101));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidSeries(L""));
    TEST_ASSERT(MediaMetadataOverrides::IsValidSeries(std::wstring(256, L'S')));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidSeries(std::wstring(257, L'S')));

    ScopedTempDir tempDir(L"Aperture_MetaOverrides_");
    const std::wstring videoPath = tempDir.SubPath(L"movie.mp4");
    tempDir.WriteFile(L"movie.mp4", "dummy movie data");

    MediaLibrary library;
    MediaItem* item = library.AddOrUpdateMedia(videoPath);
    TEST_ASSERT(item != nullptr);
    item->title = L"Base Probed Title";

    MediaMetadataOverrides ov;
    ov.title = L"Overridden Title";
    ov.year = 2021;
    ov.series = L"Custom Series";
    ov.extraFields = JsonValue::Object();
    ov.extraFields["customPluginScore"] = 9.8;
    TEST_ASSERT(library.SetMetadataOverrides(videoPath, ov));

    auto mockSuccessProbe = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 185.0;
        outW = 3840;
        outH = 2160;
        return true;
    };
    TEST_ASSERT(library.RefreshMediaMetadata(videoPath, false, mockSuccessProbe));
    MediaItem* updated = library.FindByPath(videoPath);
    TEST_ASSERT(updated != nullptr);
    TEST_ASSERT_EQ(updated->durationSec, 185.0);
    TEST_ASSERT_EQ(updated->GetDisplayTitle(), L"Overridden Title");
    TEST_ASSERT_EQ(updated->overrides->extraFields.get("customPluginScore").as_number(), 9.8);

    auto mockFailProbe = [](const std::wstring&, double&, uint32_t&, uint32_t&) { return false; };
    TEST_ASSERT(!library.RefreshMediaMetadata(videoPath, false, mockFailProbe));
    TEST_ASSERT_EQ(updated->durationSec, 185.0);
    TEST_ASSERT_EQ(updated->GetDisplayTitle(), L"Overridden Title");
    TEST_ASSERT(updated->metadataProbeFailed);

    updated->durationSec = 0.0;
    MetadataBackfillOptions bfOpts;
    bfOpts.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 60.0;
        outW = 1280;
        outH = 720;
        return true;
    };
    MetadataBackfillResult bfRes = library.BackfillMissingMetadata(bfOpts, {}, false);
    TEST_ASSERT(bfRes.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT_EQ(library.FindByPath(videoPath)->GetDisplayTitle(), L"Overridden Title");

    ScopedTempDir relocDir(L"Aperture_Relocate_Meta_");
    relocDir.WriteFile(L"movie.mp4", "relocated data");
    DeleteFileW(videoPath.c_str());
    TEST_ASSERT_EQ(library.RelocateMissingMediaToRoot(relocDir.GetPath()).relocatedCount, 1);
    MediaItem* relocated = library.FindByPath(relocDir.SubPath(L"movie.mp4"));
    TEST_ASSERT(relocated != nullptr);
    TEST_ASSERT_EQ(relocated->GetDisplayTitle(), L"Overridden Title");

    MediaLibrary cursorLib;
    for (int i = 0; i < 30; ++i) {
        MediaItem ci;
        ci.id = std::to_string(i);
        ci.filePath = LR"(C:\Metadata\clip)" + std::to_wstring(i) + L".mkv";
        if (i % 3 == 0) ci.durationSec = 100;
        cursorLib.mediaItems.push_back(ci);
    }
    cursorLib.RebuildPathIndex();
    MetadataBackfillCursor cursor;
    MetadataBackfillOptions cursorOpts;
    cursorOpts.maxItems = 7;
    cursorOpts.nowTimestamp = 1000;
    size_t completed = 0;
    for (;;) {
        auto batch = cursor.Take(cursorLib, cursorOpts);
        if (batch.empty()) break;
        std::vector<MetadataBackfillBaseline> baseline;
        for (auto& bItem : batch) {
            baseline.push_back(MetadataBackfillBaseline::Capture(bItem));
            bItem.durationSec = 100;
        }
        const auto before = cursorLib.GetRevisions().catalog;
        completed += cursorLib.MergeMetadataBackfill(baseline, batch).changedCount;
        cursor.AcceptOwnChanges(before, cursorLib.GetRevisions().catalog);
    }
    TEST_ASSERT_EQ(completed, size_t(20));
    TEST_ASSERT_EQ(cursor.GetExaminedCount(), cursorLib.mediaItems.size());

    const auto mergeBase = MetadataBackfillBaseline::Capture(cursorLib.mediaItems[0]);
    MediaItem mergeUpdate = cursorLib.mediaItems[0];
    mergeUpdate.durationSec = 240;
    cursorLib.mediaItems[0].lastPosSec = 75;
    cursorLib.mediaItems[0].overrides.emplace();
    cursorLib.mediaItems[0].overrides->title = L"Live Override";
    const auto mergeRes = cursorLib.MergeMetadataBackfill({ mergeBase }, { mergeUpdate });
    TEST_ASSERT_EQ(mergeRes.changedCount, size_t(1));
    TEST_ASSERT_EQ(cursorLib.mediaItems[0].lastPosSec, 75.0);
    TEST_ASSERT_EQ(cursorLib.mediaItems[0].GetDisplayTitle(), std::wstring(L"Live Override"));
}

} // namespace Aperture::Test
