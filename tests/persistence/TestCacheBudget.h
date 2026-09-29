#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/Json.h"

#include <windows.h>
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(CacheBudget, TotalBudgetEnforcement) {
    ScopedTempDir tempDir(L"Aperture_Cache_TotalBudget_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.totalCeilingBytes = 5000;
    config.thumbnailBudgetBytes = 5000;
    config.shaderBudgetBytes = 5000;
    config.subtitleBudgetBytes = 5000;
    config.genericBudgetBytes = 5000;

    std::string payload1(1500, 'A');
    std::string payload2(1500, 'B');
    std::string payload3(1500, 'C');
    std::string payload4(1500, 'D');

    // Write file 1 (Thumbnails)
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb1.bin", payload1, config, root).is_ok());
    // Write file 2 (Shaders)
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Shaders, L"shader1.bin", payload2, config, root).is_ok());
    // Write file 3 (Generic)
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"misc1.bin", payload3, config, root).is_ok());

    CacheStats statsBefore = QueryCacheStats(root);
    TEST_ASSERT_EQ(statsBefore.totalBytes, 4500ULL);
    TEST_ASSERT_EQ(statsBefore.totalFiles, 3ULL);

    // Write file 4 (Subtitles) - adding 1500 bytes to 4500 exceeds 5000 bytes ceiling.
    // Total budget enforcement must evict the oldest file (thumb1.bin).
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Subtitles, L"sub1.bin", payload4, config, root).is_ok());

    CacheStats statsAfter = QueryCacheStats(root);
    TEST_ASSERT(statsAfter.totalBytes <= config.totalCeilingBytes);
    TEST_ASSERT_EQ(statsAfter.totalBytes, 4500ULL);
    TEST_ASSERT_EQ(statsAfter.totalFiles, 3ULL);

    // Oldest file (thumb1.bin) should have been evicted
    std::wstring thumbPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\thumb1.bin";
    std::wstring subPath = GetCacheDir(CacheClass::Subtitles, root) + L"\\sub1.bin";
    TEST_ASSERT(!FileExists(thumbPath));
    TEST_ASSERT(FileExists(subPath));

    std::string readContent;
    TEST_ASSERT(ReadCacheFile(CacheClass::Subtitles, L"sub1.bin", readContent, root).is_ok());
    TEST_ASSERT_EQ(readContent, payload4);
}

TEST_CASE(CacheBudget, PerClassBudgetEnforcement) {
    ScopedTempDir tempDir(L"Aperture_Cache_ClassBudget_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.totalCeilingBytes = 50000;
    config.thumbnailBudgetBytes = 4000; // 3000 hub bytes + 1000 reserved for seek previews
    config.shaderBudgetBytes = 3000;
    config.subtitleBudgetBytes = 3000;
    config.genericBudgetBytes = 3000;

    std::string payloadA(1200, '1');
    std::string payloadB(1200, '2');
    std::string payloadC(1200, '3');
    std::string shaderPayload(2000, 'S');

    // Write a shader cache file
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Shaders, L"pipeline.bin", shaderPayload, config, root).is_ok());

    // Write two library thumbnails (2400 bytes within the 3000-byte hub allowance).
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb_a.bin", payloadA, config, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb_b.bin", payloadB, config, root).is_ok());

    CacheStats statsMid = QueryCacheStats(root);
    TEST_ASSERT_EQ(statsMid.classStats[CacheClass::Thumbnails].totalBytes, 2400ULL);
    TEST_ASSERT_EQ(statsMid.classStats[CacheClass::Shaders].totalBytes, 2000ULL);

    // A third library thumbnail exceeds the 3000-byte hub allowance.
    // Eviction must evict the oldest thumbnail (thumb_a.bin), NOT the shader file.
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb_c.bin", payloadC, config, root).is_ok());

    CacheStats statsEnd = QueryCacheStats(root);
    TEST_ASSERT(statsEnd.classStats[CacheClass::Thumbnails].totalBytes <= config.thumbnailBudgetBytes);
    TEST_ASSERT_EQ(statsEnd.classStats[CacheClass::Thumbnails].totalBytes, 2400ULL);
    // Shader cache must be completely intact
    TEST_ASSERT_EQ(statsEnd.classStats[CacheClass::Shaders].totalBytes, 2000ULL);

    std::wstring thumbAPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\thumb_a.bin";
    std::wstring thumbBPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\thumb_b.bin";
    std::wstring thumbCPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\thumb_c.bin";
    std::wstring shaderPath = GetCacheDir(CacheClass::Shaders, root) + L"\\pipeline.bin";

    TEST_ASSERT(!FileExists(thumbAPath));
    TEST_ASSERT(FileExists(thumbBPath));
    TEST_ASSERT(FileExists(thumbCPath));
    TEST_ASSERT(FileExists(shaderPath));
}

TEST_CASE(CacheBudget, DeterministicOldestAccessEviction) {
    ScopedTempDir tempDir(L"Aperture_Cache_DeterministicLRU_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.totalCeilingBytes = 3500;
    config.genericBudgetBytes = 3500;

    std::string payload(1000, 'X');

    std::wstring file1 = GetCacheDir(CacheClass::Generic, root) + L"\\file1.dat";
    std::wstring file2 = GetCacheDir(CacheClass::Generic, root) + L"\\file2.dat";
    std::wstring file3 = GetCacheDir(CacheClass::Generic, root) + L"\\file3.dat";

    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"file1.dat", payload, config, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"file2.dat", payload, config, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"file3.dat", payload, config, root).is_ok());

    // Explicitly set timestamps to test deterministic ordering:
    // file1: oldest access time (100)
    // file2: newest access time (300)
    // file3: middle access time (200)
    auto setAccessTime = [](const std::wstring& p, uint64_t tick) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft;
            ft.dwLowDateTime = static_cast<DWORD>(tick & 0xFFFFFFFF);
            ft.dwHighDateTime = static_cast<DWORD>(tick >> 32);
            SetFileTime(h, &ft, &ft, &ft);
            CloseHandle(h);
        }
    };

    setAccessTime(file1, 10000000ULL);
    setAccessTime(file2, 30000000ULL);
    setAccessTime(file3, 20000000ULL);

    // Writing file4 (1000 bytes) requires evicting at least 1 file.
    // file1 has oldest access time, so it must be evicted first.
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"file4.dat", payload, config, root).is_ok());

    TEST_ASSERT(!FileExists(file1));
    TEST_ASSERT(FileExists(file2));
    TEST_ASSERT(FileExists(file3));

    std::wstring file4 = GetCacheDir(CacheClass::Generic, root) + L"\\file4.dat";
    TEST_ASSERT(FileExists(file4));
    setAccessTime(file4, 40000000ULL);

    // Writing file5 (1000 bytes) now requires evicting the next oldest, which is file3 (access time 200).
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"file5.dat", payload, config, root).is_ok());

    TEST_ASSERT(!FileExists(file1));
    TEST_ASSERT(!FileExists(file3));
    TEST_ASSERT(FileExists(file2));
    TEST_ASSERT(FileExists(file4));
    std::wstring file5 = GetCacheDir(CacheClass::Generic, root) + L"\\file5.dat";
    TEST_ASSERT(FileExists(file5));
}

TEST_CASE(CacheBudget, LockedFileTolerance) {
    ScopedTempDir tempDir(L"Aperture_Cache_LockedFile_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.totalCeilingBytes = 4000;
    config.genericBudgetBytes = 4000;

    std::string payload1(1500, '1');
    std::string payload2(1500, '2');
    std::string payload3(1500, '3');

    // Write file1 (oldest) and file2 (newer)
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"locked_file.dat", payload1, config, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"unlocked_file.dat", payload2, config, root).is_ok());

    std::wstring lockedPath = GetCacheDir(CacheClass::Generic, root) + L"\\locked_file.dat";
    std::wstring unlockedPath = GetCacheDir(CacheClass::Generic, root) + L"\\unlocked_file.dat";

    // Set locked_file to have older access time
    HANDLE hTouch = CreateFileW(lockedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTouch != INVALID_HANDLE_VALUE) {
        FILETIME ft{};
        ft.dwLowDateTime = 10000;
        SetFileTime(hTouch, &ft, &ft, &ft);
        CloseHandle(hTouch);
    }

    // Acquire exclusive lock on locked_file.dat (no share delete/read/write)
    HANDLE hLocked = CreateFileW(lockedPath.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    TEST_ASSERT(hLocked != INVALID_HANDLE_VALUE);

    // Attempt to write file3 (1500 bytes). Total budget is 4000, current usage is 3000.
    // 3000 + 1500 = 4500 > 4000. Eviction will first attempt to evict locked_file.dat (oldest),
    // which fails due to sharing violation. Eviction MUST tolerate the failure, proceed to unlocked_file.dat,
    // evict unlocked_file.dat, and successfully complete the write of file3!
    StorageStatus writeStatus = AtomicWriteCacheFile(CacheClass::Generic, L"incoming_file.dat", payload3, config, root);
    TEST_ASSERT(writeStatus.is_ok());

    std::wstring incomingPath = GetCacheDir(CacheClass::Generic, root) + L"\\incoming_file.dat";

    // Locked file must still exist and remain untouched
    TEST_ASSERT(FileExists(lockedPath));
    // Unlocked file must have been evicted
    TEST_ASSERT(!FileExists(unlockedPath));
    // Incoming file must exist and have correct content
    TEST_ASSERT(FileExists(incomingPath));

    CloseHandle(hLocked);
}

TEST_CASE(CacheBudget, PathContainmentAndUnsafePathRejection) {
    ScopedTempDir tempDir(L"Aperture_Cache_PathSafety_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;

    // Test path traversal outside cache root
    TEST_ASSERT(!IsCachePathSafe(L"..\\data\\settings.json", root));
    TEST_ASSERT(!IsCachePathSafe(L"..\\outside.txt", root));
    TEST_ASSERT(!IsCachePathSafe(L"C:\\Windows\\System32\\calc.exe", root));
    TEST_ASSERT(!IsCachePathSafe(L"", root));
    TEST_ASSERT(!IsCachePathSafe(root, root));
    TEST_ASSERT(!IsCachePathSafe(root + L"\\data", root));
    TEST_ASSERT(!IsCachePathSafe(root + L"\\cache", root)); // directory itself as a file
    TEST_ASSERT(!IsCachePathSafe(root + L"\\cache\\sub:stream", root)); // NT stream

    // Writing outside cache root MUST fail with InvalidPath
    StorageStatus s1 = AtomicWriteCacheFile(CacheClass::Generic, L"..\\..\\data\\hacked.json", "bad", config, root);
    TEST_ASSERT(!s1.is_ok());
    TEST_ASSERT_EQ(s1.code, StorageErrorCode::InvalidPath);

    StorageStatus s2 = AtomicWriteCacheFile(CacheClass::Generic, L"C:\\Windows\\hacked.bin", "bad", config, root);
    TEST_ASSERT(!s2.is_ok());
    TEST_ASSERT_EQ(s2.code, StorageErrorCode::InvalidPath);

    std::string outData;
    StorageStatus s3 = ReadCacheFile(CacheClass::Generic, L"..\\..\\data\\settings.json", outData, root);
    TEST_ASSERT(!s3.is_ok());
    TEST_ASSERT_EQ(s3.code, StorageErrorCode::InvalidPath);
}

TEST_CASE(CacheBudget, DisabledAndZeroBudget) {
    ScopedTempDir tempDir(L"Aperture_Cache_ZeroBudget_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.totalCeilingBytes = 0; // Disabled total ceiling

    TEST_ASSERT(!config.IsEnabled(CacheClass::Thumbnails));
    TEST_ASSERT(!config.IsEnabled(CacheClass::Shaders));

    StorageStatus status = AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb.bin", "data", config, root);
    TEST_ASSERT(!status.is_ok());
    TEST_ASSERT_EQ(status.code, StorageErrorCode::CacheBudgetExceeded);

    // Test with per-class budget = 0
    CacheBudgetConfig classZeroConfig;
    classZeroConfig.totalCeilingBytes = 50000;
    classZeroConfig.thumbnailBudgetBytes = 0; // Thumbnails disabled

    TEST_ASSERT(!classZeroConfig.IsEnabled(CacheClass::Thumbnails));
    TEST_ASSERT(classZeroConfig.IsEnabled(CacheClass::Shaders));

    StorageStatus sThumb = AtomicWriteCacheFile(CacheClass::Thumbnails, L"thumb.bin", "data", classZeroConfig, root);
    TEST_ASSERT(!sThumb.is_ok());
    TEST_ASSERT_EQ(sThumb.code, StorageErrorCode::CacheBudgetExceeded);

    // Shaders class is enabled, should succeed
    StorageStatus sShader = AtomicWriteCacheFile(CacheClass::Shaders, L"shader.bin", "shader_data", classZeroConfig, root);
    TEST_ASSERT(sShader.is_ok());
}

TEST_CASE(CacheBudget, NoDeletionOutsideCacheRoot) {
    ScopedTempDir tempDir(L"Aperture_Cache_DurablePreservation_");
    std::wstring root = tempDir.GetPath();

    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring backupDir = tempDir.SubPath(L"data\\backups");
    std::wstring cacheDir = tempDir.SubPath(L"cache");

    CreateDirectoryW(dataDir.c_str(), NULL);
    CreateDirectoryW(backupDir.c_str(), NULL);
    CreateDirectoryW(cacheDir.c_str(), NULL);

    std::wstring settingsFile = dataDir + L"\\settings.json";
    std::wstring libFile = dataDir + L"\\library.json";
    std::wstring backupFile = backupDir + L"\\library.json.bak.1";

    TEST_ASSERT(AtomicWriteTextFile(settingsFile, "{\"theme\": \"oled\"}").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(libFile, "[{\"title\": \"Movie 1\"}]").is_ok());
    TEST_ASSERT(AtomicWriteTextFile(backupFile, "{\"backup\": 1}").is_ok());

    CacheBudgetConfig smallConfig;
    smallConfig.totalCeilingBytes = 1000;
    smallConfig.genericBudgetBytes = 1000;

    std::string chunk(600, 'Z');
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"c1.dat", chunk, smallConfig, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"c2.dat", chunk, smallConfig, root).is_ok());
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"c3.dat", chunk, smallConfig, root).is_ok());

    // Clear all disposable cache
    TEST_ASSERT(ClearDisposableCache(root).is_ok());

    // Durable files MUST be 100% untouched and unchanged
    TEST_ASSERT(FileExists(settingsFile));
    TEST_ASSERT(FileExists(libFile));
    TEST_ASSERT(FileExists(backupFile));

    std::string sContent, lContent, bContent;
    TEST_ASSERT(ReadTextFile(settingsFile, sContent).is_ok());
    TEST_ASSERT(ReadTextFile(libFile, lContent).is_ok());
    TEST_ASSERT(ReadTextFile(backupFile, bContent).is_ok());

    TEST_ASSERT_EQ(sContent, "{\"theme\": \"oled\"}");
    TEST_ASSERT_EQ(lContent, "[{\"title\": \"Movie 1\"}]");
    TEST_ASSERT_EQ(bContent, "{\"backup\": 1}");
}

TEST_CASE(CacheBudget, MissingAndReadOnlyCacheRootHandling) {
    ScopedTempDir tempDir(L"Aperture_Cache_RootWritability_");
    std::wstring root = tempDir.GetPath();

    // Cache directory does not exist initially
    std::wstring cacheDir = root + L"\\cache";
    TEST_ASSERT(!DirectoryExists(cacheDir));

    // Check writability creates missing directory
    StorageStatus writeOk = CheckCacheDirectoryWritability(root, CacheClass::Thumbnails);
    TEST_ASSERT(writeOk.is_ok());
    TEST_ASSERT(DirectoryExists(cacheDir));

    // Mark cache directory as read-only
    SetFileAttributesW(cacheDir.c_str(), FILE_ATTRIBUTE_READONLY);

    StorageStatus roStatus = CheckCacheDirectoryWritability(root);
    TEST_ASSERT(!roStatus.is_ok());
    TEST_ASSERT_EQ(roStatus.code, StorageErrorCode::DirectoryNotWritable);
    TEST_ASSERT_EQ(roStatus.win32Error, (DWORD)ERROR_ACCESS_DENIED);

    // Restore attributes for clean teardown
    SetFileAttributesW(cacheDir.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(CacheBudget, SettingsCacheBudgetsIntegration) {
    ScopedTempDir tempDir(L"Aperture_Cache_Settings_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    CreateDirectoryW(dataDir.c_str(), NULL);
    std::wstring settingsPath = dataDir + L"\\settings.json";

    Settings settings;
    settings.cacheTotalBudgetMB = 1024;
    settings.cacheThumbnailBudgetMB = 256;
    settings.cacheShaderBudgetMB = 256;
    settings.cacheSubtitleBudgetMB = 128;
    settings.cacheGenericBudgetMB = 128;

    CacheBudgetConfig cfg = settings.GetCacheBudgets();
    TEST_ASSERT_EQ(cfg.totalCeilingBytes, 1024ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQ(cfg.thumbnailBudgetBytes, 256ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQ(cfg.shaderBudgetBytes, 256ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQ(cfg.subtitleBudgetBytes, 128ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQ(cfg.genericBudgetBytes, 128ULL * 1024ULL * 1024ULL);

    JsonValue root = JsonValue::Object();
    root["schemaVersion"] = 6;
    root["cacheTotalBudgetMB"] = 1024;
    root["cacheThumbnailBudgetMB"] = 256;
    root["cacheShaderBudgetMB"] = 256;
    root["cacheSubtitleBudgetMB"] = 128;
    root["cacheGenericBudgetMB"] = 128;

    std::string jsonStr = root.stringify(true);
    TEST_ASSERT(AtomicWriteTextFile(settingsPath, jsonStr).is_ok());

    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(jsonStr, parsed));
    TEST_ASSERT_EQ(parsed.get("cacheTotalBudgetMB").as_int(), 1024);
    TEST_ASSERT_EQ(parsed.get("cacheThumbnailBudgetMB").as_int(), 256);
}

} // namespace Aperture::Test
