#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(CacheIsolation, DirectorySeparationUnderRoot) {
    std::wstring exeDir = GetExeDir();
    std::wstring dataDir = GetDataDir();
    std::wstring cacheDir = GetCacheDir();

    TEST_ASSERT(!dataDir.empty());
    TEST_ASSERT(!cacheDir.empty());

    TEST_ASSERT(IsPathUnderRoot(dataDir, exeDir));
    TEST_ASSERT(IsPathUnderRoot(cacheDir, exeDir));

    // Data and Cache must be separate directory trees
    TEST_ASSERT(_wcsicmp(dataDir.c_str(), cacheDir.c_str()) != 0);
    std::wstring expectedCache = exeDir + L"\\cache";
    TEST_ASSERT_EQ(cacheDir, expectedCache);
}

TEST_CASE(CacheIsolation, ClearDisposableCachePreservesDurableData) {
    ScopedTempDir tempDir(L"Aperture_Cache_Test_");
    std::wstring root = tempDir.GetPath();

    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring backupDir = tempDir.SubPath(L"data\\backups");
    std::wstring cacheDir = tempDir.SubPath(L"cache");
    std::wstring legacyShaderCache = tempDir.SubPath(L"data\\shader-cache");

    CreateDirectoryW(dataDir.c_str(), NULL);
    CreateDirectoryW(backupDir.c_str(), NULL);
    CreateDirectoryW(cacheDir.c_str(), NULL);
    CreateDirectoryW(legacyShaderCache.c_str(), NULL);

    // Create durable files
    std::wstring settingsFile = dataDir + L"\\settings.json";
    std::wstring libFile = dataDir + L"\\library.json";
    std::wstring backupFile = backupDir + L"\\settings.json.bak.1";
    TEST_ASSERT(AtomicWriteTextFile(settingsFile, "{\"durable\": true}").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(libFile, "[]").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(backupFile, "{\"backup\": true}").is_ok());

    // Create disposable cache files
    std::wstring cacheFile1 = cacheDir + L"\\shader_001.bin";
    std::wstring cacheFile2 = cacheDir + L"\\thumb_cache.tmp";
    std::wstring legacyCacheFile = legacyShaderCache + L"\\old_shader.bin";
    TEST_ASSERT(AtomicWriteTextFile(cacheFile1, "shader blob 1").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(cacheFile2, "thumb blob 2").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(legacyCacheFile, "legacy blob").is_ok());

    TEST_ASSERT(FileExists(cacheFile1));
    TEST_ASSERT(FileExists(cacheFile2));
    TEST_ASSERT(FileExists(legacyCacheFile));

    // Clear disposable cache
    StorageStatus cleanStatus = ClearDisposableCache(root);
    TEST_ASSERT(cleanStatus.is_ok());

    // Disposable cache files must be deleted
    TEST_ASSERT(!FileExists(cacheFile1));
    TEST_ASSERT(!FileExists(cacheFile2));
    TEST_ASSERT(!FileExists(legacyCacheFile));

    // Durable files MUST remain completely untouched!
    TEST_ASSERT(FileExists(settingsFile));
    TEST_ASSERT(FileExists(libFile));
    TEST_ASSERT(FileExists(backupFile));

    std::string sText, lText, bText;
    TEST_ASSERT(ReadTextFile(settingsFile, sText).is_ok());
    TEST_ASSERT(ReadTextFile(libFile, lText).is_ok());
    TEST_ASSERT(ReadTextFile(backupFile, bText).is_ok());

    TEST_ASSERT_EQ(sText, "{\"durable\": true}");
    TEST_ASSERT_EQ(lText, "[]");
    TEST_ASSERT_EQ(bText, "{\"backup\": true}");
}

TEST_CASE(CacheIsolation, ClearDisposableCacheRejectsInvalidRoot) {
    // Attempting to clear cache with an empty or root-escaping path fails safely
    StorageStatus status = ClearDisposableCache(L"");
    TEST_ASSERT(!status.is_ok());
}

} // namespace Aperture::Test
