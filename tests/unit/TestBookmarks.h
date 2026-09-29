#pragma once

#include "../support/TestFramework.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"
#include <cmath>
#include <limits>

namespace Aperture::Test {

TEST_CASE(Bookmarks, BookmarkModelValidationAndBounds) {
    MediaBookmark bm;
    bm.timeSec = 45.5;
    bm.label = L"Opening Scene";
    TEST_ASSERT(bm.IsValid());
    TEST_ASSERT(MediaBookmark::IsValidTime(0.0));
    TEST_ASSERT(MediaBookmark::IsValidTime(3600.0));

    // Reject invalid/non-finite positions
    TEST_ASSERT(!MediaBookmark::IsValidTime(-1.0));
    TEST_ASSERT(!MediaBookmark::IsValidTime(-0.001));
    TEST_ASSERT(!MediaBookmark::IsValidTime(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaBookmark::IsValidTime(std::numeric_limits<double>::infinity()));
    TEST_ASSERT(!MediaBookmark::IsValidTime(-std::numeric_limits<double>::infinity()));

    // Label length bound
    std::wstring longLabel(300, L'A');
    MediaLibrary lib;
    bm.label = longLabel;
    bm.id = "bm_test_1";
    TEST_ASSERT(lib.AddBookmark(L"C:\\media\\test.mp4", bm));

    const auto* bookmarks = lib.GetBookmarks(L"C:\\media\\test.mp4");
    TEST_ASSERT(bookmarks != nullptr);
    TEST_ASSERT_EQ(bookmarks->size(), 1);
    TEST_ASSERT_EQ(bookmarks->front().label.length(), MediaBookmark::kMaxLabelLength);
}

TEST_CASE(Bookmarks, BookmarkDeterministicOrdering) {
    MediaLibrary lib;
    const std::wstring path = L"C:\\media\\movie.mkv";

    MediaBookmark bm3;
    bm3.id = "bm_3";
    bm3.timeSec = 120.0;
    bm3.label = L"Scene 3";
    bm3.createdTimestamp = 300;

    MediaBookmark bm1;
    bm1.id = "bm_1";
    bm1.timeSec = 10.0;
    bm1.label = L"Scene 1";
    bm1.createdTimestamp = 100;

    MediaBookmark bm2;
    bm2.id = "bm_2";
    bm2.timeSec = 45.0;
    bm2.label = L"Scene 2";
    bm2.createdTimestamp = 200;

    MediaBookmark bm_same_time;
    bm_same_time.id = "bm_same";
    bm_same_time.timeSec = 45.0;
    bm_same_time.label = L"Scene 2 Alternate";
    bm_same_time.createdTimestamp = 250;

    // Add out of order
    TEST_ASSERT(lib.AddBookmark(path, bm3));
    TEST_ASSERT(lib.AddBookmark(path, bm1));
    TEST_ASSERT(lib.AddBookmark(path, bm_same_time));
    TEST_ASSERT(lib.AddBookmark(path, bm2));

    const auto* bookmarks = lib.GetBookmarks(path);
    TEST_ASSERT(bookmarks != nullptr);
    TEST_ASSERT_EQ(bookmarks->size(), 4);

    // Verify sorted order: 10.0 -> 45.0 (ts 200) -> 45.0 (ts 250) -> 120.0
    TEST_ASSERT_EQ((*bookmarks)[0].id, "bm_1");
    TEST_ASSERT_EQ((*bookmarks)[0].timeSec, 10.0);
    TEST_ASSERT_EQ((*bookmarks)[1].id, "bm_2");
    TEST_ASSERT_EQ((*bookmarks)[1].timeSec, 45.0);
    TEST_ASSERT_EQ((*bookmarks)[2].id, "bm_same");
    TEST_ASSERT_EQ((*bookmarks)[2].timeSec, 45.0);
    TEST_ASSERT_EQ((*bookmarks)[3].id, "bm_3");
    TEST_ASSERT_EQ((*bookmarks)[3].timeSec, 120.0);
}

TEST_CASE(Bookmarks, BookmarkLibraryOperations) {
    MediaLibrary lib;
    const std::wstring path = L"C:\\media\\video.mp4";

    // Nonexistent media has no bookmarks
    TEST_ASSERT_EQ(lib.GetBookmarks(path), nullptr);

    // Reject adding invalid bookmark
    MediaBookmark invalidBm;
    invalidBm.timeSec = -5.0;
    invalidBm.label = L"Invalid";
    TEST_ASSERT(!lib.AddBookmark(path, invalidBm));

    // Add valid bookmarks
    MediaBookmark bm1;
    bm1.id = "bm_a";
    bm1.timeSec = 30.0;
    bm1.label = L"First";
    TEST_ASSERT(lib.AddBookmark(path, bm1));

    MediaBookmark bm2;
    bm2.id = "bm_b";
    bm2.timeSec = 60.0;
    bm2.label = L"Second";
    TEST_ASSERT(lib.AddBookmark(path, bm2));

    TEST_ASSERT_EQ(lib.GetBookmarks(path)->size(), 2);

    // Update existing bookmark by ID
    MediaBookmark bm1_updated = bm1;
    bm1_updated.label = L"First Renamed";
    TEST_ASSERT(lib.AddBookmark(path, bm1_updated));
    TEST_ASSERT_EQ(lib.GetBookmarks(path)->size(), 2);
    TEST_ASSERT_EQ(lib.GetBookmarks(path)->front().label, L"First Renamed");

    // Remove single bookmark
    TEST_ASSERT(lib.RemoveBookmark(path, "bm_a"));
    TEST_ASSERT_EQ(lib.GetBookmarks(path)->size(), 1);
    TEST_ASSERT_EQ(lib.GetBookmarks(path)->front().id, "bm_b");

    // Remove nonexistent bookmark returns false
    TEST_ASSERT(!lib.RemoveBookmark(path, "nonexistent_id"));

    // Clear bookmarks
    TEST_ASSERT(lib.ClearBookmarks(path));
    TEST_ASSERT_EQ(lib.GetBookmarks(path)->size(), 0);
    TEST_ASSERT(!lib.ClearBookmarks(path)); // Already empty
}

TEST_CASE(Bookmarks, BookmarkRoundTripAndUnknownFields) {
    MediaLibrary lib;
    const std::wstring path = L"C:\\media\\show.mp4";

    MediaItem* item = lib.AddOrUpdateMedia(path);
    TEST_ASSERT(item != nullptr);
    item->durationSec = 500.0;

    // Attach item-level unknown field
    item->extraFields = JsonValue::Object();
    item->extraFields["customItemMetadata"] = "preserve_me";

    // Add bookmark with unknown fields
    MediaBookmark bm;
    bm.id = "bm_roundtrip_1";
    bm.timeSec = 125.5;
    bm.label = L"Key Moment";
    bm.createdTimestamp = 1690000000;
    bm.extraFields = JsonValue::Object();
    bm.extraFields["customColor"] = "#00FF88";
    bm.extraFields["customTag"] = "important_scene";
    TEST_ASSERT(lib.AddBookmark(path, bm));

    // Simulate serialization
    JsonValue outArray = JsonValue::Array();
    for (const auto& m : lib.mediaItems) {
        JsonValue j = m.extraFields.is_object() ? m.extraFields : JsonValue::Object();
        j["id"] = m.id;
        j["filePath"] = Utf16ToUtf8(ToRelocatablePath(m.filePath));
        j["title"] = Utf16ToUtf8(m.title);
        j["durationSec"] = m.durationSec;
        if (!m.bookmarks.empty()) {
            JsonValue bArray = JsonValue::Array();
            for (const auto& b : m.bookmarks) {
                if (!b.IsValid()) continue;
                JsonValue bObj = b.extraFields.is_object() ? b.extraFields : JsonValue::Object();
                bObj["id"] = b.id;
                bObj["timeSec"] = b.timeSec;
                bObj["label"] = Utf16ToUtf8(b.label);
                if (b.createdTimestamp > 0) bObj["createdTimestamp"] = b.createdTimestamp;
                bArray.push_back(std::move(bObj));
            }
            j["bookmarks"] = std::move(bArray);
        }
        outArray.push_back(j);
    }

    std::string serialized = outArray.stringify(true);

    // Simulate deserialization
    JsonValue reloaded;
    TEST_ASSERT(JsonValue::parse(serialized, reloaded));
    TEST_ASSERT(reloaded.is_array());
    TEST_ASSERT_EQ(reloaded.as_array().size(), 1);

    const auto& itemJson = reloaded[0];
    TEST_ASSERT_EQ(itemJson.get("customItemMetadata").as_string(), "preserve_me");
    TEST_ASSERT(itemJson.has_key("bookmarks"));
    TEST_ASSERT_EQ(itemJson.get("bookmarks").as_array().size(), 1);

    const auto& bmJson = itemJson.get("bookmarks")[0];
    TEST_ASSERT_EQ(bmJson.get("id").as_string(), "bm_roundtrip_1");
    TEST_ASSERT_EQ(bmJson.get("timeSec").as_number(), 125.5);
    TEST_ASSERT_EQ(bmJson.get("label").as_string(), "Key Moment");
    TEST_ASSERT_EQ(bmJson.get("createdTimestamp").as_int64(), 1690000000);
    TEST_ASSERT_EQ(bmJson.get("customColor").as_string(), "#00FF88");
    TEST_ASSERT_EQ(bmJson.get("customTag").as_string(), "important_scene");
}

TEST_CASE(Bookmarks, BookmarkRelocationRetention) {
    MediaLibrary lib;
    const std::wstring oldPath = L"C:\\old_root\\video.mp4";
    MediaItem* item = lib.AddOrUpdateMedia(oldPath);
    TEST_ASSERT(item != nullptr);

    MediaBookmark bm;
    bm.id = "bm_reloc";
    bm.timeSec = 77.0;
    bm.label = L"Relocation Test Bookmark";
    TEST_ASSERT(lib.AddBookmark(oldPath, bm));

    // Verify bookmark is on item
    TEST_ASSERT_EQ(item->bookmarks.size(), 1);

    // Simulate path relocation
    const std::wstring newPath = L"D:\\new_root\\video.mp4";
    item->filePath = newPath;
    lib.RebuildPathIndex();

    // Query with new path
    const auto* bookmarks = lib.GetBookmarks(newPath);
    TEST_ASSERT(bookmarks != nullptr);
    TEST_ASSERT_EQ(bookmarks->size(), 1);
    TEST_ASSERT_EQ(bookmarks->front().id, "bm_reloc");
    TEST_ASSERT_EQ(bookmarks->front().timeSec, 77.0);
    TEST_ASSERT_EQ(bookmarks->front().label, L"Relocation Test Bookmark");
}

} // namespace Aperture::Test
