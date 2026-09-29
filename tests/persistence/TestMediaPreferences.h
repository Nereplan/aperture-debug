#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(MediaPreferences, OptionalAndDefaultBehavior) {
    MediaLibrary lib;
    MediaItem* item = lib.AddOrUpdateMedia(L"C:\\media\\sample.mp4");
    TEST_ASSERT(item != nullptr);
    TEST_ASSERT(!item->preferences.has_value());
    TEST_ASSERT_EQ(lib.GetMediaPreferences(L"C:\\media\\sample.mp4"), nullptr);

    // Save simulation: item without preferences emits no "preferences" key
    JsonValue outArray = JsonValue::Array();
    for (const auto& m : lib.mediaItems) {
        JsonValue j = m.extraFields.is_object() ? m.extraFields : JsonValue::Object();
        j["id"] = m.id;
        j["filePath"] = Utf16ToUtf8(ToRelocatablePath(m.filePath));
        j["title"] = Utf16ToUtf8(m.title);
        if (m.preferences.has_value() && m.preferences->HasAnyPreference()) {
            JsonValue prefObj = JsonValue::Object();
            if (m.preferences->playbackSpeed.has_value()) prefObj["playbackSpeed"] = *m.preferences->playbackSpeed;
            j["preferences"] = prefObj;
        }
        outArray.push_back(j);
    }

    std::string savedJson = outArray.stringify(false);
    JsonValue reloaded;
    TEST_ASSERT(JsonValue::parse(savedJson, reloaded));
    TEST_ASSERT(!reloaded[0].has_key("preferences"));
}

TEST_CASE(MediaPreferences, FullRoundTripSerialization) {
    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 1.5;
    prefs.aspectRatio = "16:9";
    prefs.audioTrackId = 2;
    prefs.audioTrackTitle = L"Japanese [5.1 Surround]";
    prefs.audioTrackLang = "jpn";
    prefs.subtitleTrackId = 3;
    prefs.subtitleTrackTitle = L"English [Full Dialogue]";
    prefs.subtitleTrackLang = "eng";
    prefs.audioDelayMs = -150;
    prefs.subDelayMs = 250;
    prefs.subtitlesVisible = true;

    // Simulate serialization
    JsonValue prefObj = JsonValue::Object();
    if (prefs.playbackSpeed.has_value() && MediaPlaybackPreferences::IsValidPlaybackSpeed(*prefs.playbackSpeed)) {
        prefObj["playbackSpeed"] = *prefs.playbackSpeed;
    }
    if (prefs.aspectRatio.has_value() && MediaPlaybackPreferences::IsValidAspectRatio(*prefs.aspectRatio)) {
        prefObj["aspectRatio"] = *prefs.aspectRatio;
    }
    if (prefs.audioTrackId.has_value()) prefObj["audioTrackId"] = *prefs.audioTrackId;
    if (prefs.audioTrackTitle.has_value()) prefObj["audioTrackTitle"] = Utf16ToUtf8(*prefs.audioTrackTitle);
    if (prefs.audioTrackLang.has_value()) prefObj["audioTrackLang"] = *prefs.audioTrackLang;
    if (prefs.subtitleTrackId.has_value()) prefObj["subtitleTrackId"] = *prefs.subtitleTrackId;
    if (prefs.subtitleTrackTitle.has_value()) prefObj["subtitleTrackTitle"] = Utf16ToUtf8(*prefs.subtitleTrackTitle);
    if (prefs.subtitleTrackLang.has_value()) prefObj["subtitleTrackLang"] = *prefs.subtitleTrackLang;
    if (prefs.audioDelayMs.has_value() && MediaPlaybackPreferences::IsValidAudioDelayMs(*prefs.audioDelayMs)) {
        prefObj["audioDelayMs"] = *prefs.audioDelayMs;
    }
    if (prefs.subDelayMs.has_value() && MediaPlaybackPreferences::IsValidSubDelayMs(*prefs.subDelayMs)) {
        prefObj["subDelayMs"] = *prefs.subDelayMs;
    }
    if (prefs.subtitlesVisible.has_value()) prefObj["subtitlesVisible"] = *prefs.subtitlesVisible;

    JsonValue itemJson = JsonValue::Object();
    itemJson["id"] = "med_1";
    itemJson["filePath"] = ".\\media\\movie.mkv";
    itemJson["title"] = "Movie";
    itemJson["preferences"] = prefObj;

    std::string serialized = itemJson.stringify(false);

    // Simulate deserialization
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(serialized, parsed));
    TEST_ASSERT(parsed.has_key("preferences"));
    const auto& pJson = parsed.get("preferences");

    MediaPlaybackPreferences loadedPrefs;
    if (pJson.has_key("playbackSpeed") && pJson.get("playbackSpeed").is_number()) {
        double spd = pJson.get("playbackSpeed").as_number();
        if (MediaPlaybackPreferences::IsValidPlaybackSpeed(spd)) loadedPrefs.playbackSpeed = spd;
    }
    if (pJson.has_key("aspectRatio") && pJson.get("aspectRatio").is_string()) {
        std::string ar = pJson.get("aspectRatio").as_string();
        if (MediaPlaybackPreferences::IsValidAspectRatio(ar)) loadedPrefs.aspectRatio = ar;
    }
    if (pJson.has_key("audioTrackId") && pJson.get("audioTrackId").is_number()) {
        loadedPrefs.audioTrackId = pJson.get("audioTrackId").as_int();
    }
    if (pJson.has_key("audioTrackTitle") && pJson.get("audioTrackTitle").is_string()) {
        loadedPrefs.audioTrackTitle = pJson.get("audioTrackTitle").as_wstring();
    }
    if (pJson.has_key("audioTrackLang") && pJson.get("audioTrackLang").is_string()) {
        loadedPrefs.audioTrackLang = pJson.get("audioTrackLang").as_string();
    }
    if (pJson.has_key("subtitleTrackId") && pJson.get("subtitleTrackId").is_number()) {
        loadedPrefs.subtitleTrackId = pJson.get("subtitleTrackId").as_int();
    }
    if (pJson.has_key("subtitleTrackTitle") && pJson.get("subtitleTrackTitle").is_string()) {
        loadedPrefs.subtitleTrackTitle = pJson.get("subtitleTrackTitle").as_wstring();
    }
    if (pJson.has_key("subtitleTrackLang") && pJson.get("subtitleTrackLang").is_string()) {
        loadedPrefs.subtitleTrackLang = pJson.get("subtitleTrackLang").as_string();
    }
    if (pJson.has_key("audioDelayMs") && pJson.get("audioDelayMs").is_number()) {
        int d = pJson.get("audioDelayMs").as_int();
        if (MediaPlaybackPreferences::IsValidAudioDelayMs(d)) loadedPrefs.audioDelayMs = d;
    }
    if (pJson.has_key("subDelayMs") && pJson.get("subDelayMs").is_number()) {
        int d = pJson.get("subDelayMs").as_int();
        if (MediaPlaybackPreferences::IsValidSubDelayMs(d)) loadedPrefs.subDelayMs = d;
    }
    if (pJson.has_key("subtitlesVisible") && pJson.get("subtitlesVisible").is_bool()) {
        loadedPrefs.subtitlesVisible = pJson.get("subtitlesVisible").as_bool();
    }

    TEST_ASSERT(loadedPrefs.playbackSpeed.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.playbackSpeed, 1.5);
    TEST_ASSERT(loadedPrefs.aspectRatio.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.aspectRatio, std::string("16:9"));
    TEST_ASSERT(loadedPrefs.audioTrackId.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.audioTrackId, 2);
    TEST_ASSERT(loadedPrefs.audioTrackTitle.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.audioTrackTitle, std::wstring(L"Japanese [5.1 Surround]"));
    TEST_ASSERT(loadedPrefs.audioTrackLang.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.audioTrackLang, std::string("jpn"));
    TEST_ASSERT(loadedPrefs.subtitleTrackId.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subtitleTrackId, 3);
    TEST_ASSERT(loadedPrefs.subtitleTrackTitle.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subtitleTrackTitle, std::wstring(L"English [Full Dialogue]"));
    TEST_ASSERT(loadedPrefs.subtitleTrackLang.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subtitleTrackLang, std::string("eng"));
    TEST_ASSERT(loadedPrefs.audioDelayMs.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.audioDelayMs, -150);
    TEST_ASSERT(loadedPrefs.subDelayMs.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subDelayMs, 250);
    TEST_ASSERT(loadedPrefs.subtitlesVisible.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subtitlesVisible, true);
}

TEST_CASE(MediaPreferences, InvalidValueFallbackAndBoundsValidation) {
    // Validation functions
    TEST_ASSERT(MediaPlaybackPreferences::IsValidPlaybackSpeed(0.25));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidPlaybackSpeed(1.0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidPlaybackSpeed(4.0));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidPlaybackSpeed(0.1));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidPlaybackSpeed(5.0));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidPlaybackSpeed(-1.0));

    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("auto"));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("16:9"));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("4:3"));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("21:9"));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("fill"));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAspectRatio("cover"));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidAspectRatio("wide_16_10"));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidAspectRatio(""));

    TEST_ASSERT(MediaPlaybackPreferences::IsValidAudioDelayMs(-5000));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAudioDelayMs(5000));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAudioDelayMs(0));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidAudioDelayMs(-5001));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidAudioDelayMs(5001));

    TEST_ASSERT(MediaPlaybackPreferences::IsValidSubDelayMs(-10000));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidSubDelayMs(10000));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidSubDelayMs(0));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSubDelayMs(-10001));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSubDelayMs(10001));

    // Test JSON deserialization of invalid fields
    std::string invalidJson = "{\n"
        "  \"playbackSpeed\": 99.0,\n"
        "  \"aspectRatio\": \"ultra_wide_anamorphic\",\n"
        "  \"audioDelayMs\": 999999,\n"
        "  \"subDelayMs\": -999999,\n"
        "  \"audioTrackId\": 1,\n"
        "  \"subtitlesVisible\": false\n"
        "}";

    JsonValue prefJson;
    TEST_ASSERT(JsonValue::parse(invalidJson, prefJson));

    MediaPlaybackPreferences loadedPrefs;
    if (prefJson.has_key("playbackSpeed") && prefJson.get("playbackSpeed").is_number()) {
        double spd = prefJson.get("playbackSpeed").as_number();
        if (MediaPlaybackPreferences::IsValidPlaybackSpeed(spd)) loadedPrefs.playbackSpeed = spd;
    }
    if (prefJson.has_key("aspectRatio") && prefJson.get("aspectRatio").is_string()) {
        std::string ar = prefJson.get("aspectRatio").as_string();
        if (MediaPlaybackPreferences::IsValidAspectRatio(ar)) loadedPrefs.aspectRatio = ar;
    }
    if (prefJson.has_key("audioDelayMs") && prefJson.get("audioDelayMs").is_number()) {
        int d = prefJson.get("audioDelayMs").as_int();
        if (MediaPlaybackPreferences::IsValidAudioDelayMs(d)) loadedPrefs.audioDelayMs = d;
    }
    if (prefJson.has_key("subDelayMs") && prefJson.get("subDelayMs").is_number()) {
        int d = prefJson.get("subDelayMs").as_int();
        if (MediaPlaybackPreferences::IsValidSubDelayMs(d)) loadedPrefs.subDelayMs = d;
    }
    if (prefJson.has_key("audioTrackId") && prefJson.get("audioTrackId").is_number()) {
        loadedPrefs.audioTrackId = prefJson.get("audioTrackId").as_int();
    }
    if (prefJson.has_key("subtitlesVisible") && prefJson.get("subtitlesVisible").is_bool()) {
        loadedPrefs.subtitlesVisible = prefJson.get("subtitlesVisible").as_bool();
    }

    // Invalid fields must be rejected (nullopt), valid fields parsed
    TEST_ASSERT(!loadedPrefs.playbackSpeed.has_value());
    TEST_ASSERT(!loadedPrefs.aspectRatio.has_value());
    TEST_ASSERT(!loadedPrefs.audioDelayMs.has_value());
    TEST_ASSERT(!loadedPrefs.subDelayMs.has_value());
    TEST_ASSERT(loadedPrefs.audioTrackId.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.audioTrackId, 1);
    TEST_ASSERT(loadedPrefs.subtitlesVisible.has_value());
    TEST_ASSERT_EQ(*loadedPrefs.subtitlesVisible, false);
}

TEST_CASE(MediaPreferences, UnknownFieldPreservation) {
    std::string jsonWithUnknowns = "{\n"
        "  \"id\": \"med_unk\",\n"
        "  \"filePath\": \".\\\\video_unk.mp4\",\n"
        "  \"title\": \"Unknown Fields Video\",\n"
        "  \"customMediaRating\": 5,\n"
        "  \"preferences\": {\n"
        "    \"playbackSpeed\": 1.25,\n"
        "    \"aspectRatio\": \"16:9\",\n"
        "    \"customEqualizerProfile\": \"BassBoost\",\n"
        "    \"vendorExtensionData\": {\n"
        "      \"pluginId\": \"com.example.hdr\",\n"
        "      \"colorTone\": 42\n"
        "    }\n"
        "  }\n"
        "}";

    JsonValue itemJson;
    TEST_ASSERT(JsonValue::parse(jsonWithUnknowns, itemJson));

    MediaItem item;
    item.id = itemJson.get("id").as_string();
    item.filePath = ResolveAppPath(itemJson.get("filePath").as_wstring());
    item.title = itemJson.get("title").as_wstring();

    const auto& pJson = itemJson.get("preferences");
    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = pJson.get("playbackSpeed").as_number();
    prefs.aspectRatio = pJson.get("aspectRatio").as_string();
    prefs.extraFields = pJson;
    item.preferences = std::move(prefs);
    item.extraFields = itemJson;

    // Modify a known preference
    item.preferences->playbackSpeed = 1.75;

    // Serialize
    JsonValue outObj = item.extraFields.is_object() ? item.extraFields : JsonValue::Object();
    outObj["id"] = item.id;
    if (item.preferences.has_value()) {
        JsonValue prefOut = item.preferences->extraFields.is_object() ? item.preferences->extraFields : JsonValue::Object();
        if (item.preferences->playbackSpeed.has_value()) prefOut["playbackSpeed"] = *item.preferences->playbackSpeed;
        if (item.preferences->aspectRatio.has_value()) prefOut["aspectRatio"] = *item.preferences->aspectRatio;
        outObj["preferences"] = prefOut;
    }

    std::string reloadedContent = outObj.stringify(false);
    JsonValue root;
    TEST_ASSERT(JsonValue::parse(reloadedContent, root));

    TEST_ASSERT_EQ(root.get("customMediaRating").as_int(), 5);
    const auto& pOut = root.get("preferences");
    TEST_ASSERT_EQ(pOut.get("playbackSpeed").as_number(), 1.75);
    TEST_ASSERT_EQ(pOut.get("aspectRatio").as_string(), "16:9");
    TEST_ASSERT_EQ(pOut.get("customEqualizerProfile").as_string(), "BassBoost");
    TEST_ASSERT(pOut.get("vendorExtensionData").is_object());
    TEST_ASSERT_EQ(pOut.get("vendorExtensionData").get("pluginId").as_string(), "com.example.hdr");
    TEST_ASSERT_EQ(pOut.get("vendorExtensionData").get("colorTone").as_int(), 42);
}

TEST_CASE(MediaPreferences, RelocationAndImportPreservation) {
    MediaLibrary lib;
    std::wstring originalPath = L"C:\\media\\episode1.mp4";
    std::wstring targetPath = L"C:\\relocated\\episode1.mp4";

    MediaItem* item = lib.AddOrUpdateMedia(originalPath);
    TEST_ASSERT(item != nullptr);

    MediaPlaybackPreferences prefs;
    prefs.playbackSpeed = 2.0;
    prefs.aspectRatio = "21:9";
    prefs.audioDelayMs = 100;
    prefs.subDelayMs = -200;
    prefs.subtitlesVisible = false;
    item->preferences = prefs;

    // Simulate in-place path relocation
    item->filePath = targetPath;
    lib.RebuildPathIndex();

    MediaItem* relocatedItem = lib.FindByPath(targetPath);
    TEST_ASSERT(relocatedItem != nullptr);
    TEST_ASSERT(relocatedItem->preferences.has_value());
    TEST_ASSERT_EQ(*relocatedItem->preferences->playbackSpeed, 2.0);
    TEST_ASSERT_EQ(*relocatedItem->preferences->aspectRatio, std::string("21:9"));
    TEST_ASSERT_EQ(*relocatedItem->preferences->audioDelayMs, 100);
    TEST_ASSERT_EQ(*relocatedItem->preferences->subDelayMs, -200);
    TEST_ASSERT_EQ(*relocatedItem->preferences->subtitlesVisible, false);
}

} // namespace Aperture::Test
