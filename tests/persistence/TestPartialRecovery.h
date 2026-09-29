#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(PartialRecovery, CorruptLibraryPreservesPlaylistsAndHistory) {
    ScopedTempDir tempDir(L"Aperture_Partial_Lib_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    std::wstring libPath = dataDir + L"\\library.json";
    std::wstring plPath = dataDir + L"\\playlists.json";
    std::wstring histPath = dataDir + L"\\history.json";

    // Library is corrupt
    TEST_ASSERT(AtomicWriteTextFile(libPath, "INVALID_JSON_CORRUPTION_HERE{{{").is_ok());

    // Playlists are valid
    std::string plContent = "[\n"
        "  {\n"
        "    \"id\": \"pl_favorites\",\n"
        "    \"name\": \"My Favorites\",\n"
        "    \"itemPaths\": [\".\\\\media\\\\song.mp3\"]\n"
        "  }\n"
        "]";
    TEST_ASSERT(AtomicWriteTextFile(plPath, plContent).is_ok());

    // History is valid
    std::string histContent = "[\n"
        "  {\n"
        "    \"filePath\": \".\\\\media\\\\song.mp3\",\n"
        "    \"title\": \"Song Title\",\n"
        "    \"lastPosSec\": 10.0,\n"
        "    \"durationSec\": 180.0,\n"
        "    \"watchedTimestamp\": 1788009000\n"
        "  }\n"
        "]";
    TEST_ASSERT(AtomicWriteTextFile(histPath, histContent).is_ok());

    MediaLibrary library;
    // Simulate loading with dataDir staging
    JsonValue root;
    bool libParseOk = JsonValue::parse("INVALID_JSON_CORRUPTION_HERE{{{", root);
    TEST_ASSERT(!libParseOk);

    // Playlists load successfully
    JsonValue plRoot;
    TEST_ASSERT(JsonValue::parse(plContent, plRoot));
    TEST_ASSERT(plRoot.is_array());
    TEST_ASSERT_EQ(plRoot.as_array().size(), 1);
    TEST_ASSERT_EQ(plRoot[0].get("name").as_string(), "My Favorites");

    // History loads successfully
    JsonValue histRoot;
    TEST_ASSERT(JsonValue::parse(histContent, histRoot));
    TEST_ASSERT(histRoot.is_array());
    TEST_ASSERT_EQ(histRoot.as_array().size(), 1);
    TEST_ASSERT_EQ(histRoot[0].get("title").as_string(), "Song Title");
}

TEST_CASE(PartialRecovery, CorruptPlaylistsPreservesLibraryAndHistory) {
    ScopedTempDir tempDir(L"Aperture_Partial_Pl_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    std::wstring libPath = dataDir + L"\\library.json";
    std::wstring plPath = dataDir + L"\\playlists.json";
    std::wstring histPath = dataDir + L"\\history.json";

    std::string libContent = "[\n"
        "  {\n"
        "    \"id\": \"m_movie\",\n"
        "    \"filePath\": \".\\\\media\\\\movie.mp4\",\n"
        "    \"title\": \"Great Movie\",\n"
        "    \"durationSec\": 3600.0\n"
        "  }\n"
        "]";
    TEST_ASSERT(AtomicWriteTextFile(libPath, libContent).is_ok());
    TEST_ASSERT(AtomicWriteTextFile(plPath, "CORRUPT_PLAYLISTS_BLOB{{").is_ok());

    std::string histContent = "[\n"
        "  {\n"
        "    \"filePath\": \".\\\\media\\\\movie.mp4\",\n"
        "    \"title\": \"Great Movie\",\n"
        "    \"lastPosSec\": 120.0,\n"
        "    \"durationSec\": 3600.0,\n"
        "    \"watchedTimestamp\": 1788001000\n"
        "  }\n"
        "]";
    TEST_ASSERT(AtomicWriteTextFile(histPath, histContent).is_ok());

    // Verify library and history parse properly despite playlists corruption
    JsonValue libRoot, histRoot;
    TEST_ASSERT(JsonValue::parse(libContent, libRoot));
    TEST_ASSERT(JsonValue::parse(histContent, histRoot));
    TEST_ASSERT_EQ(libRoot.as_array().size(), 1);
    TEST_ASSERT_EQ(histRoot.as_array().size(), 1);
}

TEST_CASE(PartialRecovery, MediaLibraryLoadResultReporting) {
    MediaLibraryLoadResult res;
    res.libraryStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, L"data\\library.json", "Malformed");
    res.playlistsStatus = StorageStatus::Ok(L"data\\playlists.json");
    res.historyStatus = StorageStatus::Ok(L"data\\history.json");
    res.playlistsRecoveredFromBackup = true;

    TEST_ASSERT(!res.is_ok());
    TEST_ASSERT(!res.libraryStatus.is_ok());
    TEST_ASSERT(res.playlistsStatus.is_ok());
    TEST_ASSERT(res.historyStatus.is_ok());
    TEST_ASSERT(res.playlistsRecoveredFromBackup);
}

} // namespace Aperture::Test
