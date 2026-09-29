#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(PortableRoot, ExeAndDataDirFormat) {
    std::wstring exeDir = GetExeDir();
    TEST_ASSERT(!exeDir.empty());
    TEST_ASSERT(exeDir != L".");
    // Ensure no trailing slash (unless root like C:\)
    if (exeDir.size() > 3) {
        TEST_ASSERT(exeDir.back() != L'\\');
        TEST_ASSERT(exeDir.back() != L'/');
    }

    std::wstring dataDir = GetDataDir();
    TEST_ASSERT(!dataDir.empty());
    std::wstring expectedDataDir = exeDir + L"\\data";
    TEST_ASSERT_EQ(dataDir, expectedDataDir);

    DWORD attr = GetFileAttributesW(dataDir.c_str());
    TEST_ASSERT(attr != INVALID_FILE_ATTRIBUTES);
    TEST_ASSERT(attr & FILE_ATTRIBUTE_DIRECTORY);
}

TEST_CASE(PortableRoot, AtomicWriteAndReadIntegrity) {
    ScopedTempDir tempDir;
    std::wstring filePath = tempDir.SubPath(L"test_atomic.txt");

    std::string testContent = "Hello Aperture Portable Persistence!\nLine 2 with UTF-8: 日本語 / 한국어 / äöü";
    StorageStatus writeStatus = AtomicWriteTextFile(filePath, testContent);
    TEST_ASSERT(writeStatus.is_ok());
    TEST_ASSERT_EQ(writeStatus.code, StorageErrorCode::Success);

    // Verify destination exists and contains exact content
    std::string readContent;
    StorageStatus readStatus = ReadTextFile(filePath, readContent);
    TEST_ASSERT(readStatus.is_ok());
    TEST_ASSERT_EQ(readContent, testContent);

    // Overwrite test
    std::string updatedContent = "Overwritten content!";
    StorageStatus overwriteStatus = AtomicWriteTextFile(filePath, updatedContent);
    TEST_ASSERT(overwriteStatus.is_ok());

    std::string readUpdated;
    TEST_ASSERT(ReadTextFile(filePath, readUpdated).is_ok());
    TEST_ASSERT_EQ(readUpdated, updatedContent);

    // Verify no temporary files (.tmp.*) left behind in directory
    for (const auto& entry : fs::directory_iterator(fs::path(tempDir.GetPath()))) {
        std::wstring filename = entry.path().filename().wstring();
        TEST_ASSERT_MSG(filename.find(L".tmp.") == std::wstring::npos, "Orphaned temp file found after atomic write");
    }
}

TEST_CASE(PortableRoot, AtomicWriteEmptyFile) {
    ScopedTempDir tempDir;
    std::wstring emptyFile = tempDir.SubPath(L"empty.txt");

    StorageStatus writeStatus = AtomicWriteTextFile(emptyFile, "");
    TEST_ASSERT(writeStatus.is_ok());

    std::string readContent = "initial_non_empty";
    StorageStatus readStatus = ReadTextFile(emptyFile, readContent);
    TEST_ASSERT(readStatus.is_ok());
    TEST_ASSERT(readContent.empty());
    TEST_ASSERT_EQ(readContent.size(), 0);
}

TEST_CASE(PortableRoot, AtomicWriteSubdirectoryCreation) {
    ScopedTempDir tempDir;
    std::wstring nestedFile = tempDir.SubPath(L"nested\\sub1\\sub2\\data.json");

    StorageStatus writeStatus = AtomicWriteTextFile(nestedFile, "{\"nested\": true}");
    TEST_ASSERT(writeStatus.is_ok());

    std::string readContent;
    TEST_ASSERT(ReadTextFile(nestedFile, readContent).is_ok());
    TEST_ASSERT_EQ(readContent, "{\"nested\": true}");
}

TEST_CASE(PortableRoot, ReadNonExistentFileReturnsError) {
    ScopedTempDir tempDir;
    std::wstring nonExistent = tempDir.SubPath(L"does_not_exist_12345.json");

    std::string out;
    StorageStatus status = ReadTextFile(nonExistent, out);
    TEST_ASSERT(!status.is_ok());
    TEST_ASSERT_EQ(status.code, StorageErrorCode::FileOpenFailed);
    TEST_ASSERT_EQ(status.win32Error, ERROR_FILE_NOT_FOUND);
}

TEST_CASE(PortableRoot, NoExternalStateLeakage) {
    // Verify that data storage is strictly portable and does not use APPDATA / LOCALAPPDATA
    wchar_t appData[MAX_PATH] = { 0 };
    wchar_t localAppData[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);

    std::wstring exeDir = GetExeDir();
    std::wstring dataDir = GetDataDir();

    TEST_ASSERT(IsPathUnderRoot(dataDir, exeDir));
    TEST_ASSERT_EQ(dataDir, exeDir + L"\\data");

    if (wcslen(appData) > 0 && !IsPathUnderRoot(exeDir, appData)) {
        TEST_ASSERT_MSG(!IsPathUnderRoot(dataDir, appData),
            "DataDir must not be under APPDATA");
    }
    if (wcslen(localAppData) > 0 && !IsPathUnderRoot(exeDir, localAppData)) {
        TEST_ASSERT_MSG(!IsPathUnderRoot(dataDir, localAppData),
            "DataDir must not be under LOCALAPPDATA");
    }
}

TEST_CASE(PortableRoot, SettingsStagedSaveAndLoad) {
    ScopedTempDir tempDir(L"Aperture_Settings_Test_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    std::wstring settingsPath = dataDir + L"\\settings.json";

    // Manually construct Settings JSON and write to staged data dir
    JsonValue s = JsonValue::Object();
    s["themePreset"] = "catppuccin-macchiato";
    s["accentColor"] = "#c6a0f6";
    s["volume"] = 0.65;
    s["rememberPos"] = true;
    s["storageMode"] = "source";
    s["playlistViewMode"] = "grid";

    TEST_ASSERT(AtomicWriteTextFile(settingsPath, s.stringify(true)).is_ok());

    // Read back and verify
    std::string readStr;
    TEST_ASSERT(ReadTextFile(settingsPath, readStr).is_ok());
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(readStr, parsed));
    TEST_ASSERT_EQ(parsed.get("themePreset").as_string(), "catppuccin-macchiato");
    TEST_ASSERT_EQ(parsed.get("accentColor").as_string(), "#c6a0f6");
    TEST_ASSERT_EQ(parsed.get("volume").as_number(), 0.65);
    TEST_ASSERT_EQ(parsed.get("playlistViewMode").as_string(), "grid");
}

TEST_CASE(PortableRoot, MediaLibraryStagedSaveAndLoad) {
    ScopedTempDir tempDir(L"Aperture_MediaLib_Test_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    std::wstring libPath = dataDir + L"\\library.json";
    std::wstring plPath = dataDir + L"\\playlists.json";
    std::wstring histPath = dataDir + L"\\history.json";

    // Construct library
    JsonValue lib = JsonValue::Array();
    JsonValue m1 = JsonValue::Object();
    m1["id"] = "m_1";
    m1["filePath"] = ".\\media\\clip1.mp4";
    m1["title"] = "Clip 1";
    m1["durationSec"] = 60.0;
    m1["lastPosSec"] = 30.0;
    m1["status"] = "in_progress";
    lib.push_back(m1);
    TEST_ASSERT(AtomicWriteTextFile(libPath, lib.stringify(false)).is_ok());

    // Construct playlists
    JsonValue pl = JsonValue::Array();
    JsonValue p1 = JsonValue::Object();
    p1["id"] = "pl_1";
    p1["name"] = "My Playlist";
    JsonValue items = JsonValue::Array();
    items.push_back(JsonValue(".\\media\\clip1.mp4"));
    p1["itemPaths"] = items;
    pl.push_back(p1);
    TEST_ASSERT(AtomicWriteTextFile(plPath, pl.stringify(false)).is_ok());

    // Construct history
    JsonValue hist = JsonValue::Array();
    JsonValue h1 = JsonValue::Object();
    h1["filePath"] = ".\\media\\clip1.mp4";
    h1["title"] = "Clip 1";
    h1["durationSec"] = 60.0;
    h1["lastPosSec"] = 30.0;
    h1["watchedTimestamp"] = 1788005000;
    hist.push_back(h1);
    TEST_ASSERT(AtomicWriteTextFile(histPath, hist.stringify(false)).is_ok());

    // Verify all 3 files exist and are loadable
    TEST_ASSERT(FileExists(libPath));
    TEST_ASSERT(FileExists(plPath));
    TEST_ASSERT(FileExists(histPath));

    std::string libContent, plContent, histContent;
    TEST_ASSERT(ReadTextFile(libPath, libContent).is_ok());
    TEST_ASSERT(ReadTextFile(plPath, plContent).is_ok());
    TEST_ASSERT(ReadTextFile(histPath, histContent).is_ok());

    JsonValue parsedLib, parsedPl, parsedHist;
    TEST_ASSERT(JsonValue::parse(libContent, parsedLib));
    TEST_ASSERT(JsonValue::parse(plContent, parsedPl));
    TEST_ASSERT(JsonValue::parse(histContent, parsedHist));

    TEST_ASSERT_EQ(parsedLib.as_array().size(), 1);
    TEST_ASSERT_EQ(parsedPl.as_array().size(), 1);
    TEST_ASSERT_EQ(parsedHist.as_array().size(), 1);
}
} // namespace Aperture::Test
