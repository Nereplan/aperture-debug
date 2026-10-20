#pragma once

#include "../support/TestFramework.h"
#include "../../src/MpvPlayer.h"
#include "../../src/ThumbnailMigration.h"

namespace Aperture::Test {

TEST_CASE(HubThumbnailCache, DefaultWritersHonorActiveDisabledBudget) {
    struct Restore { CacheBudgetConfig saved = GetGlobalCacheBudgets(); ~Restore() { SetGlobalCacheBudgets(saved); } } restore;
    CacheBudgetConfig disabled; disabled.thumbnailBudgetBytes = 0;
    SetGlobalCacheBudgets(disabled);
    PreviewThumbnailFrame frame;
    frame.filePath = L"C:\\Aperture-tests\\disabled-thumbnail-setting.mkv";
    frame.width = 32; frame.height = 18; frame.bgraPixels.assign(32 * 18 * 4, 255);
    // Exercise the default arguments used by background hub persistence.
    TEST_ASSERT(!SaveHubThumbnailToDiskCache(frame));
}

TEST_CASE(HubThumbnailCache, PrivateReadDoesNotMigrateLegacyCache) {
    ScopedTempDir tempDir(L"Aperture_Thumb_Private_");
    const auto root = tempDir.GetPath();
    const std::wstring path = L"C:\\Media\\private-cache-read.mkv";
    const auto key = MakeHubThumbnailCacheKey(path, 320, 180);
    ThumbnailDiskHeader header;
    header.width = 32; header.height = 18; header.stride = 128; header.pixelBytes = 32 * 18 * 4;
    std::string legacy(sizeof(header) + header.pixelBytes, static_cast<char>(255));
    std::memcpy(legacy.data(), &header, sizeof(header));
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Thumbnails, key, legacy, CacheBudgetConfig{}, root).is_ok());
    PreviewThumbnailFrame frame;
    TEST_ASSERT(LoadHubThumbnailFromDiskCache(path, 320, 180, frame, root, false));
    ThumbnailMigrations().WaitIdle();
    std::string after;
    TEST_ASSERT(ReadCacheFile(CacheClass::Thumbnails, key, after, root, false).is_ok());
    TEST_ASSERT(after == legacy);
    TEST_ASSERT(LoadHubThumbnailFromDiskCache(path, 320, 180, frame, root, true));
    ThumbnailMigrations().WaitIdle();
    TEST_ASSERT(ReadCacheFile(CacheClass::Thumbnails, key, after, root, false).is_ok());
    TEST_ASSERT(after.size() < legacy.size());
}

TEST_CASE(HubThumbnailCache, HubCacheKeyIsStableAcrossExtractorSizes) {
    const std::wstring path = L"C:\\Media\\variable-shell-size.mkv";
    const std::wstring smallKey = MakeHubThumbnailCacheKey(path, 160, 90);
    const std::wstring canonical = MakeHubThumbnailCacheKey(path, 320, 180);
    const std::wstring large = MakeHubThumbnailCacheKey(path, 640, 360);

    TEST_ASSERT(smallKey == canonical);
    TEST_ASSERT(canonical == large);
    TEST_ASSERT(canonical.rfind(L"hub_", 0) == 0);
    TEST_ASSERT(canonical != MakeHubThumbnailCacheKey(L"C:\\Media\\other.mkv", 320, 180));
}

TEST_CASE(HubThumbnailCache, DurableDiskCacheContainmentAndRoundTrip) {
    ScopedTempDir tempDir(L"Aperture_Thumb_Containment_");
    std::wstring root = tempDir.GetPath();

    CacheBudgetConfig config;
    config.thumbnailBudgetBytes = 10 * 1024 * 1024;
    config.totalCeilingBytes = 50 * 1024 * 1024;

    PreviewThumbnailFrame frameIn;
    frameIn.filePath = L"C:\\Videos\\Nature 4K.mkv";
    frameIn.timestampSec = 42.0;
    frameIn.width = 320;
    frameIn.height = 180;
    frameIn.bgraPixels.resize(320 * 180 * 4);
    for (size_t i = 0; i < frameIn.bgraPixels.size(); ++i) {
        frameIn.bgraPixels[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Save to durable cache
    bool saved = SaveHubThumbnailToDiskCache(frameIn, config, root);
    TEST_ASSERT(saved);

    // Verify key containment strictly under GetCacheDir(CacheClass::Thumbnails, root)
    std::wstring key = MakeHubThumbnailCacheKey(frameIn.filePath, frameIn.width, frameIn.height);
    std::wstring fullPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\" + key;
    TEST_ASSERT(FileExists(fullPath));
    TEST_ASSERT(IsCachePathSafe(fullPath, root));

    // Verify loading round-trip
    PreviewThumbnailFrame frameOut;
    bool loaded = LoadHubThumbnailFromDiskCache(frameIn.filePath, 320, 180, frameOut, root);
    TEST_ASSERT(loaded);
    TEST_ASSERT_EQ(frameOut.filePath, frameIn.filePath);
    TEST_ASSERT(std::abs(frameOut.timestampSec - 42.0) < 0.001);
    TEST_ASSERT_EQ(frameOut.width, 320);
    TEST_ASSERT_EQ(frameOut.height, 180);
    TEST_ASSERT_EQ(frameOut.bgraPixels.size(), frameIn.bgraPixels.size());
    TEST_ASSERT_EQ(frameOut.bgraPixels[100], frameIn.bgraPixels[100]);

    // Unsafe path containment check: attempts to read or write with traversal key must be rejected
    PreviewThumbnailFrame unsafeOut;
    TEST_ASSERT(!LoadHubThumbnailFromDiskCache(L"..\\..\\Windows\\System32\\calc.exe", 320, 180, unsafeOut, root));
}

} // namespace Aperture::Test
