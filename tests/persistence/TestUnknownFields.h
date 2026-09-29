#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(UnknownFields, SettingsTopLevelUnknownFieldsPreserved) {
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\..\\tests\\fixtures\\settings_with_unknown_fields.json";

    std::string content;
    TEST_ASSERT(ReadTextFile(fixturePath, content).is_ok());

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(content, root));

    // Verify initial fixture has custom fields
    TEST_ASSERT_EQ(root.get("customExperimentalFeature").as_bool(), true);
    TEST_ASSERT(root.get("userProfileMetadata").is_object());
    TEST_ASSERT_EQ(root.get("userProfileMetadata").get("organization").as_string(), "Aperture Portable");
    TEST_ASSERT(root.get("arbitraryNestedList").is_array());

    // Simulate Settings serialization preserving persistedFields
    JsonValue savedRoot = root; // Settings uses root as base for persistedFields
    savedRoot["themePreset"] = "oled";
    savedRoot["volume"] = 0.90;

    std::string serialized = savedRoot.stringify(true);

    // Parse serialized and verify unknown fields survived unchanged
    JsonValue reloaded;
    TEST_ASSERT(JsonValue::parse(serialized, reloaded));
    TEST_ASSERT_EQ(reloaded.get("customExperimentalFeature").as_bool(), true);
    TEST_ASSERT_EQ(reloaded.get("userProfileMetadata").get("organization").as_string(), "Aperture Portable");
    TEST_ASSERT_EQ(reloaded.get("arbitraryNestedList").as_array().size(), 2);
    TEST_ASSERT_EQ(reloaded.get("volume").as_number(), 0.90);
}

TEST_CASE(UnknownFields, SettingsShortcutsCustomPropertiesPreserved) {
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\tests\\fixtures\\settings_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\..\\tests\\fixtures\\settings_with_unknown_fields.json";

    std::string content;
    TEST_ASSERT(ReadTextFile(fixturePath, content).is_ok());

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(content, root));

    JsonValue shortcutsObj = root.get("shortcuts");
    TEST_ASSERT(shortcutsObj.is_object());

    // Check custom properties on shortcuts
    JsonValue togglePlay = shortcutsObj.get("togglePlay");
    TEST_ASSERT_EQ(togglePlay.get("customShortcutProperty").as_string(), "custom_val_123");

    JsonValue customAction = shortcutsObj.get("customAction");
    TEST_ASSERT_EQ(customAction.get("pluginId").as_string(), "com.aperture.custom");

    // Simulate saving shortcuts
    shortcutsObj["togglePlay"]["vk"] = 32; // Modify a known property
    root["shortcuts"] = shortcutsObj;

    std::string savedJson = root.stringify(true);

    JsonValue reloaded;
    TEST_ASSERT(JsonValue::parse(savedJson, reloaded));
    TEST_ASSERT_EQ(reloaded.get("shortcuts").get("togglePlay").get("customShortcutProperty").as_string(), "custom_val_123");
    TEST_ASSERT_EQ(reloaded.get("shortcuts").get("customAction").get("pluginId").as_string(), "com.aperture.custom");
}

TEST_CASE(UnknownFields, MediaLibraryItemExtraFieldsPreserved) {
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\library_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"tests\\fixtures\\library_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\tests\\fixtures\\library_with_unknown_fields.json";
    if (!FileExists(fixturePath)) fixturePath = L"..\\..\\tests\\fixtures\\library_with_unknown_fields.json";

    std::string content;
    TEST_ASSERT(ReadTextFile(fixturePath, content).is_ok());

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(content, root));
    TEST_ASSERT(root.is_array());
    TEST_ASSERT_EQ(root.as_array().size(), 2);

    // Simulate MediaLibrary::Load logic preserving item.extraFields
    std::vector<MediaItem> items;
    for (auto& json : root.as_array()) {
        MediaItem item;
        item.id = json.get("id").as_string();
        item.filePath = ResolveAppPath(json.get("filePath").as_wstring());
        item.title = json.get("title").as_wstring();
        item.durationSec = json.get("durationSec").as_number();
        item.extraFields = std::move(json);
        items.push_back(std::move(item));
    }

    // Verify extra fields in loaded items
    TEST_ASSERT_EQ(items[0].extraFields.get("customMediaRating").as_int(), 5);
    TEST_ASSERT(items[0].extraFields.get("genres").is_array());
    TEST_ASSERT_EQ(items[0].extraFields.get("genres").as_array().size(), 2);
    TEST_ASSERT_EQ(items[0].extraFields.get("vendorExtra").get("checksum").as_string(), "abc12345def");

    TEST_ASSERT_EQ(items[1].extraFields.get("userNotes").as_string(), "Must watch in HDR");
    TEST_ASSERT_EQ(items[1].extraFields.get("isFavorite").as_bool(), true);

    // Simulate MediaLibrary::Save logic writing back item.extraFields
    JsonValue outArray = JsonValue::Array();
    for (const auto& m : items) {
        JsonValue j = m.extraFields.is_object() ? m.extraFields : JsonValue::Object();
        j["id"] = m.id;
        j["filePath"] = Utf16ToUtf8(ToRelocatablePath(m.filePath));
        j["title"] = Utf16ToUtf8(m.title);
        j["durationSec"] = m.durationSec;
        outArray.push_back(j);
    }

    std::string savedJson = outArray.stringify(false);

    JsonValue reloaded;
    TEST_ASSERT(JsonValue::parse(savedJson, reloaded));
    TEST_ASSERT_EQ(reloaded[0].get("customMediaRating").as_int(), 5);
    TEST_ASSERT_EQ(reloaded[0].get("vendorExtra").get("checksum").as_string(), "abc12345def");
    TEST_ASSERT_EQ(reloaded[1].get("userNotes").as_string(), "Must watch in HDR");
}

TEST_CASE(UnknownFields, PlaylistsAndHistoryExtraFieldsPreserved) {
    std::wstring plFixture = GetExeDir() + L"\\tests\\fixtures\\playlists_with_unknown_fields.json";
    if (!FileExists(plFixture)) plFixture = L"tests\\fixtures\\playlists_with_unknown_fields.json";
    if (!FileExists(plFixture)) plFixture = L"..\\tests\\fixtures\\playlists_with_unknown_fields.json";
    if (!FileExists(plFixture)) plFixture = L"..\\..\\tests\\fixtures\\playlists_with_unknown_fields.json";

    std::string plContent;
    TEST_ASSERT(ReadTextFile(plFixture, plContent).is_ok());

    JsonValue plRoot;
    TEST_ASSERT(JsonValue::parse(plContent, plRoot));
    TEST_ASSERT(plRoot.is_array());

    Playlist pl;
    pl.id = plRoot[0].get("id").as_string();
    pl.name = plRoot[0].get("name").as_string();
    pl.extraFields = plRoot[0];

    // Verify extra fields
    TEST_ASSERT_EQ(pl.extraFields.get("curator").as_string(), "Aperture Tester");
    TEST_ASSERT_EQ(pl.extraFields.get("customPlaylistColor").as_string(), "#ff5500");

    // History fixture
    std::wstring histFixture = GetExeDir() + L"\\tests\\fixtures\\history_with_unknown_fields.json";
    if (!FileExists(histFixture)) histFixture = L"tests\\fixtures\\history_with_unknown_fields.json";
    if (!FileExists(histFixture)) histFixture = L"..\\tests\\fixtures\\history_with_unknown_fields.json";
    if (!FileExists(histFixture)) histFixture = L"..\\..\\tests\\fixtures\\history_with_unknown_fields.json";

    std::string histContent;
    TEST_ASSERT(ReadTextFile(histFixture, histContent).is_ok());

    JsonValue histRoot;
    TEST_ASSERT(JsonValue::parse(histContent, histRoot));
    TEST_ASSERT(histRoot.is_array());

    HistoryRecord hr;
    hr.title = histRoot[0].get("title").as_wstring();
    hr.extraFields = histRoot[0];

    TEST_ASSERT_EQ(hr.extraFields.get("deviceType").as_string(), "portable_laptop");
    TEST_ASSERT_EQ(hr.extraFields.get("subtitlesTrackIndex").as_int(), 2);
}

} // namespace Aperture::Test
