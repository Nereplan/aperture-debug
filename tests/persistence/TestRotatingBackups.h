#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(RotatingBackups, RotationAndPartialRecovery) {
    std::wstring exeDir = GetExeDir();
    std::wstring backupDirGlobal = GetBackupDir();
    TEST_ASSERT(!backupDirGlobal.empty());
    TEST_ASSERT(IsPathUnderRoot(backupDirGlobal, exeDir));
    TEST_ASSERT_EQ(backupDirGlobal, GetDataDir() + L"\\backups");

    ScopedTempDir tempDir(L"Aperture_Bak_Seq_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring backupDir = tempDir.SubPath(L"data\\backups");
    CreateDirectoryW(dataDir.c_str(), NULL);
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::wstring testFile = dataDir + L"\\sample.json";
    std::wstring bak1 = backupDir + L"\\sample.json.bak.1";
    std::wstring bak2 = backupDir + L"\\sample.json.bak.2";
    std::wstring bak3 = backupDir + L"\\sample.json.bak.3";

    for (int v = 1; v <= 4; ++v) {
        TEST_ASSERT(AtomicWriteTextFile(testFile, "{\"v\": " + std::to_string(v) + "}").is_ok());
        TEST_ASSERT(CreateRotatingBackup(testFile, 3).is_ok());
    }

    std::string content4_b1, content4_b2, content4_b3;
    TEST_ASSERT(ReadTextFile(bak1, content4_b1).is_ok());
    TEST_ASSERT(ReadTextFile(bak2, content4_b2).is_ok());
    TEST_ASSERT(ReadTextFile(bak3, content4_b3).is_ok());
    TEST_ASSERT_EQ(content4_b1, "{\"v\": 4}");
    TEST_ASSERT_EQ(content4_b2, "{\"v\": 3}");
    TEST_ASSERT_EQ(content4_b3, "{\"v\": 2}");

    std::wstring settingsFile = dataDir + L"\\settings.json";
    std::wstring sbak1 = backupDir + L"\\settings.json.bak.1";
    std::wstring sbak2 = backupDir + L"\\settings.json.bak.2";
    TEST_ASSERT(AtomicWriteTextFile(sbak1, "{{{invalid json garbage...").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(sbak2, "{\"themePreset\": \"sunset\", \"volume\": 0.85}").is_ok());

    std::string recoveredText;
    StorageStatus restoreStatus = RestoreFromNewestValidBackup(settingsFile, recoveredText, [](const std::string& t) {
        JsonValue j;
        return JsonValue::parse(t, j) && j.is_object();
    });
    TEST_ASSERT(restoreStatus.is_ok());
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(recoveredText, parsed));
    TEST_ASSERT_EQ(parsed.get("themePreset").as_string(), "sunset");
    TEST_ASSERT_EQ(parsed.get("volume").as_number(), 0.85);

    ScopedTempDir partialDir(L"Aperture_Partial_Lib_");
    std::wstring pDataDir = partialDir.SubPath(L"data");
    CreateDirectoryW(pDataDir.c_str(), NULL);
    std::wstring libPath = pDataDir + L"\\library.json";
    std::wstring plPath = pDataDir + L"\\playlists.json";
    std::wstring histPath = pDataDir + L"\\history.json";

    TEST_ASSERT(AtomicWriteTextFile(libPath, "INVALID_JSON_CORRUPTION_HERE{{{").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(plPath, "[{\"id\":\"pl_favorites\",\"name\":\"My Favorites\",\"itemPaths\":[\".\\\\media\\\\song.mp3\"]}]").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(histPath, "[{\"filePath\":\".\\\\media\\\\song.mp3\",\"title\":\"Song Title\",\"lastPosSec\":10.0,\"durationSec\":180.0,\"watchedTimestamp\":1788009000}]").is_ok());

    MediaLibrary library;
    TEST_ASSERT(!library.LoadFromRoot(partialDir.GetPath()).is_ok());
    TEST_ASSERT(library.mediaItems.empty());
    TEST_ASSERT_EQ(library.playlists.at(0).name, "My Favorites");
    TEST_ASSERT_EQ(library.history.at(0).title, L"Song Title");
    TEST_ASSERT(!library.GetLastLoadResult().libraryStatus.is_ok());
    TEST_ASSERT(library.GetLastLoadResult().playlistsStatus.is_ok());
    TEST_ASSERT(library.GetLastLoadResult().historyStatus.is_ok());
    TEST_ASSERT(!library.Save().libraryStatus.is_ok());
    std::string preserved;
    TEST_ASSERT(ReadTextFile(libPath, preserved).is_ok());
    TEST_ASSERT_EQ(preserved, "INVALID_JSON_CORRUPTION_HERE{{{");
}

} // namespace Aperture::Test
