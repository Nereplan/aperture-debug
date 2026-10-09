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

TEST_CASE(SchemaMigration, ManifestAndDocumentationParity) {
    // 1. Direct compiled API constants parity
    TEST_ASSERT_EQ(Settings::kCurrentSettingsSchemaVersion, 15);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentLibrarySchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentPlaylistsSchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentHistorySchemaVersion, 2);
    TEST_ASSERT_EQ(MediaLibrary::kCurrentWatchedFoldersSchemaVersion, 1);
    TEST_ASSERT_EQ(PlaybackSession::kCurrentSessionSchemaVersion, 1);
    TEST_ASSERT_EQ(NetworkStreamModel::kCurrentStreamSchemaVersion, 1);
    TEST_ASSERT_EQ(std::wstring(NetworkStreamModel::kDefaultRecentStreamsFileName), L"recent_streams.json");

    // 2. Read and validate machine-readable manifest
    std::wstring manifestPath = L"docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\..\\..\\docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\docs\\portable_data_manifest.json";
    std::string manifestContent;
    StorageStatus readStatus = ReadTextFile(manifestPath, manifestContent);
    TEST_ASSERT(readStatus.is_ok());

    JsonValue manifest;
    TEST_ASSERT(JsonValue::parse(manifestContent, manifest));
    std::string valError;
    TEST_ASSERT(ValidateManifest(manifest, valError));
    TEST_ASSERT(valError.empty());

    // 3. Verify documentation file existence and content
    std::wstring docPath = L"docs\\PORTABLE_DATA_MIGRATIONS.md";
    if (!FileExists(docPath)) docPath = GetExeDir() + L"\\..\\..\\docs\\PORTABLE_DATA_MIGRATIONS.md";
    if (!FileExists(docPath)) docPath = GetExeDir() + L"\\docs\\PORTABLE_DATA_MIGRATIONS.md";
    std::string docContent;
    StorageStatus docStatus = ReadTextFile(docPath, docContent);
    TEST_ASSERT(docStatus.is_ok());
    TEST_ASSERT(docContent.find("data\\settings.json") != std::string::npos);
    TEST_ASSERT(docContent.find("data\\library.json") != std::string::npos);
    TEST_ASSERT(docContent.find("data\\playlists.json") != std::string::npos);
    TEST_ASSERT(docContent.find("data\\history.json") != std::string::npos);
    TEST_ASSERT(docContent.find("data\\session.json") != std::string::npos);
}

TEST_CASE(SchemaMigration, ManifestDriftDetectionRejection) {
    std::wstring manifestPath = L"docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\..\\..\\docs\\portable_data_manifest.json";
    if (!FileExists(manifestPath)) manifestPath = GetExeDir() + L"\\docs\\portable_data_manifest.json";
    std::string manifestContent;
    TEST_ASSERT(ReadTextFile(manifestPath, manifestContent).is_ok());

    JsonValue baseManifest;
    TEST_ASSERT(JsonValue::parse(manifestContent, baseManifest));

    // Drift 1: Stale / wrong schemaVersion in manifest
    JsonValue mutated1 = baseManifest;
    mutated1["stores"][0]["schemaVersion"] = 999;
    std::string err1;
    TEST_ASSERT(!ValidateManifest(mutated1, err1));
    TEST_ASSERT(err1.find("version mismatch") != std::string::npos);

    // Drift 2: Duplicate store id
    JsonValue mutated2 = baseManifest;
    mutated2["stores"].push_back(baseManifest["stores"][0]);
    std::string err2;
    TEST_ASSERT(!ValidateManifest(mutated2, err2));

    // Drift 3: Unknown / unmapped store
    JsonValue mutated3 = baseManifest;
    mutated3["stores"][0]["id"] = "invented_external_store";
    std::string err3;
    TEST_ASSERT(!ValidateManifest(mutated3, err3));
    TEST_ASSERT(err3.find("Unmapped store id") != std::string::npos);
}
TEST_CASE(SchemaMigration, SettingsUnversionedToCurrent) {
    // Unversioned legacy JSON (v0)
    std::string unversionedJson = "{\n"
        "  \"themePreset\": \"cyberpunk\",\n"
        "  \"accentColor\": \"#f43f5e\",\n"
        "  \"volume\": 0.75,\n"
        "  \"customExtensionProperty\": 42\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(unversionedJson, root));
    TEST_ASSERT(!root.has_key("schemaVersion"));

    // Run migration
    TEST_ASSERT(Settings::MigrateSchema(root, 0, Settings::kCurrentSettingsSchemaVersion));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), Settings::kCurrentSettingsSchemaVersion);
    TEST_ASSERT_EQ(root.get("themePreset").as_string(), "cyberpunk");
    TEST_ASSERT_EQ(root.get("accentColor").as_string(), "#f43f5e");
    TEST_ASSERT_EQ(root.get("volume").as_number(), 0.75);

    // Verify newly migrated default fields
    TEST_ASSERT_EQ(root.get("autoplay").as_bool(), true);
    TEST_ASSERT_EQ(root.get("storageMode").as_string(), "source");
    TEST_ASSERT_EQ(root.get("playlistViewMode").as_string(), "list");
    TEST_ASSERT_EQ(root.get("audioLang").as_string(), "eng");
    TEST_ASSERT_EQ(root.get("subLang").as_string(), "eng");
    TEST_ASSERT_EQ(root.get("appLanguage").as_string(), "en-US");
    // Verify custom extension property survived
    TEST_ASSERT_EQ(root.get("customExtensionProperty").as_int(), 42);
}

TEST_CASE(SchemaMigration, SettingsVersionStagesDeterministic) {
    // v1 JSON
    JsonValue v1 = JsonValue::Object();
    v1["schemaVersion"] = 1;
    v1["themePreset"] = "oled";
    v1["volume"] = 0.5;

    TEST_ASSERT(Settings::MigrateSchema(v1, 1, 2));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(v1.get("audioLang").as_string(), "eng");
    TEST_ASSERT_EQ(v1.get("subLang").as_string(), "eng");

    TEST_ASSERT(Settings::MigrateSchema(v1, 2, 3));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 3);
    TEST_ASSERT_EQ(v1.get("storageMode").as_string(), "source");
    TEST_ASSERT_EQ(v1.get("playlistViewMode").as_string(), "list");
    TEST_ASSERT_EQ(v1.get("windowWidth").as_int(), 1280);

    TEST_ASSERT(Settings::MigrateSchema(v1, 3, 4));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 4);
    TEST_ASSERT_EQ(v1.get("lastAudioTrackTitle").as_string(), "");
    TEST_ASSERT_EQ(v1.get("lastSubtitleTrackTitle").as_string(), "");

    TEST_ASSERT(Settings::MigrateSchema(v1, 4, 5));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 5);
    TEST_ASSERT_EQ(v1.get("restoreSession").as_bool(), true);

    TEST_ASSERT(Settings::MigrateSchema(v1, 5, 6));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 6);
    TEST_ASSERT_EQ(v1.get("cacheTotalBudgetMB").as_int(), 160);
    TEST_ASSERT_EQ(v1.get("cacheThumbnailBudgetMB").as_int(), 64);
    TEST_ASSERT_EQ(v1.get("cacheShaderBudgetMB").as_int(), 64);
    TEST_ASSERT_EQ(v1.get("cacheSubtitleBudgetMB").as_int(), 16);
    TEST_ASSERT_EQ(v1.get("cacheGenericBudgetMB").as_int(), 16);

    TEST_ASSERT(Settings::MigrateSchema(v1, 6, 7));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 7);
    TEST_ASSERT_EQ(v1.get("hdrMode").as_string(), "auto");
    TEST_ASSERT_EQ(v1.get("toneMappingAlgorithm").as_string(), "auto");
    TEST_ASSERT_EQ(v1.get("targetPeak").as_int(), 0);
    TEST_ASSERT_EQ(v1.get("gamutMapping").as_string(), "auto");
    TEST_ASSERT_EQ(v1.get("iccProfilePath").as_string(), "");

    TEST_ASSERT(Settings::MigrateSchema(v1, 7, 8));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 8);
    TEST_ASSERT_EQ(v1.get("audioChannelMix").as_string(), "auto");
    TEST_ASSERT_EQ(v1.get("audioNormalizeDownmix").as_bool(), true);
    TEST_ASSERT_EQ(v1.get("nightMode").as_string(), "off");

    TEST_ASSERT(Settings::MigrateSchema(v1, 8, 9));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 9);
    TEST_ASSERT(v1.get("subtitleProfiles").is_array());
    TEST_ASSERT_EQ(v1.get("activeSubtitleProfile").as_string(), "");
    TEST_ASSERT_EQ(v1.get("subSafeAreaX").as_int(), 0);
    TEST_ASSERT_EQ(v1.get("subSafeAreaY").as_int(), 0);

    TEST_ASSERT(Settings::MigrateSchema(v1, 9, 10));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 10);
    TEST_ASSERT_EQ(v1.get("onlineSubtitlesEnabled").as_bool(), false);
    TEST_ASSERT_EQ(v1.get("onlineSubtitleEndpoint").as_string(), "");

    TEST_ASSERT(Settings::MigrateSchema(v1, 10, 11));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 11);
    TEST_ASSERT_EQ(v1.get("customMpvConfigEnabled").as_bool(), false);
    TEST_ASSERT_EQ(v1.get("customMpvConfigPath").as_string(), "mpv.conf");

    TEST_ASSERT(Settings::MigrateSchema(v1, 11, 12));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 12);
    TEST_ASSERT_EQ(v1.get("jumpListEnabled").as_bool(), false);

    TEST_ASSERT(Settings::MigrateSchema(v1, 12, 13));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 13);
    TEST_ASSERT_EQ(v1.get("hoverPreviewEnabled").as_bool(), true);
    TEST_ASSERT_EQ(v1.get("reduceMotionEnabled").as_bool(), false);

    TEST_ASSERT(Settings::MigrateSchema(v1, 13, 14));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 14);
    TEST_ASSERT_EQ(v1.get("autoUpdateCheckEnabled").as_bool(), false);
    TEST_ASSERT_EQ(v1.get("updateChannel").as_string(), "stable");
    TEST_ASSERT_EQ(v1.get("updateCheckUrl").as_string(), "");

    TEST_ASSERT(Settings::MigrateSchema(v1, 14, 15));
    TEST_ASSERT_EQ(v1.get("schemaVersion").as_int(), 15);
    TEST_ASSERT_EQ(v1.get("appLanguage").as_string(), "en-US");
}

TEST_CASE(SchemaMigration, SettingsLegacyShortcutStringConversion) {
    std::string jsonStr = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"shortcuts\": {\n"
        "    \"togglePlay\": \"Space\",\n"
        "    \"toggleMute\": \"M\"\n"
        "  }\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(jsonStr, root));
    TEST_ASSERT(Settings::MigrateSchema(root, 1, 4));

    JsonValue sc = root.get("shortcuts");
    TEST_ASSERT(sc.is_object());
    TEST_ASSERT(sc.get("togglePlay").is_object());
    TEST_ASSERT_EQ(sc.get("togglePlay").get("vk").as_int(), (int)VK_SPACE);
    TEST_ASSERT_EQ(sc.get("togglePlay").get("key").as_string(), "Space");
    TEST_ASSERT_EQ(sc.get("toggleMute").get("vk").as_int(), 'M');
}

TEST_CASE(SchemaMigration, MediaLibraryArrayMigrationToVersion2) {
    std::string legacyLib = "[\n"
        "  {\n"
        "    \"id\": \"m1\",\n"
        "    \"filePath\": \".\\\\media\\\\video1.mp4\",\n"
        "    \"title\": \"Video 1\",\n"
        "    \"durationSec\": 120.0,\n"
        "    \"status\": \"in_progress\",\n"
        "    \"extraCustomRating\": 5\n"
        "  }\n"
        "]";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(legacyLib, root));
    TEST_ASSERT(root.is_array());

    TEST_ASSERT(MediaLibrary::MigrateLibrarySchema(root, 1, 2));
    TEST_ASSERT(root.is_object());
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 2);
    TEST_ASSERT(root.get("items").is_array());
    TEST_ASSERT_EQ(root.get("items").as_array().size(), 1);

    const JsonValue& item = root.get("items")[0];
    TEST_ASSERT_EQ(item.get("id").as_string(), "m1");
    TEST_ASSERT_EQ(item.get("title").as_string(), "Video 1");
    TEST_ASSERT_EQ(item.get("extraCustomRating").as_int(), 5);
}

TEST_CASE(SchemaMigration, PlaylistsAndHistoryArrayMigrationToVersion2) {
    // Playlists migration
    std::string legacyPl = "[\n"
        "  {\n"
        "    \"id\": \"pl_1\",\n"
        "    \"name\": \"Action Movies\",\n"
        "    \"itemPaths\": [\".\\\\media\\\\action.mp4\"]\n"
        "  }\n"
        "]";

    JsonValue plRoot;
    TEST_ASSERT(JsonValue::parse(legacyPl, plRoot));
    TEST_ASSERT(MediaLibrary::MigratePlaylistsSchema(plRoot, 1, 2));
    TEST_ASSERT(plRoot.is_object());
    TEST_ASSERT_EQ(plRoot.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(plRoot.get("items").as_array().size(), 1);
    TEST_ASSERT_EQ(plRoot.get("items")[0].get("name").as_string(), "Action Movies");

    // History migration
    std::string legacyHist = "[\n"
        "  {\n"
        "    \"filePath\": \".\\\\media\\\\action.mp4\",\n"
        "    \"title\": \"Action Movie\",\n"
        "    \"lastPosSec\": 45.0,\n"
        "    \"durationSec\": 120.0,\n"
        "    \"watchedTimestamp\": 1788002000\n"
        "  }\n"
        "]";

    JsonValue histRoot;
    TEST_ASSERT(JsonValue::parse(legacyHist, histRoot));
    TEST_ASSERT(MediaLibrary::MigrateHistorySchema(histRoot, 1, 2));
    TEST_ASSERT(histRoot.is_object());
    TEST_ASSERT_EQ(histRoot.get("schemaVersion").as_int(), 2);
    TEST_ASSERT_EQ(histRoot.get("items").as_array().size(), 1);
    TEST_ASSERT_EQ(histRoot.get("items")[0].get("title").as_string(), "Action Movie");
}

TEST_CASE(SchemaMigration, WatchedFoldersMigrationToVersion1) {
    std::string legacyWf = "[\n"
        "  \".\\\\media\\\\movies\",\n"
        "  \".\\\\media\\\\tv\"\n"
        "]";

    JsonValue wfRoot;
    TEST_ASSERT(JsonValue::parse(legacyWf, wfRoot));
    TEST_ASSERT(MediaLibrary::MigrateWatchedFoldersSchema(wfRoot, 0, 1));
    TEST_ASSERT(wfRoot.is_object());
    TEST_ASSERT_EQ(wfRoot.get("schemaVersion").as_int(), 1);
    TEST_ASSERT(wfRoot.get("folders").is_array());
    TEST_ASSERT_EQ(wfRoot.get("folders").as_array().size(), 2);
    TEST_ASSERT_EQ(wfRoot.get("folders")[0].as_string(), ".\\media\\movies");
    TEST_ASSERT_EQ(wfRoot.get("folders")[1].as_string(), ".\\media\\tv");
}

TEST_CASE(SchemaMigration, PlaybackSessionMigrationToVersion1) {
    std::string legacySession = "{\n"
        "  \"currentFilePath\": \".\\\\media\\\\movie.mkv\",\n"
        "  \"playbackPositionSec\": 125.5\n"
        "}";

    JsonValue sRoot;
    TEST_ASSERT(JsonValue::parse(legacySession, sRoot));
    TEST_ASSERT(PlaybackSession::MigrateSchema(sRoot, 0, 1));
    TEST_ASSERT(sRoot.is_object());
    TEST_ASSERT_EQ(sRoot.get("schemaVersion").as_int(), 1);
    TEST_ASSERT_EQ(sRoot.get("queueMode").as_string(), "direct");
    TEST_ASSERT_EQ(sRoot.get("currentFilePath").as_string(), ".\\media\\movie.mkv");
    TEST_ASSERT_EQ(sRoot.get("playbackPositionSec").as_number(), 125.5);
    TEST_ASSERT(sRoot.get("queue").is_array());
}

TEST_CASE(SchemaMigration, PlaybackSessionPrivateModeSuppression) {
    ScopedTempDir tempDir(L"Aperture_SessionPrivateModeTest");
    std::wstring sessionPath = tempDir.GetPath() + L"\\session.json";

    PlaybackSession session;
    session.OpenDirect(L"C:\\media\\test.mp4", 42.0);
    session.SetPrivateMode(true);

    StorageStatus saveRes = session.Save(sessionPath);
    TEST_ASSERT(saveRes.is_ok());
    TEST_ASSERT(!FileExists(sessionPath)); // Must NOT create file on disk

    session.SetPrivateMode(false);
    saveRes = session.Save(sessionPath);
    TEST_ASSERT(saveRes.is_ok());
    TEST_ASSERT(FileExists(sessionPath)); // Now saved normally
}

TEST_CASE(SchemaMigration, ForwardCompatibilityAndUnknownFieldsPreserved) {
    // Settings with future schema version and unknown properties
    std::string futureSettings = "{\n"
        "  \"schemaVersion\": 99,\n"
        "  \"themePreset\": \"cyberpunk\",\n"
        "  \"futureQuantumColorEngine\": true,\n"
        "  \"experimentalAiUpscaling\": {\n"
        "    \"model\": \"v3-ultra\",\n"
        "    \"passes\": 4\n"
        "  }\n"
        "}";

    JsonValue sRoot;
    TEST_ASSERT(JsonValue::parse(futureSettings, sRoot));

    // When loaded with version >= target, MigrateSchema is a clean no-op
    TEST_ASSERT(Settings::MigrateSchema(sRoot, 99, Settings::kCurrentSettingsSchemaVersion));
    TEST_ASSERT_EQ(sRoot.get("schemaVersion").as_int(), 99);
    TEST_ASSERT(sRoot.get("futureQuantumColorEngine").as_bool());
    TEST_ASSERT(sRoot.get("experimentalAiUpscaling").is_object());
    TEST_ASSERT_EQ(sRoot.get("experimentalAiUpscaling").get("model").as_string(), "v3-ultra");
}

} // namespace Aperture::Test
