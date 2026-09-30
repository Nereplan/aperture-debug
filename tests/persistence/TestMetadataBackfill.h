#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(MetadataBackfill, NewPlaybackRecordCanDeferProbeWithoutLosingUserState) {
    ScopedTempDir tempDir(L"Aperture_Deferred_Metadata_");
    tempDir.WriteFile(L"new.mp4", "metadata supplied by worker");
    const auto path = tempDir.SubPath(L"new.mp4");
    MediaLibrary library;
    auto* item = library.AddOrUpdateMedia(path, false);
    TEST_ASSERT(item != nullptr);
    TEST_ASSERT_EQ(item->durationSec, 0.0);
    TEST_ASSERT(!item->metadataProbeFailed);
    TEST_ASSERT_EQ(item->probeAttemptCount, 0);
    item->lastPosSec = 12.5;
    item->preferences.emplace();
    item->preferences->playbackSpeed = 1.25;
    MetadataBackfillOptions options;
    options.probeFunction = [](const auto&, double& duration, uint32_t& width, uint32_t& height) {
        duration = 90; width = 1920; height = 1080; return true;
    };
    const auto result = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(result.completedCount, 1);
    item = library.FindByPath(path);
    TEST_ASSERT_EQ(item->durationSec, 90.0);
    TEST_ASSERT_EQ(item->videoWidth, 1920);
    TEST_ASSERT_EQ(item->videoHeight, 1080);
    TEST_ASSERT_EQ(item->lastPosSec, 12.5);
    TEST_ASSERT_EQ(*item->preferences->playbackSpeed, 1.25);
}

TEST_CASE(MetadataBackfill, SuccessfulRetryAndAtomicUpdate) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Success_");
    const std::wstring videoPath = tempDir.SubPath(L"sample_video.mp4");
    tempDir.WriteFile(L"sample_video.mp4", "dummy video stream data 123456789");

    MediaLibrary library;
    // Simulate an item added where initial probe was transiently failed
    MediaItem item;
    item.id = "media_item_transient";
    item.filePath = videoPath;
    item.title = L"Custom User Defined Title";
    item.durationSec = 0.0;
    item.lastPosSec = 45.0;
    item.status = "in_progress";
    item.fileSizeBytes = 100;
    item.addedTimestamp = 1000;
    item.lastWatchedTimestamp = 1500;
    item.metadataProbeFailed = true;
    item.probeAttemptCount = 1;
    item.lastProbeAttemptTimestamp = 1600;

    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.75;
    prefs.aspectRatio = "16:9";
    prefs.audioTrackId = 2;
    prefs.audioTrackTitle = L"English 5.1";
    prefs.audioDelayMs = 250;
    item.preferences = prefs;

    JsonValue extra = JsonValue::Object();
    extra["customPluginRating"] = 5;
    extra["customTag"] = "transient_probe_test";
    item.extraFields = extra;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    // Custom probe simulator returning duration 125.5s, 1920x1080
    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 125.5;
        outW = 1920;
        outH = 1080;
        return true;
    };

    MetadataBackfillResult result = library.BackfillMissingMetadata(options, {}, false);

    TEST_ASSERT(result.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT(result.status.is_ok());
    TEST_ASSERT_EQ(result.totalEligible, 1);
    TEST_ASSERT_EQ(result.processedCount, 1);
    TEST_ASSERT_EQ(result.completedCount, 1);
    TEST_ASSERT_EQ(result.failedCount, 0);
    TEST_ASSERT_EQ(result.skippedBackoffCount, 0);

    MediaItem* updated = library.FindByPath(videoPath);
    TEST_ASSERT(updated != nullptr);
    TEST_ASSERT_EQ(updated->durationSec, 125.5);
    TEST_ASSERT_EQ(updated->videoWidth, 1920);
    TEST_ASSERT_EQ(updated->videoHeight, 1080);
    TEST_ASSERT_EQ(updated->metadataProbeFailed, false);
    TEST_ASSERT_EQ(updated->probeAttemptCount, 0);
    TEST_ASSERT_EQ(updated->lastProbeAttemptTimestamp, 0);

    // Verify all preserved user state
    TEST_ASSERT_EQ(updated->title, L"Custom User Defined Title");
    TEST_ASSERT_EQ(updated->lastPosSec, 45.0);
    TEST_ASSERT_EQ(updated->status, "in_progress");
    TEST_ASSERT_EQ(updated->addedTimestamp, 1000);
    TEST_ASSERT_EQ(updated->lastWatchedTimestamp, 1500);

    TEST_ASSERT(updated->preferences.has_value());
    TEST_ASSERT_EQ(*updated->preferences->playbackSpeed, 1.75);
    TEST_ASSERT_EQ(*updated->preferences->aspectRatio, "16:9");
    TEST_ASSERT_EQ(*updated->preferences->audioTrackId, 2);
    TEST_ASSERT_EQ(*updated->preferences->audioTrackTitle, L"English 5.1");
    TEST_ASSERT_EQ(*updated->preferences->audioDelayMs, 250);

    TEST_ASSERT_EQ(updated->extraFields.get("customPluginRating").as_int(), 5);
    TEST_ASSERT_EQ(updated->extraFields.get("customTag").as_string(), "transient_probe_test");
}

TEST_CASE(MetadataBackfill, FailedRetryAndDeterministicBackoff) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Backoff_");
    const std::wstring videoPath = tempDir.SubPath(L"corrupt_or_unreadable.mp4");
    tempDir.WriteFile(L"corrupt_or_unreadable.mp4", "corrupt header data");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_item_failing";
    item.filePath = videoPath;
    item.title = L"corrupt_or_unreadable.mp4";
    item.durationSec = 0.0;
    item.metadataProbeFailed = false;
    item.probeAttemptCount = 0;
    item.lastProbeAttemptTimestamp = 0;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    auto failingProbe = [](const std::wstring&, double&, uint32_t&, uint32_t&) {
        return false;
    };

    // First attempt at T = 1000
    MetadataBackfillOptions opt1;
    opt1.nowTimestamp = 1000;
    opt1.baseBackoffSec = 30;
    opt1.maxAttempts = 3;
    opt1.probeFunction = failingProbe;

    MetadataBackfillResult r1 = library.BackfillMissingMetadata(opt1, {}, false);
    TEST_ASSERT_EQ(r1.totalEligible, 1);
    TEST_ASSERT_EQ(r1.processedCount, 1);
    TEST_ASSERT_EQ(r1.completedCount, 0);
    TEST_ASSERT_EQ(r1.failedCount, 1);
    TEST_ASSERT_EQ(r1.skippedBackoffCount, 0);

    MediaItem* cur = library.FindByPath(videoPath);
    TEST_ASSERT_EQ(cur->metadataProbeFailed, true);
    TEST_ASSERT_EQ(cur->probeAttemptCount, 1);
    TEST_ASSERT_EQ(cur->lastProbeAttemptTimestamp, 1000);

    // Second attempt too early at T = 1010 (backoff is 30s, 1010 - 1000 = 10 < 30)
    MetadataBackfillOptions opt2;
    opt2.nowTimestamp = 1010;
    opt2.baseBackoffSec = 30;
    opt2.maxAttempts = 3;
    opt2.probeFunction = failingProbe;

    MetadataBackfillResult r2 = library.BackfillMissingMetadata(opt2, {}, false);
    TEST_ASSERT_EQ(r2.totalEligible, 1);
    TEST_ASSERT_EQ(r2.processedCount, 0);
    TEST_ASSERT_EQ(r2.skippedBackoffCount, 1);
    TEST_ASSERT_EQ(cur->probeAttemptCount, 1);

    // Third attempt after backoff window at T = 1035 (1035 - 1000 = 35 >= 30) -> fails again
    MetadataBackfillOptions opt3;
    opt3.nowTimestamp = 1035;
    opt3.baseBackoffSec = 30;
    opt3.maxAttempts = 3;
    opt3.probeFunction = failingProbe;

    MetadataBackfillResult r3 = library.BackfillMissingMetadata(opt3, {}, false);
    TEST_ASSERT_EQ(r3.processedCount, 1);
    TEST_ASSERT_EQ(r3.failedCount, 1);
    TEST_ASSERT_EQ(cur->probeAttemptCount, 2);
    TEST_ASSERT_EQ(cur->lastProbeAttemptTimestamp, 1035);

    // Fourth attempt at T = 1070 (exponential backoff is now 30 * 2^1 = 60s, 1070 - 1035 = 35 < 60) -> skipped
    MetadataBackfillOptions opt4;
    opt4.nowTimestamp = 1070;
    opt4.baseBackoffSec = 30;
    opt4.maxAttempts = 3;
    opt4.probeFunction = failingProbe;

    MetadataBackfillResult r4 = library.BackfillMissingMetadata(opt4, {}, false);
    TEST_ASSERT_EQ(r4.processedCount, 0);
    TEST_ASSERT_EQ(r4.skippedBackoffCount, 1);

    // Force retry bypasses backoff interval even before deadline
    MetadataBackfillOptions optForce;
    optForce.nowTimestamp = 1070;
    optForce.forceRetry = true;
    optForce.maxAttempts = 3;
    optForce.probeFunction = failingProbe;

    MetadataBackfillResult rForce = library.BackfillMissingMetadata(optForce, {}, false);
    TEST_ASSERT_EQ(rForce.processedCount, 1);
    TEST_ASSERT_EQ(rForce.failedCount, 1);
    TEST_ASSERT_EQ(cur->probeAttemptCount, 3);
    TEST_ASSERT_EQ(cur->lastProbeAttemptTimestamp, 1070);

    // Max attempts threshold reached (3 attempts) -> subsequent regular backfills treat as permanent failure
    MetadataBackfillOptions optPermanent;
    optPermanent.nowTimestamp = 50000;
    optPermanent.maxAttempts = 3;
    optPermanent.probeFunction = failingProbe;

    MetadataBackfillResult rPerm = library.BackfillMissingMetadata(optPermanent, {}, false);
    TEST_ASSERT_EQ(rPerm.processedCount, 0);
    TEST_ASSERT_EQ(rPerm.skippedBackoffCount, 1);
}

TEST_CASE(MetadataBackfill, CancellationDuringBackfill) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Cancel_");

    MediaLibrary library;
    for (int i = 0; i < 5; ++i) {
        std::wstring name = L"clip_" + std::to_wstring(i) + L".mp4";
        tempDir.WriteFile(name, "clip content data");
        MediaItem mi;
        mi.id = "media_" + std::to_string(i);
        mi.filePath = tempDir.SubPath(name);
        mi.title = name;
        mi.durationSec = 0.0;
        mi.metadataProbeFailed = true;
        library.mediaItems.push_back(mi);
    }
    library.RebuildPathIndex();

    size_t progressCallCount = 0;
    auto cancelAfter2 = [&](size_t processed, size_t, const std::wstring&) {
        progressCallCount++;
        // Cancel after 2 items have been processed
        return processed < 2;
    };

    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 60.0;
        outW = 1280;
        outH = 720;
        return true;
    };

    MetadataBackfillResult result = library.BackfillMissingMetadata(options, cancelAfter2, false);

    TEST_ASSERT(result.outcome == LongOperationOutcome::Canceled);
    TEST_ASSERT(result.status.is_ok());
    TEST_ASSERT_EQ(result.totalEligible, 5);
    TEST_ASSERT_EQ(result.processedCount, 2);
    TEST_ASSERT_EQ(result.completedCount, 2);

    // Check that items 0 and 1 got probed, but remaining items did not
    TEST_ASSERT_EQ(library.mediaItems[0].durationSec, 60.0);
    TEST_ASSERT_EQ(library.mediaItems[1].durationSec, 60.0);
    TEST_ASSERT_EQ(library.mediaItems[2].durationSec, 0.0);
    TEST_ASSERT_EQ(library.mediaItems[3].durationSec, 0.0);
    TEST_ASSERT_EQ(library.mediaItems[4].durationSec, 0.0);
}

TEST_CASE(MetadataBackfill, BoundedBatchProcessing) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Bounded_");

    MediaLibrary library;
    for (int i = 0; i < 8; ++i) {
        std::wstring name = L"file_" + std::to_wstring(i) + L".mkv";
        tempDir.WriteFile(name, "mkv content stream");
        MediaItem mi;
        mi.id = "media_bounded_" + std::to_string(i);
        mi.filePath = tempDir.SubPath(name);
        mi.title = name;
        mi.durationSec = 0.0;
        library.mediaItems.push_back(mi);
    }
    library.RebuildPathIndex();

    MetadataBackfillOptions options;
    options.maxItems = 3; // Process at most 3 items per batch
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 180.0;
        outW = 1920;
        outH = 1080;
        return true;
    };

    // First batch: processes exactly 3 items
    MetadataBackfillResult r1 = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT(r1.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT_EQ(r1.totalEligible, 8);
    TEST_ASSERT_EQ(r1.processedCount, 3);
    TEST_ASSERT_EQ(r1.completedCount, 3);

    // Second batch: processes next 3 items (5 remaining eligible)
    MetadataBackfillResult r2 = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(r2.totalEligible, 5);
    TEST_ASSERT_EQ(r2.processedCount, 3);
    TEST_ASSERT_EQ(r2.completedCount, 3);

    // Third batch: processes remaining 2 items
    MetadataBackfillResult r3 = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(r3.totalEligible, 2);
    TEST_ASSERT_EQ(r3.processedCount, 2);
    TEST_ASSERT_EQ(r3.completedCount, 2);

    // Fourth batch: 0 remaining eligible
    MetadataBackfillResult r4 = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(r4.totalEligible, 0);
    TEST_ASSERT_EQ(r4.processedCount, 0);
}

TEST_CASE(MetadataBackfill, PersistenceRoundTrip) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Persist_");
    const std::wstring vid1 = tempDir.SubPath(L"vid1.mp4");
    const std::wstring vid2 = tempDir.SubPath(L"vid2.mp4");
    const std::wstring vid3 = tempDir.SubPath(L"vid3.mp4");
    tempDir.WriteFile(L"vid1.mp4", "content 1");
    tempDir.WriteFile(L"vid2.mp4", "content 2");
    tempDir.WriteFile(L"vid3.mp4", "content 3");

    {
        MediaLibrary library;
        // Item 1: probed successfully
        MediaItem m1;
        m1.id = "m1";
        m1.filePath = vid1;
        m1.title = L"Video 1";
        m1.durationSec = 200.0;
        m1.videoWidth = 1920;
        m1.videoHeight = 1080;
        m1.metadataProbeFailed = false;
        m1.probeAttemptCount = 0;
        m1.lastProbeAttemptTimestamp = 0;

        // Item 2: probe failed with retry state
        MediaItem m2;
        m2.id = "m2";
        m2.filePath = vid2;
        m2.title = L"Video 2";
        m2.durationSec = 0.0;
        m2.metadataProbeFailed = true;
        m2.probeAttemptCount = 2;
        m2.lastProbeAttemptTimestamp = 1700000000;

        // Item 3: unprobed with preferences
        MediaItem m3;
        m3.id = "m3";
        m3.filePath = vid3;
        m3.title = L"Video 3";
        m3.durationSec = 0.0;
        m3.metadataProbeFailed = false;
        m3.probeAttemptCount = 0;
        m3.preferences.emplace();
        m3.preferences->playbackSpeed = 2.0;

        library.mediaItems = { m1, m2, m3 };
        MediaLibrarySaveResult saveRes = library.Save();
        TEST_ASSERT(saveRes.is_ok());
    }

    // Reload in a fresh library instance
    {
        MediaLibrary reloaded;
        StorageStatus loadStatus = reloaded.Load();
        TEST_ASSERT(loadStatus.is_ok());
        TEST_ASSERT_EQ(reloaded.mediaItems.size(), 3);

        MediaItem* loaded1 = reloaded.FindByPath(vid1);
        TEST_ASSERT(loaded1 != nullptr);
        TEST_ASSERT_EQ(loaded1->durationSec, 200.0);
        TEST_ASSERT_EQ(loaded1->videoWidth, 1920);
        TEST_ASSERT_EQ(loaded1->videoHeight, 1080);
        TEST_ASSERT_EQ(loaded1->metadataProbeFailed, false);
        TEST_ASSERT_EQ(loaded1->probeAttemptCount, 0);
        TEST_ASSERT_EQ(loaded1->lastProbeAttemptTimestamp, 0);

        MediaItem* loaded2 = reloaded.FindByPath(vid2);
        TEST_ASSERT(loaded2 != nullptr);
        TEST_ASSERT_EQ(loaded2->durationSec, 0.0);
        TEST_ASSERT_EQ(loaded2->metadataProbeFailed, true);
        TEST_ASSERT_EQ(loaded2->probeAttemptCount, 2);
        TEST_ASSERT_EQ(loaded2->lastProbeAttemptTimestamp, 1700000000);

        MediaItem* loaded3 = reloaded.FindByPath(vid3);
        TEST_ASSERT(loaded3 != nullptr);
        TEST_ASSERT_EQ(loaded3->durationSec, 0.0);
        TEST_ASSERT_EQ(loaded3->metadataProbeFailed, false);
        TEST_ASSERT(loaded3->preferences.has_value());
        TEST_ASSERT_EQ(*loaded3->preferences->playbackSpeed, 2.0);
    }
}

TEST_CASE(MetadataBackfill, UnknownFieldPreservationDuringBackfill) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Unknown_");
    const std::wstring vidPath = tempDir.SubPath(L"plugin_media.mp4");
    tempDir.WriteFile(L"plugin_media.mp4", "plugin media stream");

    MediaLibrary library;
    MediaItem item;
    item.id = "media_plugin_1";
    item.filePath = vidPath;
    item.title = L"Plugin Media";
    item.durationSec = 0.0;
    item.metadataProbeFailed = true;

    // Attach rich unknown vendor fields
    JsonValue extra = JsonValue::Object();
    extra["vendorPluginGuid"] = "{ABCD-1234-EF56-7890}";
    extra["customScore"] = 98.5;
    JsonValue nestedArray = JsonValue::Array();
    nestedArray.push_back("tag1");
    nestedArray.push_back("tag2");
    extra["customTags"] = nestedArray;
    item.extraFields = extra;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    // Run backfill with successful probe
    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 90.0;
        outW = 1280;
        outH = 720;
        return true;
    };

    MetadataBackfillResult res = library.BackfillMissingMetadata(options, {}, true);
    TEST_ASSERT(res.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT_EQ(res.completedCount, 1);

    // Save and reload to verify unknown fields survived both mutation and disk serialization
    MediaLibrary reloaded;
    TEST_ASSERT(reloaded.Load().is_ok());

    MediaItem* loaded = reloaded.FindByPath(vidPath);
    TEST_ASSERT(loaded != nullptr);
    TEST_ASSERT_EQ(loaded->durationSec, 90.0);
    TEST_ASSERT_EQ(loaded->videoWidth, 1280);
    TEST_ASSERT_EQ(loaded->videoHeight, 720);
    TEST_ASSERT_EQ(loaded->metadataProbeFailed, false);

    TEST_ASSERT_EQ(loaded->extraFields.get("vendorPluginGuid").as_string(), "{ABCD-1234-EF56-7890}");
    TEST_ASSERT_EQ(loaded->extraFields.get("customScore").as_number(), 98.5);
    TEST_ASSERT(loaded->extraFields.get("customTags").is_array());
    TEST_ASSERT_EQ(loaded->extraFields.get("customTags").as_array().size(), 2);
}

TEST_CASE(MetadataBackfill, NoOverwriteOfValidMetadataOrPreferences) {
    ScopedTempDir tempDir(L"Aperture_Backfill_NoOverwrite_");
    const std::wstring validPath = tempDir.SubPath(L"fully_probed.mp4");
    tempDir.WriteFile(L"fully_probed.mp4", "valid video stream");

    MediaLibrary library;
    MediaItem item;
    item.id = "valid_item_1";
    item.filePath = validPath;
    item.title = L"User Customized Title";
    item.durationSec = 500.0;
    item.videoWidth = 3840;
    item.videoHeight = 2160;
    item.metadataProbeFailed = false;
    item.probeAttemptCount = 0;
    item.lastPosSec = 150.0;

    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.25;
    prefs.aspectRatio = "21:9";
    prefs.audioDelayMs = -100;
    item.preferences = prefs;

    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    // Probe simulator that would return different values if invoked
    bool probeInvoked = false;
    MetadataBackfillOptions options;
    options.probeFunction = [&](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        probeInvoked = true;
        outDuration = 10.0;
        outW = 640;
        outH = 480;
        return true;
    };

    MetadataBackfillResult result = library.BackfillMissingMetadata(options, {}, false);

    // Eligible should be 0 because item already has valid duration and probeFailed == false
    TEST_ASSERT_EQ(result.totalEligible, 0);
    TEST_ASSERT_EQ(result.processedCount, 0);
    TEST_ASSERT_EQ(probeInvoked, false);

    MediaItem* current = library.FindByPath(validPath);
    TEST_ASSERT(current != nullptr);
    TEST_ASSERT_EQ(current->durationSec, 500.0);
    TEST_ASSERT_EQ(current->videoWidth, 3840);
    TEST_ASSERT_EQ(current->videoHeight, 2160);
    TEST_ASSERT_EQ(current->title, L"User Customized Title");
    TEST_ASSERT_EQ(current->lastPosSec, 150.0);
    TEST_ASSERT(current->preferences.has_value());
    TEST_ASSERT_EQ(*current->preferences->playbackSpeed, 1.25);
    TEST_ASSERT_EQ(*current->preferences->aspectRatio, "21:9");
    TEST_ASSERT_EQ(*current->preferences->audioDelayMs, -100);
}

TEST_CASE(MetadataBackfill, SchedulingEligibilityCriteria) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Eligibility_");
    MediaLibrary library;

    // 1. Valid probed item: duration > 0, probeFailed == false -> NOT eligible
    tempDir.WriteFile(L"valid.mp4", "valid");
    MediaItem itemValid;
    itemValid.id = "item_valid";
    itemValid.filePath = tempDir.SubPath(L"valid.mp4");
    itemValid.durationSec = 100.0;
    itemValid.metadataProbeFailed = false;
    library.mediaItems.push_back(itemValid);

    // 2. Unprobed item: duration == 0, probeFailed == false -> eligible
    tempDir.WriteFile(L"unprobed.mp4", "unprobed");
    MediaItem itemUnprobed;
    itemUnprobed.id = "item_unprobed";
    itemUnprobed.filePath = tempDir.SubPath(L"unprobed.mp4");
    itemUnprobed.durationSec = 0.0;
    itemUnprobed.metadataProbeFailed = false;
    library.mediaItems.push_back(itemUnprobed);

    // 3. Failed item within backoff window: T_attempt = 1040, now = 1050, backoff = 30s (1050 - 1040 = 10 < 30) -> NOT eligible
    tempDir.WriteFile(L"backoff.mp4", "backoff");
    MediaItem itemBackoff;
    itemBackoff.id = "item_backoff";
    itemBackoff.filePath = tempDir.SubPath(L"backoff.mp4");
    itemBackoff.durationSec = 0.0;
    itemBackoff.metadataProbeFailed = true;
    itemBackoff.probeAttemptCount = 1;
    itemBackoff.lastProbeAttemptTimestamp = 1040;
    library.mediaItems.push_back(itemBackoff);

    // 4. Failed item after backoff window: T_attempt = 1000, now = 1050, backoff = 30s (1050 - 1000 = 50 >= 30) -> eligible
    tempDir.WriteFile(L"ready.mp4", "ready");
    MediaItem itemReady;
    itemReady.id = "item_ready";
    itemReady.filePath = tempDir.SubPath(L"ready.mp4");
    itemReady.durationSec = 0.0;
    itemReady.metadataProbeFailed = true;
    itemReady.probeAttemptCount = 1;
    itemReady.lastProbeAttemptTimestamp = 1000;
    library.mediaItems.push_back(itemReady);

    // 5. Max attempts exceeded: attempts = 5 >= maxAttempts(5) -> NOT eligible without forceRetry
    tempDir.WriteFile(L"maxed.mp4", "maxed");
    MediaItem itemMaxed;
    itemMaxed.id = "item_maxed";
    itemMaxed.filePath = tempDir.SubPath(L"maxed.mp4");
    itemMaxed.durationSec = 0.0;
    itemMaxed.metadataProbeFailed = true;
    itemMaxed.probeAttemptCount = 5;
    itemMaxed.lastProbeAttemptTimestamp = 500;
    library.mediaItems.push_back(itemMaxed);
    library.RebuildPathIndex();

    MetadataBackfillOptions options;
    options.nowTimestamp = 1050;
    options.maxAttempts = 5;
    options.baseBackoffSec = 30;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 50.0;
        outW = 1280;
        outH = 720;
        return true;
    };

    MetadataBackfillResult res = library.BackfillMissingMetadata(options, {}, false);
    // Total items needing backfill = 4 (unprobed, backoff, ready, maxed)
    TEST_ASSERT_EQ(res.totalEligible, 4);
    // Skipped due to backoff/maxAttempts = 2 (backoff, maxed)
    TEST_ASSERT_EQ(res.skippedBackoffCount, 2);
    // Processed = 2 (unprobed, ready)
    TEST_ASSERT_EQ(res.processedCount, 2);
    TEST_ASSERT_EQ(res.completedCount, 2);
    TEST_ASSERT_EQ(res.failedCount, 0);

    // Now test with forceRetry = true -> all 4 unprobed/failed items are attempted regardless of backoff or max attempts
    MetadataBackfillOptions forceOptions = options;
    forceOptions.forceRetry = true;
    MetadataBackfillResult forceRes = library.BackfillMissingMetadata(forceOptions, {}, false);
    TEST_ASSERT_EQ(forceRes.totalEligible, 2); // only itemBackoff and itemMaxed are still unprobed
    TEST_ASSERT_EQ(forceRes.skippedBackoffCount, 0);
    TEST_ASSERT_EQ(forceRes.processedCount, 2);
    TEST_ASSERT_EQ(forceRes.completedCount, 2);
}

TEST_CASE(MetadataBackfill, NoOverlapWorkerCoordination) {
    ScopedTempDir tempDir(L"Aperture_Backfill_NoOverlap_");
    MediaLibrary library;

    for (int i = 0; i < 4; ++i) {
        std::wstring name = L"no_overlap_" + std::to_wstring(i) + L".mp4";
        tempDir.WriteFile(name, "video data");
        MediaItem item;
        item.id = "no_overlap_" + std::to_string(i);
        item.filePath = tempDir.SubPath(name);
        item.durationSec = 0.0;
        library.mediaItems.push_back(item);
    }
    library.RebuildPathIndex();

    std::atomic<bool> jobActive{ false };
    std::atomic<int> concurrentExecutions{ 0 };
    std::atomic<int> maxConcurrent{ 0 };

    auto simulateSchedule = [&](const MetadataBackfillOptions& opt) -> MetadataBackfillResult {
        if (jobActive.exchange(true)) {
            // Job is already active -> do not start overlapping job
            MetadataBackfillResult r;
            r.outcome = LongOperationOutcome::Completed;
            return r;
        }

        concurrentExecutions.fetch_add(1);
        int current = concurrentExecutions.load();
        int prevMax = maxConcurrent.load();
        while (current > prevMax && !maxConcurrent.compare_exchange_weak(prevMax, current)) {}

        // Run on isolated snapshot
        MediaLibrary staged;
        staged.mediaItems = library.mediaItems;
        staged.RebuildPathIndex();
        MetadataBackfillResult result = staged.BackfillMissingMetadata(opt, {}, false);

        concurrentExecutions.fetch_sub(1);
        jobActive.store(false);
        return result;
    };

    MetadataBackfillOptions options;
    options.maxItems = 2;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 60.0;
        outW = 1920;
        outH = 1080;
        return true;
    };

    // First batch runs
    MetadataBackfillResult res1 = simulateSchedule(options);
    TEST_ASSERT_EQ(res1.processedCount, 2);
    TEST_ASSERT_EQ(maxConcurrent.load(), 1);

    // While job is held active, secondary scheduling returns without overlapping
    jobActive.store(true);
    MetadataBackfillResult resOverlap = simulateSchedule(options);
    TEST_ASSERT_EQ(resOverlap.processedCount, 0);
    jobActive.store(false);

    // Subsequent batch runs after job is released
    MetadataBackfillResult res2 = simulateSchedule(options);
    TEST_ASSERT_EQ(res2.processedCount, 2);
    TEST_ASSERT_EQ(maxConcurrent.load(), 1);
}

TEST_CASE(MetadataBackfill, AsyncCancellationAndCleanTeardown) {
    ScopedTempDir tempDir(L"Aperture_Backfill_Teardown_");
    MediaLibrary library;

    for (int i = 0; i < 10; ++i) {
        std::wstring name = L"async_clip_" + std::to_wstring(i) + L".mp4";
        tempDir.WriteFile(name, "video data content");
        MediaItem item;
        item.id = "async_clip_" + std::to_string(i);
        item.filePath = tempDir.SubPath(name);
        item.durationSec = 0.0;
        library.mediaItems.push_back(item);
    }
    library.RebuildPathIndex();

    std::atomic<bool> cancelRequested{ false };
    std::atomic<bool> done{ false };
    std::atomic<size_t> processed{ 0 };
    MetadataBackfillResult asyncResult;
    std::vector<MediaItem> stagedItems;

    // Launch background worker on isolated snapshot
    std::thread worker([&, stagedSnapshot = library.mediaItems]() mutable {
        MediaLibrary staged;
        staged.mediaItems = std::move(stagedSnapshot);
        staged.RebuildPathIndex();
        auto progress = [&](size_t count, size_t, const std::wstring&) {
            processed.store(count);
            return !cancelRequested.load();
        };
        MetadataBackfillOptions options;
        options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            outDuration = 100.0;
            outW = 1280;
            outH = 720;
            return true;
        };
        asyncResult = staged.BackfillMissingMetadata(options, progress, false);
        stagedItems = std::move(staged.mediaItems);
        done.store(true);
    });

    // Request cancellation from UI thread
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    cancelRequested.store(true);

    if (worker.joinable()) {
        worker.join();
    }

    TEST_ASSERT(done.load());
    TEST_ASSERT(asyncResult.outcome == LongOperationOutcome::Canceled);
    TEST_ASSERT(asyncResult.processedCount < 10);
}

TEST_CASE(MetadataBackfill, UIThreadResultApplicationAndSnapshotIsolation) {
    ScopedTempDir tempDir(L"Aperture_Backfill_UIMerge_");
    const std::wstring path1 = tempDir.SubPath(L"item1.mp4");
    const std::wstring path2 = tempDir.SubPath(L"item2.mp4");
    const std::wstring path3 = tempDir.SubPath(L"item3.mp4");
    tempDir.WriteFile(L"item1.mp4", "item1 data 12345");
    tempDir.WriteFile(L"item2.mp4", "item2 data 67890");
    tempDir.WriteFile(L"item3.mp4", "item3 data abcde");

    MediaLibrary library;
    MediaItem m1;
    m1.id = "id_1";
    m1.filePath = path1;
    m1.title = L"Initial Title 1";
    m1.durationSec = 0.0;
    m1.lastPosSec = 10.0;
    m1.metadataProbeFailed = true;

    MediaItem m3;
    m3.id = "id_3";
    m3.filePath = path3;
    m3.title = L"Initial Title 3";
    m3.durationSec = 0.0;
    m3.metadataProbeFailed = true;

    library.mediaItems = { m1, m3 };
    library.RebuildPathIndex();

    // 1. Take isolated snapshot for background worker
    std::vector<MediaItem> snapshot = library.mediaItems;

    // 2. Worker runs backfill on snapshot
    MediaLibrary staged;
    staged.mediaItems = std::move(snapshot);
    staged.RebuildPathIndex();
    MetadataBackfillOptions options;
    options.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 120.0;
        outW = 1920;
        outH = 1080;
        return true;
    };
    MetadataBackfillResult stagedResult = staged.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(stagedResult.completedCount, 2);

    // 3. Simultaneously, UI thread modifies live library:
    // User updates title, progress, preferences on item 1
    MediaItem* liveM1 = library.FindByPath(path1);
    TEST_ASSERT(liveM1 != nullptr);
    liveM1->title = L"User Modified Title 1";
    liveM1->lastPosSec = 45.0;
    liveM1->status = "in_progress";
    liveM1->lastWatchedTimestamp = 5000;
    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.5;
    liveM1->preferences = prefs;

    // User adds new item 2 on UI thread
    MediaItem m2;
    m2.id = "id_2";
    m2.filePath = path2;
    m2.title = L"Newly Added Item 2";
    m2.durationSec = 250.0;
    library.mediaItems.push_back(m2);

    // User deletes item 3 on UI thread
    library.RemoveMedia("id_3");
    library.RebuildPathIndex();

    // 4. UI thread merges staged backfill results into live library using App pattern
    const auto& stagedItems = staged.mediaItems;
    bool anyChanged = false;
    for (const auto& stagedItem : stagedItems) {
        MediaItem* existing = library.FindByPath(stagedItem.filePath);
        if (!existing && !stagedItem.id.empty()) {
            existing = library.FindById(stagedItem.id);
        }
        if (existing) {
            if (existing->durationSec != stagedItem.durationSec ||
                existing->videoWidth != stagedItem.videoWidth ||
                existing->videoHeight != stagedItem.videoHeight ||
                existing->fileSizeBytes != stagedItem.fileSizeBytes ||
                existing->metadataProbeFailed != stagedItem.metadataProbeFailed ||
                existing->probeAttemptCount != stagedItem.probeAttemptCount ||
                existing->lastProbeAttemptTimestamp != stagedItem.lastProbeAttemptTimestamp) {
                
                existing->durationSec = stagedItem.durationSec;
                existing->videoWidth = stagedItem.videoWidth;
                existing->videoHeight = stagedItem.videoHeight;
                existing->fileSizeBytes = stagedItem.fileSizeBytes;
                existing->metadataProbeFailed = stagedItem.metadataProbeFailed;
                existing->probeAttemptCount = stagedItem.probeAttemptCount;
                existing->lastProbeAttemptTimestamp = stagedItem.lastProbeAttemptTimestamp;
                if (existing->durationSec > 0.0 && existing->lastPosSec > existing->durationSec) {
                    existing->lastPosSec = existing->durationSec;
                }
                anyChanged = true;
            }
        }
    }

    TEST_ASSERT(anyChanged);
    library.RebuildPathIndex();
    library.NotifyPlaylistChanged();

    // Verify item 1 received probe metadata while preserving all UI-thread changes
    MediaItem* finalM1 = library.FindByPath(path1);
    TEST_ASSERT(finalM1 != nullptr);
    TEST_ASSERT_EQ(finalM1->durationSec, 120.0);
    TEST_ASSERT_EQ(finalM1->videoWidth, 1920);
    TEST_ASSERT_EQ(finalM1->videoHeight, 1080);
    TEST_ASSERT_EQ(finalM1->metadataProbeFailed, false);
    TEST_ASSERT_EQ(finalM1->probeAttemptCount, 0);
    TEST_ASSERT_EQ(finalM1->title, L"User Modified Title 1");
    TEST_ASSERT_EQ(finalM1->lastPosSec, 45.0);
    TEST_ASSERT_EQ(finalM1->status, "in_progress");
    TEST_ASSERT_EQ(finalM1->lastWatchedTimestamp, 5000);
    TEST_ASSERT(finalM1->preferences.has_value());
    TEST_ASSERT_EQ(*finalM1->preferences->playbackSpeed, 1.5);

    // Verify item 2 was preserved
    MediaItem* finalM2 = library.FindByPath(path2);
    TEST_ASSERT(finalM2 != nullptr);
    TEST_ASSERT_EQ(finalM2->title, L"Newly Added Item 2");
    TEST_ASSERT_EQ(finalM2->durationSec, 250.0);

    // Verify deleted item 3 was NOT resurrected
    MediaItem* finalM3 = library.FindByPath(path3);
    TEST_ASSERT(finalM3 == nullptr);
    TEST_ASSERT_EQ(library.mediaItems.size(), 2);

    // Verify persistence roundtrip
    MediaLibrarySaveResult saveRes = library.Save();
    TEST_ASSERT(saveRes.is_ok());

    MediaLibrary reloaded;
    TEST_ASSERT(reloaded.Load().is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems.size(), 2);
    MediaItem* reloaded1 = reloaded.FindByPath(path1);
    TEST_ASSERT(reloaded1 != nullptr);
    TEST_ASSERT_EQ(reloaded1->durationSec, 120.0);
    TEST_ASSERT_EQ(reloaded1->title, L"User Modified Title 1");
    TEST_ASSERT_EQ(reloaded1->lastPosSec, 45.0);
}

TEST_CASE(MetadataBackfill, FailureAndExponentialBackoffProgression) {
    ScopedTempDir tempDir(L"Aperture_Backfill_ExpBackoff_");
    const std::wstring corruptFile = tempDir.SubPath(L"broken.mkv");
    tempDir.WriteFile(L"broken.mkv", "invalid stream");

    MediaLibrary library;
    MediaItem item;
    item.id = "broken_1";
    item.filePath = corruptFile;
    item.title = L"broken.mkv";
    item.durationSec = 0.0;
    item.metadataProbeFailed = false;
    item.probeAttemptCount = 0;
    item.lastProbeAttemptTimestamp = 0;
    library.mediaItems.push_back(item);
    library.RebuildPathIndex();

    auto failProbe = [](const std::wstring&, double&, uint32_t&, uint32_t&) { return false; };

    MetadataBackfillOptions opt;
    opt.baseBackoffSec = 30;
    opt.maxBackoffSec = 3600;
    opt.maxAttempts = 5;
    opt.probeFunction = failProbe;

    // Attempt 1 at T = 100 -> fails, backoff = 30 * 2^0 = 30s
    opt.nowTimestamp = 100;
    MetadataBackfillResult r1 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(r1.processedCount, 1);
    TEST_ASSERT_EQ(r1.failedCount, 1);
    TEST_ASSERT_EQ(library.mediaItems[0].probeAttemptCount, 1);
    TEST_ASSERT_EQ(library.mediaItems[0].lastProbeAttemptTimestamp, 100);

    // Attempt at T = 120 (120 - 100 = 20 < 30) -> skipped
    opt.nowTimestamp = 120;
    MetadataBackfillResult rSkip1 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(rSkip1.processedCount, 0);
    TEST_ASSERT_EQ(rSkip1.skippedBackoffCount, 1);

    // Attempt 2 at T = 135 (135 - 100 = 35 >= 30) -> fails, backoff = 30 * 2^1 = 60s
    opt.nowTimestamp = 135;
    MetadataBackfillResult r2 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(r2.processedCount, 1);
    TEST_ASSERT_EQ(library.mediaItems[0].probeAttemptCount, 2);
    TEST_ASSERT_EQ(library.mediaItems[0].lastProbeAttemptTimestamp, 135);

    // Attempt at T = 180 (180 - 135 = 45 < 60) -> skipped
    opt.nowTimestamp = 180;
    MetadataBackfillResult rSkip2 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(rSkip2.processedCount, 0);
    TEST_ASSERT_EQ(rSkip2.skippedBackoffCount, 1);

    // Attempt 3 at T = 200 (200 - 135 = 65 >= 60) -> fails, backoff = 30 * 2^2 = 120s
    opt.nowTimestamp = 200;
    MetadataBackfillResult r3 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(r3.processedCount, 1);
    TEST_ASSERT_EQ(library.mediaItems[0].probeAttemptCount, 3);
    TEST_ASSERT_EQ(library.mediaItems[0].lastProbeAttemptTimestamp, 200);

    // Attempt 4 at T = 330 (330 - 200 = 130 >= 120) -> fails, backoff = 30 * 2^3 = 240s
    opt.nowTimestamp = 330;
    MetadataBackfillResult r4 = library.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(r4.processedCount, 1);
    TEST_ASSERT_EQ(library.mediaItems[0].probeAttemptCount, 4);
    TEST_ASSERT_EQ(library.mediaItems[0].lastProbeAttemptTimestamp, 330);

    // Save and reload to verify backoff state persists across restarts
    TEST_ASSERT(library.Save().is_ok());
    MediaLibrary reloaded;
    TEST_ASSERT(reloaded.Load().is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[0].probeAttemptCount, 4);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].lastProbeAttemptTimestamp, 330);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].metadataProbeFailed, true);

    // Attempt 5 at T = 580 (580 - 330 = 250 >= 240) -> fails, hits maxAttempts (5)
    opt.nowTimestamp = 580;
    MetadataBackfillResult r5 = reloaded.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(r5.processedCount, 1);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].probeAttemptCount, 5);

    // Subsequent regular backfills permanently skip item
    opt.nowTimestamp = 999999999;
    MetadataBackfillResult rPermanent = reloaded.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(rPermanent.processedCount, 0);
    TEST_ASSERT_EQ(rPermanent.skippedBackoffCount, 1);

    // Force retry recovers probe when file becomes valid
    opt.forceRetry = true;
    opt.probeFunction = [](const std::wstring&, double& outDuration, uint32_t& outW, uint32_t& outH) {
        outDuration = 300.0;
        outW = 1920;
        outH = 1080;
        return true;
    };
    MetadataBackfillResult rRecover = reloaded.BackfillMissingMetadata(opt, {}, false);
    TEST_ASSERT_EQ(rRecover.processedCount, 1);
    TEST_ASSERT_EQ(rRecover.completedCount, 1);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].durationSec, 300.0);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].metadataProbeFailed, false);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].probeAttemptCount, 0);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].lastProbeAttemptTimestamp, 0);
}

} // namespace Aperture::Test
