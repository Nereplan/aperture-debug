#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(RotatingBackups, BackupLocationUnderDataDirectoryOnly) {
    std::wstring exeDir = GetExeDir();
    std::wstring backupDir = GetBackupDir();

    TEST_ASSERT(!backupDir.empty());
    TEST_ASSERT(IsPathUnderRoot(backupDir, exeDir));

    std::wstring expectedBackupDir = GetDataDir() + L"\\backups";
    TEST_ASSERT_EQ(backupDir, expectedBackupDir);

    // Verify backupDir is within dataDir
    TEST_ASSERT_EQ(_wcsnicmp(backupDir.c_str(), GetDataDir().c_str(), GetDataDir().size()), 0);
}

TEST_CASE(RotatingBackups, BackupRotationSequence) {
    ScopedTempDir tempDir(L"Aperture_Bak_Seq_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring backupDir = tempDir.SubPath(L"data\\backups");
    CreateDirectoryW(dataDir.c_str(), NULL);
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::wstring testFile = dataDir + L"\\sample.json";

    // Version 1
    TEST_ASSERT(AtomicWriteTextFile(testFile, "{\"v\": 1}").is_ok());
    TEST_ASSERT(CreateRotatingBackup(testFile, 3).is_ok());

    std::wstring bak1 = backupDir + L"\\sample.json.bak.1";
    std::wstring bak2 = backupDir + L"\\sample.json.bak.2";
    std::wstring bak3 = backupDir + L"\\sample.json.bak.3";

    TEST_ASSERT(FileExists(bak1));
    std::string content1;
    TEST_ASSERT(ReadTextFile(bak1, content1).is_ok());
    TEST_ASSERT_EQ(content1, "{\"v\": 1}");

    // Version 2
    TEST_ASSERT(AtomicWriteTextFile(testFile, "{\"v\": 2}").is_ok());
    TEST_ASSERT(CreateRotatingBackup(testFile, 3).is_ok());

    TEST_ASSERT(FileExists(bak1));
    TEST_ASSERT(FileExists(bak2));
    std::string content2, content2_bak2;
    TEST_ASSERT(ReadTextFile(bak1, content2).is_ok());
    TEST_ASSERT(ReadTextFile(bak2, content2_bak2).is_ok());
    TEST_ASSERT_EQ(content2, "{\"v\": 2}");
    TEST_ASSERT_EQ(content2_bak2, "{\"v\": 1}");

    // Version 3
    TEST_ASSERT(AtomicWriteTextFile(testFile, "{\"v\": 3}").is_ok());
    TEST_ASSERT(CreateRotatingBackup(testFile, 3).is_ok());

    TEST_ASSERT(FileExists(bak1));
    TEST_ASSERT(FileExists(bak2));
    TEST_ASSERT(FileExists(bak3));
    std::string content3, content3_bak2, content3_bak3;
    TEST_ASSERT(ReadTextFile(bak1, content3).is_ok());
    TEST_ASSERT(ReadTextFile(bak2, content3_bak2).is_ok());
    TEST_ASSERT(ReadTextFile(bak3, content3_bak3).is_ok());
    TEST_ASSERT_EQ(content3, "{\"v\": 3}");
    TEST_ASSERT_EQ(content3_bak2, "{\"v\": 2}");
    TEST_ASSERT_EQ(content3_bak3, "{\"v\": 1}");

    // Version 4: maxBackups = 3, so v1 is dropped
    TEST_ASSERT(AtomicWriteTextFile(testFile, "{\"v\": 4}").is_ok());
    TEST_ASSERT(CreateRotatingBackup(testFile, 3).is_ok());

    std::string content4_b1, content4_b2, content4_b3;
    TEST_ASSERT(ReadTextFile(bak1, content4_b1).is_ok());
    TEST_ASSERT(ReadTextFile(bak2, content4_b2).is_ok());
    TEST_ASSERT(ReadTextFile(bak3, content4_b3).is_ok());
    TEST_ASSERT_EQ(content4_b1, "{\"v\": 4}");
    TEST_ASSERT_EQ(content4_b2, "{\"v\": 3}");
    TEST_ASSERT_EQ(content4_b3, "{\"v\": 2}");
}

TEST_CASE(RotatingBackups, RestoreFromNewestValidBackup) {
    ScopedTempDir tempDir(L"Aperture_Bak_Restore_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring backupDir = tempDir.SubPath(L"data\\backups");
    CreateDirectoryW(dataDir.c_str(), NULL);
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::wstring testFile = dataDir + L"\\settings.json";

    // Write a valid backup in .bak.2 and a corrupted backup in .bak.1
    std::wstring bak1 = backupDir + L"\\settings.json.bak.1";
    std::wstring bak2 = backupDir + L"\\settings.json.bak.2";

    TEST_ASSERT(AtomicWriteTextFile(bak1, "{{{invalid json garbage...").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(bak2, "{\"themePreset\": \"sunset\", \"volume\": 0.85}").is_ok());

    std::string recoveredText;
    StorageStatus restoreStatus = RestoreFromNewestValidBackup(testFile, recoveredText, [](const std::string& t) {
        JsonValue j;
        return JsonValue::parse(t, j) && j.is_object();
    });

    TEST_ASSERT(restoreStatus.is_ok());
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(recoveredText, parsed));
    TEST_ASSERT_EQ(parsed.get("themePreset").as_string(), "sunset");
    TEST_ASSERT_EQ(parsed.get("volume").as_number(), 0.85);
}

} // namespace Aperture::Test
