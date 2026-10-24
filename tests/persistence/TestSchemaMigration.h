#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/LibraryIncrementalStore.h"
#include "../../src/PlaybackSession.h"
#include "../../src/NetworkStream.h"
#include "../../src/ChannelPolicy.h"
#include "../../src/Json.h"

#include <set>
#include <string>
#include <vector>

namespace Aperture::Test {
inline bool ValidateManifest(const JsonValue& manifest, std::string& outError) {
    outError.clear();
    if (!manifest.is_object()) { outError = "Manifest is not a JSON object"; return false; }
    if (manifest.get("version").as_int(0) != 1) { outError = "Unsupported manifest format version"; return false; }
    if (!manifest.has_key("stores") || !manifest.get("stores").is_array()) { outError = "Missing stores array"; return false; }
    if (!manifest.has_key("nonVersionedStores") || !manifest.get("nonVersionedStores").is_array()) { outError = "Missing nonVersionedStores array"; return false; }

    const auto& stores = manifest.get("stores").as_array();
    if (stores.size() != 10) { outError = "Expected exactly 10 versioned stores, found " + std::to_string(stores.size()); return false; }

    std::set<std::string> storeIds;
    std::set<std::string> storePaths;

    for (const auto& store : stores) {
        if (!store.is_object()) { outError = "Store entry is not an object"; return false; }
        std::string id = store.get("id").as_string();
        std::string path = store.get("rootRelativePath").as_string();
        int version = store.get("schemaVersion").as_int(-1);
        bool isVersioned = store.get("isVersioned").as_bool(false);
        if (!isVersioned) { outError = "Store " + id + " must have isVersioned: true"; return false; }

        if (storeIds.count(id) > 0) { outError = "Duplicate store id: " + id; return false; }
        storeIds.insert(id);

        if (storePaths.count(path) > 0) { outError = "Duplicate store path: " + path; return false; }
        storePaths.insert(path);

        if (id == "settings") {
            if (path != "data/settings.json") { outError = "settings path mismatch"; return false; }
            if (version != Settings::kCurrentSettingsSchemaVersion) { outError = "settings version mismatch"; return false; }
            if (!store.get("atomicWrite").as_bool(false)) { outError = "settings must have atomicWrite: true"; return false; }
        } else if (id == "library") {
            if (path != "data/library.json") { outError = "library path mismatch"; return false; }
            if (version != MediaLibrary::kCurrentLibrarySchemaVersion) { outError = "library version mismatch"; return false; }
            if (!store.get("atomicWrite").as_bool(false)) { outError = "library must have atomicWrite: true"; return false; }
        } else if (id == "library_changes") {
            if (path != "data/library.changes") { outError = "library changes path mismatch"; return false; }
            if (version != LibraryIncrementalStore::kFormatVersion) { outError = "library changes version mismatch"; return false; }
            if (!store.get("atomicWrite").as_bool(false)) { outError = "library changes must have atomicWrite: true"; return false; }
        } else if (id == "playlists") {
            if (path != "data/playlists.json") { outError = "playlists path mismatch"; return false; }
            if (version != MediaLibrary::kCurrentPlaylistsSchemaVersion) { outError = "playlists version mismatch"; return false; }
            if (!store.get("atomicWrite").as_bool(false)) { outError = "playlists must have atomicWrite: true"; return false; }
        } else if (id == "history") {
            if (path != "data/history.json") { outError = "history path mismatch"; return false; }
            if (version != MediaLibrary::kCurrentHistorySchemaVersion) { outError = "history version mismatch"; return false; }
            if (!store.get("privateModeSuppression").as_bool(false)) { outError = "history must have privateModeSuppression: true"; return false; }
        } else if (id == "watched_folders") {
            if (path != "data/watched_folders.json") { outError = "watched_folders path mismatch"; return false; }
            if (version != MediaLibrary::kCurrentWatchedFoldersSchemaVersion) { outError = "watched_folders version mismatch"; return false; }
        } else if (id == "session") {
            if (path != "data/session.json") { outError = "session path mismatch"; return false; }
            if (version != PlaybackSession::kCurrentSessionSchemaVersion) { outError = "session version mismatch"; return false; }
            if (!store.get("privateModeSuppression").as_bool(false)) { outError = "session must have privateModeSuppression: true"; return false; }
        } else if (id == "recent_streams") {
            if (path != "recent_streams.json") { outError = "recent_streams path mismatch"; return false; }
            if (version != NetworkStreamModel::kCurrentStreamSchemaVersion) { outError = "recent_streams version mismatch"; return false; }
            if (!store.get("privateModeSuppression").as_bool(false)) { outError = "recent_streams must have privateModeSuppression: true"; return false; }
        } else if (id == "updater") {
            if (path != "data/updater.json") { outError = "updater path mismatch"; return false; }
            if (version != 1) { outError = "updater version mismatch"; return false; }
            if (!store.get("privateModeSuppression").as_bool(false)) { outError = "updater must have privateModeSuppression: true"; return false; }
        } else if (id == "channels") {
            if (path != "channels.json") { outError = "channels path mismatch"; return false; }
            if (version != 1) { outError = "channels version mismatch"; return false; }
        } else {
            outError = "Unmapped store id: " + id;
            return false;
        }
    }

    const auto& nvStores = manifest.get("nonVersionedStores").as_array();
    if (nvStores.size() < 8) { outError = "Too few nonVersionedStores"; return false; }

    std::set<std::string> nvIds;
    for (const auto& nv : nvStores) {
        if (!nv.is_object()) { outError = "nonVersionedStore is not object"; return false; }
        std::string id = nv.get("id").as_string();
        std::string rationale = nv.get("rationale").as_string();
        if (id.empty() || rationale.empty()) { outError = "Empty nonVersionedStore id or rationale"; return false; }
        if (nvIds.count(id) > 0) { outError = "Duplicate nonVersionedStore id: " + id; return false; }
        nvIds.insert(id);
    }

    if (nvIds.count("mpv_conf") == 0 || nvIds.count("mpv_conf_lkg") == 0 ||
        nvIds.count("diagnostics_log") == 0 || nvIds.count("crash_dumps") == 0 ||
        nvIds.count("diagnostics_exports") == 0 || nvIds.count("cache_thumbnails") == 0 ||
        nvIds.count("cache_shaders") == 0 || nvIds.count("screenshots") == 0) {
        outError = "Missing essential non-versioned store entry";
        return false;
    }

    return true;
}

TEST_CASE(SchemaMigration, MigrationAndLegacyCompatibility) {
    TEST_ASSERT_EQ(Settings::kCurrentSettingsSchemaVersion, 16);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentLibrarySchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentPlaylistsSchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentHistorySchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentWatchedFoldersSchemaVersion, 1);
    TEST_ASSERT_EQ(PlaybackSession::kCurrentSessionSchemaVersion, 1);
    TEST_ASSERT_EQ(NetworkStreamModel::kCurrentStreamSchemaVersion, 1);
    TEST_ASSERT_EQ(std::wstring(NetworkStreamModel::kDefaultRecentStreamsFileName), L"recent_streams.json");

    std::wstring manifestPath = L"docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\..\\..\\docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\docs\\portable_data_manifest.json";
    std::string manifestContent;
    TEST_ASSERT(ReadTextFile(manifestPath, manifestContent).is_ok());

    JsonValue baseManifest;
    TEST_ASSERT(JsonValue::parse(manifestContent, baseManifest));
    std::string valError;
    TEST_ASSERT(ValidateManifest(baseManifest, valError));
    TEST_ASSERT(valError.empty());

    std::wstring docPath = L"docs\\PORTABLE_DATA_MIGRATIONS.md";
    if (!FileExists(docPath)) docPath = GetExeDir() + L"\\..\\..\\docs\\PORTABLE_DATA_MIGRATIONS.md";
    if (!FileExists(docPath)) docPath = GetExeDir() + L"\\docs\\PORTABLE_DATA_MIGRATIONS.md";
    std::string docContent;
    TEST_ASSERT(ReadTextFile(docPath, docContent).is_ok());
    TEST_ASSERT(docContent.find("data\\settings.json") != std::string::npos);
    TEST_ASSERT(docContent.find("data\\library.json") != std::string::npos);

    JsonValue mutated1 = baseManifest;
    mutated1["stores"][0]["schemaVersion"] = 999;
    std::string err1;
    TEST_ASSERT(!ValidateManifest(mutated1, err1));
    TEST_ASSERT(err1.find("version mismatch") != std::string::npos);

    JsonValue mutated2 = baseManifest;
    mutated2["stores"].push_back(baseManifest["stores"][0]);
    std::string err2;
    TEST_ASSERT(!ValidateManifest(mutated2, err2));

    JsonValue mutated3 = baseManifest;
    mutated3["stores"][0]["id"] = "invented_external_store";
    std::string err3;
    TEST_ASSERT(!ValidateManifest(mutated3, err3));
    TEST_ASSERT(err3.find("Unmapped store id") != std::string::npos);

    std::string unversionedJson = "{\n"
        "  \"themePreset\": \"cyberpunk\",\n"
        "  \"accentColor\": \"#f43f5e\",\n"
        "  \"volume\": 0.75,\n"
        "  \"customExtensionProperty\": 42\n"
        "}";
    JsonValue root;
    TEST_ASSERT(JsonValue::parse(unversionedJson, root));
    TEST_ASSERT(!root.has_key("schemaVersion"));
    TEST_ASSERT(Settings::MigrateSchema(root, 0, Settings::kCurrentSettingsSchemaVersion));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), Settings::kCurrentSettingsSchemaVersion);
    TEST_ASSERT_EQ(root.get("themePreset").as_string(), "cyberpunk");
    TEST_ASSERT_EQ(root.get("accentColor").as_string(), "#f43f5e");
    TEST_ASSERT_EQ(root.get("volume").as_number(), 0.75);
    TEST_ASSERT_EQ(root.get("autoplay").as_bool(), true);
    TEST_ASSERT_EQ(root.get("storageMode").as_string(), "source");
    TEST_ASSERT_EQ(root.get("playlistViewMode").as_string(), "list");
    TEST_ASSERT_EQ(root.get("audioLang").as_string(), "eng");
    TEST_ASSERT_EQ(root.get("subLang").as_string(), "eng");
    TEST_ASSERT_EQ(root.get("appLanguage").as_string(), "en-US");
    TEST_ASSERT_EQ(root.get("customExtensionProperty").as_int(), 42);

    JsonValue v1 = JsonValue::Object();
    v1["schemaVersion"] = 1;
    v1["themePreset"] = "oled";
    v1["volume"] = 0.5;
    for (int stage = 1; stage < Settings::kCurrentSettingsSchemaVersion; ++stage) {
        TEST_ASSERT(Settings::MigrateSchema(v1, stage, stage + 1));
        TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), stage + 1);
    }
    TEST_ASSERT_EQ(v1.get("appLanguage").as_string(), "en-US");

    std::string jsonStr = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"shortcuts\": {\n"
        "    \"togglePlay\": \"Space\",\n"
        "    \"toggleMute\": \"M\"\n"
        "  }\n"
        "}";
    JsonValue scRoot;
    TEST_ASSERT(JsonValue::parse(jsonStr, scRoot));
    TEST_ASSERT(Settings::MigrateSchema(scRoot, 1, 4));
    JsonValue sc = scRoot.get("shortcuts");
    TEST_ASSERT_EQ(sc.get("togglePlay").get("vk").as_int(), (int)VK_SPACE);
    TEST_ASSERT_EQ(sc.get("toggleMute").get("vk").as_int(), 'M');

    std::string legacyLib = "[{\"id\":\"m1\",\"filePath\":\".\\\\media\\\\video1.mp4\",\"title\":\"Video 1\",\"durationSec\":120.0,\"status\":\"in_progress\",\"extraCustomRating\":5}]";
    JsonValue libRoot;
    TEST_ASSERT(JsonValue::parse(legacyLib, libRoot));
    TEST_ASSERT(MediaLibrary::MigrateLibrarySchema(libRoot, 1, 2));
    TEST_ASSERT_EQ(libRoot.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(libRoot.get("items")[0].get("extraCustomRating").as_int(), 5);

    std::string legacyPl = "[{\"id\":\"pl_1\",\"name\":\"Action Movies\",\"itemPaths\":[\".\\\\media\\\\action.mp4\"]}]";
    JsonValue plRoot;
    TEST_ASSERT(JsonValue::parse(legacyPl, plRoot));
    TEST_ASSERT(MediaLibrary::MigratePlaylistsSchema(plRoot, 1, 2));
    TEST_ASSERT_EQ(plRoot.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(plRoot.get("items")[0].get("name").as_string(), "Action Movies");

    std::string legacyHist = "[{\"filePath\":\".\\\\media\\\\action.mp4\",\"title\":\"Action Movie\",\"lastPosSec\":45.0,\"durationSec\":120.0,\"watchedTimestamp\":1788002000}]";
    JsonValue histRoot;
    TEST_ASSERT(JsonValue::parse(legacyHist, histRoot));
    TEST_ASSERT(MediaLibrary::MigrateHistorySchema(histRoot, 1, 2));
    TEST_ASSERT_EQ(histRoot.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(histRoot.get("items")[0].get("title").as_string(), "Action Movie");

    std::string legacyWf = "[\".\\\\media\\\\movies\",\".\\\\media\\\\tv\"]";
    JsonValue wfRoot;
    TEST_ASSERT(JsonValue::parse(legacyWf, wfRoot));
    TEST_ASSERT(MediaLibrary::MigrateWatchedFoldersSchema(wfRoot, 0, 1));
    TEST_ASSERT_EQ(wfRoot.get("schemaVersion").as_int(), 1);
    TEST_ASSERT_EQ(wfRoot.get("folders").as_array().size(), 2);

    std::string legacySession = "{\"currentFilePath\":\".\\\\media\\\\movie.mkv\",\"playbackPositionSec\":125.5}";
    JsonValue sRoot;
    TEST_ASSERT(JsonValue::parse(legacySession, sRoot));
    TEST_ASSERT(PlaybackSession::MigrateSchema(sRoot, 0, 1));
    TEST_ASSERT_EQ(sRoot.get("schemaVersion").as_int(), 1);
    TEST_ASSERT_EQ(sRoot.get("queueMode").as_string(), "direct");

    ScopedTempDir tempDir(L"Aperture_SchemaMigrationTest_");
    std::wstring sessionPath = tempDir.GetPath() + L"\\session.json";
    PlaybackSession session;
    session.OpenDirect(L"C:\\media\\test.mp4", 42.0);
    session.SetPrivateMode(true);
    TEST_ASSERT(session.Save(sessionPath).is_ok());
    TEST_ASSERT(!FileExists(sessionPath));
    session.SetPrivateMode(false);
    TEST_ASSERT(session.Save(sessionPath).is_ok());
    TEST_ASSERT(FileExists(sessionPath));

    std::string futureSettings = "{\"schemaVersion\":99,\"themePreset\":\"cyberpunk\",\"futureQuantumColorEngine\":true}";
    JsonValue fRoot;
    TEST_ASSERT(JsonValue::parse(futureSettings, fRoot));
    TEST_ASSERT(Settings::MigrateSchema(fRoot, 99, Settings::kCurrentSettingsSchemaVersion));
    TEST_ASSERT_EQ(fRoot.get("schemaVersion").as_int(), 99);
    TEST_ASSERT(fRoot.get("futureQuantumColorEngine").as_bool());
}

} // namespace Aperture::Test
