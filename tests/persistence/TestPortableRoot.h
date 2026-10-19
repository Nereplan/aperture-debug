#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(PortableRoot, ContainmentAndCacheIsolation) {
    std::wstring exeDir = GetExeDir();
    TEST_ASSERT(!exeDir.empty());
    TEST_ASSERT(exeDir != L".");
    if (exeDir.size() > 3) {
        TEST_ASSERT(exeDir.back() != L'\\');
        TEST_ASSERT(exeDir.back() != L'/');
    }

    std::wstring dataDir = GetDataDir();
    std::wstring cacheDir = GetCacheDir();
    TEST_ASSERT_EQ(dataDir, exeDir + L"\\data");
    TEST_ASSERT_EQ(cacheDir, exeDir + L"\\cache");
    TEST_ASSERT(IsPathUnderRoot(dataDir, exeDir));
    TEST_ASSERT(IsPathUnderRoot(cacheDir, exeDir));
    TEST_ASSERT(_wcsicmp(dataDir.c_str(), cacheDir.c_str()) != 0);

    wchar_t appData[MAX_PATH] = { 0 };
    wchar_t localAppData[MAX_PATH] = { 0 };
    SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData);
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData);
    if (wcslen(appData) > 0 && !IsPathUnderRoot(exeDir, appData)) {
        TEST_ASSERT(!IsPathUnderRoot(dataDir, appData));
    }
    if (wcslen(localAppData) > 0 && !IsPathUnderRoot(exeDir, localAppData)) {
        TEST_ASSERT(!IsPathUnderRoot(dataDir, localAppData));
    }

    std::wstring baseDir = L"C:\\ApertureApp";
    TEST_ASSERT_EQ(ToRelocatablePath(L"C:\\ApertureApp\\media\\clip.mp4", baseDir), L".\\media\\clip.mp4");
    TEST_ASSERT_EQ(ResolveAppPath(L".\\videos\\movie.mp4", baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");
    TEST_ASSERT_EQ(ResolveAppPath(L"videos\\movie.mp4", baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");
    TEST_ASSERT_EQ(ResolveAppPath(L"./videos/movie.mp4", baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");

    ScopedTempDir tempDir(L"Aperture_PortableRoot_");
    std::wstring filePath = tempDir.SubPath(L"nested\\sub1\\test_atomic.txt");
    std::string testContent = "Hello Aperture Portable Persistence!\nLine 2 with UTF-8: 日本語 / 한국어 / äöü";
    TEST_ASSERT(AtomicWriteTextFile(filePath, testContent).is_ok());

    std::string readContent;
    TEST_ASSERT(ReadTextFile(filePath, readContent).is_ok());
    TEST_ASSERT_EQ(readContent, testContent);

    std::wstring nonExistent = tempDir.SubPath(L"does_not_exist_12345.json");
    StorageStatus missingStatus = ReadTextFile(nonExistent, readContent);
    TEST_ASSERT(!missingStatus.is_ok());
    TEST_ASSERT_EQ(missingStatus.code, StorageErrorCode::FileOpenFailed);

    std::wstring root = tempDir.GetPath();
    std::wstring stagedDataDir = tempDir.SubPath(L"data");
    std::wstring stagedBackupDir = tempDir.SubPath(L"data\\backups");
    std::wstring stagedCacheDir = tempDir.SubPath(L"cache");
    std::wstring legacyShaderCache = tempDir.SubPath(L"data\\shader-cache");
    CreateDirectoryW(stagedDataDir.c_str(), NULL);
    CreateDirectoryW(stagedBackupDir.c_str(), NULL);
    CreateDirectoryW(stagedCacheDir.c_str(), NULL);
    CreateDirectoryW(legacyShaderCache.c_str(), NULL);

    std::wstring settingsFile = stagedDataDir + L"\\settings.json";
    std::wstring libFile = stagedDataDir + L"\\library.json";
    std::wstring backupFile = stagedBackupDir + L"\\settings.json.bak.1";
    std::wstring cacheFile1 = stagedCacheDir + L"\\shader_001.bin";
    std::wstring legacyCacheFile = legacyShaderCache + L"\\old_shader.bin";

    TEST_ASSERT(AtomicWriteTextFile(settingsFile, "{\"durable\": true}").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(libFile, "[]").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(backupFile, "{\"backup\": true}").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(cacheFile1, "shader blob 1").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(legacyCacheFile, "legacy blob").is_ok());

    TEST_ASSERT(ClearDisposableCache(root).is_ok());
    TEST_ASSERT(!FileExists(cacheFile1));
    TEST_ASSERT(!FileExists(legacyCacheFile));
    TEST_ASSERT(FileExists(settingsFile));
    TEST_ASSERT(FileExists(libFile));
    TEST_ASSERT(FileExists(backupFile));
    TEST_ASSERT(!ClearDisposableCache(L"").is_ok());
}

} // namespace Aperture::Test
