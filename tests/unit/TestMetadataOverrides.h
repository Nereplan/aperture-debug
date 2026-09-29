#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/LibraryQuery.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(MetadataOverrides, DisplayTitleFallback) {
    MediaItem item;
    item.filePath = L"C:\\videos\\sample_movie.mkv";
    item.title = L"";
    // When title and overrides are empty, returns filename
    TEST_ASSERT_EQ(item.GetDisplayTitle(), L"sample_movie.mkv");

    item.title = L"Probed Movie Title";
    TEST_ASSERT_EQ(item.GetDisplayTitle(), L"Probed Movie Title");

    MediaMetadataOverrides ov;
    ov.title = L"User Custom Title";
    item.overrides = ov;
    TEST_ASSERT_EQ(item.GetDisplayTitle(), L"User Custom Title");

    // Empty override title falls back to base title
    ov.title = L"";
    item.overrides = ov;
    TEST_ASSERT_EQ(item.GetDisplayTitle(), L"Probed Movie Title");
}

TEST_CASE(MetadataOverrides, BoundedValidation) {
    // Title validation
    TEST_ASSERT(!MediaMetadataOverrides::IsValidTitle(L""));
    TEST_ASSERT(MediaMetadataOverrides::IsValidTitle(L"Valid Title"));
    std::wstring longTitle(512, L'A');
    TEST_ASSERT(MediaMetadataOverrides::IsValidTitle(longTitle));
    std::wstring tooLongTitle(513, L'A');
    TEST_ASSERT(!MediaMetadataOverrides::IsValidTitle(tooLongTitle));

    // Year validation: [1888, 2100]
    TEST_ASSERT(!MediaMetadataOverrides::IsValidYear(1800));
    TEST_ASSERT(MediaMetadataOverrides::IsValidYear(1888));
    TEST_ASSERT(MediaMetadataOverrides::IsValidYear(2024));
    TEST_ASSERT(MediaMetadataOverrides::IsValidYear(2100));
    TEST_ASSERT(!MediaMetadataOverrides::IsValidYear(2101));

    // Series validation
    TEST_ASSERT(!MediaMetadataOverrides::IsValidSeries(L""));
    TEST_ASSERT(MediaMetadataOverrides::IsValidSeries(L"Breaking Bad"));
    std::wstring longSeries(256, L'S');
    TEST_ASSERT(MediaMetadataOverrides::IsValidSeries(longSeries));
    std::wstring tooLongSeries(257, L'S');
    TEST_ASSERT(!MediaMetadataOverrides::IsValidSeries(tooLongSeries));
}

TEST_CASE(MetadataOverrides, SerializationRoundTripAndUnknownFields) {
    ScopedTempDir tempDir(L"Aperture_MetaOverrides_");
    const std::wstring videoPath = tempDir.SubPath(L"movie.mp4");
    tempDir.WriteFile(L"movie.mp4", "dummy movie data");

    MediaLibrary library;
    MediaItem* item = library.AddOrUpdateMedia(videoPath);
    TEST_ASSERT(item != nullptr);
    TEST_ASSERT(!item->overrides.has_value());

    MediaMetadataOverrides ov;
    ov.title = L"Overridden Title";
    ov.year = 2021;
    ov.series = L"Custom Series";
    JsonValue extra = JsonValue::Object();
    extra["customPluginScore"] = 9.8;
    extra["nestedTag"] = "indie";
    ov.extraFields = extra;

    TEST_ASSERT(library.SetMetadataOverrides(videoPath, ov));

    const MediaMetadataOverrides* stored = library.GetMetadataOverrides(videoPath);
    TEST_ASSERT(stored != nullptr);
    TEST_ASSERT(stored->title.has_value() && *stored->title == L"Overridden Title");
    TEST_ASSERT(stored->year.has_value() && *stored->year == 2021);
    TEST_ASSERT(stored->series.has_value() && *stored->series == L"Custom Series");

    // Save and reload simulation
    MediaLibrarySaveResult saveRes = library.Save();
    TEST_ASSERT(saveRes.is_ok());

    MediaLibrary loadedLib;
    StorageStatus loadStatus = loadedLib.Load();
    TEST_ASSERT(loadStatus.is_ok());

    const MediaItem* loadedItem = loadedLib.FindByPath(videoPath);
    TEST_ASSERT(loadedItem != nullptr);
    TEST_ASSERT(loadedItem->overrides.has_value());
    TEST_ASSERT_EQ(loadedItem->GetDisplayTitle(), L"Overridden Title");
    TEST_ASSERT(loadedItem->overrides->year.has_value() && *loadedItem->overrides->year == 2021);
    TEST_ASSERT(loadedItem->overrides->series.has_value() && *loadedItem->overrides->series == L"Custom Series");
    TEST_ASSERT(loadedItem->overrides->extraFields.is_object());
    TEST_ASSERT_EQ(loadedItem->overrides->extraFields.get("customPluginScore").as_number(), 9.8);
    TEST_ASSERT_EQ(loadedItem->overrides->extraFields.get("nestedTag").as_string(), "indie");
}

TEST_CASE(MetadataOverrides, RefreshMetadataSuccessPreservesOverrides) {
    ScopedTempDir tempDir(L"Aperture_Refresh_Success_");
    const std::wstring videoPath = tempDir.SubPath(L"test_clip.mkv");
    tempDir.WriteFile(L"test_clip.mkv", "dummy video stream data 123");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_item_refresh_1";
    item.filePath = videoPath;
    item.title = L"Base Probed Title";
    item.durationSec = 10.0;
    item.videoWidth = 640;
    item.videoHeight = 480;
    item.metadataProbeFailed = true;
    item.probeAttemptCount = 2;
    item.lastProbeAttemptTimestamp = 1000;

    MediaMetadataOverrides ov;
    ov.title = L"User Display Override";
    ov.year = 2019;
    item.overrides = ov;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    auto mockSuccessProbe = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 185.0;
        outW = 3840;
        outH = 2160;
        return true;
    };

    bool ok = library.RefreshMediaMetadata(videoPath, false, mockSuccessProbe);
    TEST_ASSERT(ok);

    MediaItem* updated = library.FindByPath(videoPath);
    TEST_ASSERT(updated != nullptr);
    TEST_ASSERT_EQ(updated->durationSec, 185.0);
    TEST_ASSERT_EQ(updated->videoWidth, 3840);
    TEST_ASSERT_EQ(updated->videoHeight, 2160);
    TEST_ASSERT_EQ(updated->metadataProbeFailed, false);
    TEST_ASSERT_EQ(updated->probeAttemptCount, 0);
    TEST_ASSERT_EQ(updated->lastProbeAttemptTimestamp, 0);

    // Explicit override and base title are preserved!
    TEST_ASSERT(updated->overrides.has_value());
    TEST_ASSERT_EQ(*updated->overrides->title, L"User Display Override");
    TEST_ASSERT_EQ(*updated->overrides->year, 2019);
    TEST_ASSERT_EQ(updated->GetDisplayTitle(), L"User Display Override");
    TEST_ASSERT_EQ(updated->title, L"Base Probed Title");
}

TEST_CASE(MetadataOverrides, RefreshMetadataFailurePreservesPriorMetadata) {
    ScopedTempDir tempDir(L"Aperture_Refresh_Fail_");
    const std::wstring videoPath = tempDir.SubPath(L"corrupt.mkv");
    tempDir.WriteFile(L"corrupt.mkv", "corrupt data");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_item_refresh_2";
    item.filePath = videoPath;
    item.title = L"Original Title";
    item.durationSec = 95.0;
    item.videoWidth = 1920;
    item.videoHeight = 1080;
    item.metadataProbeFailed = false;
    item.probeAttemptCount = 0;

    MediaMetadataOverrides ov;
    ov.title = L"Custom Title Override";
    item.overrides = ov;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    auto mockFailProbe = [](const std::wstring&, double&, uint32_t&, uint32_t&) {
        return false;
    };

    bool ok = library.RefreshMediaMetadata(videoPath, false, mockFailProbe);
    TEST_ASSERT(!ok);

    MediaItem* failedItem = library.FindByPath(videoPath);
    TEST_ASSERT(failedItem != nullptr);
    // Prior metadata is preserved without corruption!
    TEST_ASSERT_EQ(failedItem->durationSec, 95.0);
    TEST_ASSERT_EQ(failedItem->videoWidth, 1920);
    TEST_ASSERT_EQ(failedItem->videoHeight, 1080);
    TEST_ASSERT_EQ(failedItem->title, L"Original Title");
    TEST_ASSERT_EQ(failedItem->GetDisplayTitle(), L"Custom Title Override");

    // Failure flags updated
    TEST_ASSERT_EQ(failedItem->metadataProbeFailed, true);
    TEST_ASSERT_EQ(failedItem->probeAttemptCount, 1);
    TEST_ASSERT(failedItem->lastProbeAttemptTimestamp > 0);
}

TEST_CASE(MetadataOverrides, ClearAndResetOverrides) {
    MediaLibrary library;
    const std::wstring path = L"C:\\videos\\video1.mp4";
    MediaItem* item = library.AddOrUpdateMedia(path);
    TEST_ASSERT(item != nullptr);
    item->title = L"Default Title";

    MediaMetadataOverrides ov;
    ov.title = L"Overridden Title";
    ov.year = 2022;
    library.SetMetadataOverrides(path, ov);
    TEST_ASSERT_EQ(item->GetDisplayTitle(), L"Overridden Title");

    // Reset / clear overrides
    TEST_ASSERT(library.ClearMetadataOverrides(path));
    TEST_ASSERT(!item->overrides.has_value());
    TEST_ASSERT_EQ(item->GetDisplayTitle(), L"Default Title");
    TEST_ASSERT_EQ(library.GetMetadataOverrides(path), nullptr);
}

TEST_CASE(MetadataOverrides, AutomatedBackfillPreservesOverrides) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Meta_");
    const std::wstring videoPath = tempDir.SubPath(L"clip.mp4");
    tempDir.WriteFile(L"clip.mp4", "valid video stream");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_item_backfill";
    item.filePath = videoPath;
    item.title = L"Original Title";
    item.durationSec = 0.0; // Needs backfill
    item.metadataProbeFailed = true;

    MediaMetadataOverrides ov;
    ov.title = L"Custom User Title";
    ov.year = 2023;
    ov.series = L"Custom Show";
    item.overrides = ov;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 60.0;
        outW = 1280;
        outH = 720;
        return true;
    };

    MetadataBackfillResult res = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT(res.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT_EQ(res.completedCount, 1);

    MediaItem* updated = library.FindByPath(videoPath);
    TEST_ASSERT(updated != nullptr);
    TEST_ASSERT_EQ(updated->durationSec, 60.0);
    TEST_ASSERT_EQ(updated->videoWidth, 1280);
    TEST_ASSERT_EQ(updated->videoHeight, 720);
    // Explicit overrides remain completely intact!
    TEST_ASSERT(updated->overrides.has_value());
    TEST_ASSERT_EQ(updated->GetDisplayTitle(), L"Custom User Title");
    TEST_ASSERT_EQ(*updated->overrides->series, L"Custom Show");
}

TEST_CASE(MetadataOverrides, RelocationPreservesOverrides) {
    ScopedTempDir tempDir(L"Aperture_Relocate_Meta_");
    const std::wstring oldPath = L"D:\\missing_drive\\clip.mp4";
    const std::wstring newPath = tempDir.SubPath(L"clip.mp4");
    tempDir.WriteFile(L"clip.mp4", "relocated data");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_item_relocate";
    item.filePath = oldPath;
    item.title = L"clip.mp4";

    MediaMetadataOverrides ov;
    ov.title = L"Overridden Relocated Title";
    ov.year = 2020;
    item.overrides = ov;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    MediaRelocationResult relRes = library.RelocateMissingMediaToRoot(tempDir.GetPath());
    TEST_ASSERT_EQ(relRes.relocatedCount, 1);

    MediaItem* relocated = library.FindByPath(newPath);
    TEST_ASSERT(relocated != nullptr);
    TEST_ASSERT_EQ(relocated->filePath, newPath);
    TEST_ASSERT(relocated->overrides.has_value());
    TEST_ASSERT_EQ(relocated->GetDisplayTitle(), L"Overridden Relocated Title");
    TEST_ASSERT_EQ(*relocated->overrides->year, 2020);
}

TEST_CASE(MetadataOverrides, QueryAndSearchReflection) {
    MediaLibrary library;
    MediaItem item1;
    item1.id = "m1";
    item1.filePath = L"C:\\videos\\video1.mp4";
    item1.title = L"Alpha Movie";
    MediaMetadataOverrides ov1;
    ov1.title = L"Zeta Overridden";
    item1.overrides = ov1;

    MediaItem item2;
    item2.id = "m2";
    item2.filePath = L"C:\\videos\\video2.mp4";
    item2.title = L"Beta Movie";

    library.mediaItems.push_back(item1);
    library.mediaItems.push_back(item2);
    library.RebuildPathIndex();

    // 1. Search Query matches display title
    TEST_ASSERT(LibraryQueryEngine::MatchesSearchQuery(item1, L"zeta"));
    TEST_ASSERT(LibraryQueryEngine::MatchesSearchQuery(item1, L"alpha")); // Also matches base title

    // 2. Sorting by Title uses display title (item2 "Beta" < item1 "Zeta")
    int cmp = LibraryQueryEngine::CompareItems(item1, item2, LibrarySortField::Title, SortDirection::Ascending);
    TEST_ASSERT(cmp > 0); // item1 ("Zeta") comes AFTER item2 ("Beta")

    // 3. Smart playlist rule matches display title
    SmartPlaylistRule rule;
    rule.field = SmartRuleField::Title;
    rule.op = SmartRuleOperator::Contains;
    rule.value = "Zeta";
    TEST_ASSERT(rule.Matches(item1));
}

} // namespace Aperture::Test
