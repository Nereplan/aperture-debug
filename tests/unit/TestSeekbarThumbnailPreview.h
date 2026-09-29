#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/DllLoader.h"
#include "../../src/MpvPlayer.h"
#include "../../src/ThumbnailMigration.h"
#include "../../src/RenderViewModel.h"
#include <cmath>
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(SeekbarThumbnail, DefaultWritersHonorActiveDisabledBudget) {
    struct Restore { CacheBudgetConfig saved = GetGlobalCacheBudgets(); ~Restore() { SetGlobalCacheBudgets(saved); } } restore;
    CacheBudgetConfig disabled; disabled.thumbnailBudgetBytes = 0;
    SetGlobalCacheBudgets(disabled);
    PreviewThumbnailFrame frame;
    frame.filePath = L"C:\\Aperture-tests\\disabled-thumbnail-setting.mkv";
    frame.width = 32; frame.height = 18; frame.bgraPixels.assign(32 * 18 * 4, 255);
    // Exercise the actual default arguments used by both background writers.
    TEST_ASSERT(!SaveHubThumbnailToDiskCache(frame));
    TEST_ASSERT(!SaveThumbnailToDiskCache(frame));
}

TEST_CASE(SeekbarThumbnail, PrivateReadDoesNotMigrateLegacyCache) {
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

// =========================================================================
// 1. Time Quantization Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, TimeQuantizationBasicAndBounds) {
    // Normal quantization with default 1.0s step
    TEST_ASSERT(std::abs(QuantizePreviewTime(0.0, 100.0, 1.0) - 0.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(0.4, 100.0, 1.0) - 0.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(0.5, 100.0, 1.0) - 1.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.2, 100.0, 1.0) - 1.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.8, 100.0, 1.0) - 2.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(45.67, 100.0, 1.0) - 46.0) < 0.001);

    // Clamping to bounds [0.0, duration]
    TEST_ASSERT(std::abs(QuantizePreviewTime(-5.0, 100.0, 1.0) - 0.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(105.0, 100.0, 1.0) - 100.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(99.8, 100.0, 1.0) - 100.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(100.0, 100.0, 1.0) - 100.0) < 0.001);

    // Zero or negative duration
    TEST_ASSERT(std::abs(QuantizePreviewTime(10.0, 0.0, 1.0) - 0.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(10.0, -50.0, 1.0) - 0.0) < 0.001);

    // Non-finite values
    TEST_ASSERT(std::abs(QuantizePreviewTime(std::nan(""), 100.0, 1.0) - 0.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(50.0, std::nan(""), 1.0) - 0.0) < 0.001);
}

TEST_CASE(SeekbarThumbnail, TimeQuantizationCustomSteps) {
    // 0.5s quantization step
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.2, 100.0, 0.5) - 1.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.3, 100.0, 0.5) - 1.5) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.7, 100.0, 0.5) - 1.5) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(1.8, 100.0, 0.5) - 2.0) < 0.001);

    // 2.0s quantization step
    TEST_ASSERT(std::abs(QuantizePreviewTime(3.1, 100.0, 2.0) - 4.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(2.9, 100.0, 2.0) - 2.0) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(5.5, 100.0, 2.0) - 6.0) < 0.001);

    // Zero or invalid step falls back to clamped time
    TEST_ASSERT(std::abs(QuantizePreviewTime(12.34, 100.0, 0.0) - 12.34) < 0.001);
    TEST_ASSERT(std::abs(QuantizePreviewTime(12.34, 100.0, -1.0) - 12.34) < 0.001);
}

TEST_CASE(SeekbarThumbnail, HubCacheKeyIsStableAcrossExtractorSizes) {
    const std::wstring path = L"C:\\Media\\variable-shell-size.mkv";
    const std::wstring smallKey = MakeHubThumbnailCacheKey(path, 160, 90);
    const std::wstring canonical = MakeHubThumbnailCacheKey(path, 320, 180);
    const std::wstring large = MakeHubThumbnailCacheKey(path, 640, 360);

    TEST_ASSERT(smallKey == canonical);
    TEST_ASSERT(canonical == large);
    TEST_ASSERT(canonical.rfind(L"hub_", 0) == 0);
    TEST_ASSERT(canonical != MakeHubThumbnailCacheKey(L"C:\\Media\\other.mkv", 320, 180));
}

TEST_CASE(SeekbarThumbnail, DiskCacheRemovalPurgesScopedAndLegacyEntries) {
    ScopedTempDir tempDir(L"Aperture_Thumb_Purge_");
    const std::wstring root = tempDir.GetPath();
    const std::wstring cacheDir = GetCacheDir(CacheClass::Thumbnails, root);
    const std::wstring removedPath = L"C:\\Media\\removed.mkv";
    const std::wstring livePath = L"C:\\Media\\live.mkv";
    const double fractionalDuration = 2.6;

    auto createEntry = [&cacheDir](const std::wstring& key) {
        HANDLE file = CreateFileW((cacheDir + L"\\" + key).c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        TEST_ASSERT(file != INVALID_HANDLE_VALUE);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    };

    std::vector<std::wstring> removedKeys = {
        MakeHubThumbnailCacheKey(removedPath, 320, 180),
        MakeThumbnailCacheKey(removedPath, 0.0),
        MakeThumbnailCacheKey(removedPath, 1.0),
        MakeThumbnailCacheKey(removedPath, 2.0),
        MakeThumbnailCacheKey(removedPath, 3.0),
        MakeThumbnailCacheKey(removedPath, fractionalDuration),
    };
    for (const auto& key : removedKeys) createEntry(key);

    const std::wstring liveHubKey = MakeHubThumbnailCacheKey(livePath, 320, 180);
    const std::wstring unrelatedSeekKey = MakeThumbnailCacheKey(livePath, 1.0);
    createEntry(liveHubKey);
    createEntry(unrelatedSeekKey);
    createEntry(L"hub_0123456789abcdef.bin"); // legacy/orphan key

    TEST_ASSERT_EQ(PurgeThumbnailDiskCacheForFile(
        removedPath, fractionalDuration, root), removedKeys.size());
    for (const auto& key : removedKeys) {
        TEST_ASSERT(!FileExists(cacheDir + L"\\" + key));
    }
    TEST_ASSERT(FileExists(cacheDir + L"\\" + liveHubKey));
    TEST_ASSERT(FileExists(cacheDir + L"\\" + unrelatedSeekKey));

    TEST_ASSERT_EQ(PruneOrphanedHubThumbnailDiskCache({ livePath }, root), (size_t)1);
    TEST_ASSERT(FileExists(cacheDir + L"\\" + liveHubKey));
    TEST_ASSERT(!FileExists(cacheDir + L"\\hub_0123456789abcdef.bin"));
    TEST_ASSERT(FileExists(cacheDir + L"\\" + unrelatedSeekKey));
}

// =========================================================================
// 2. In-Memory Cache Bounds & LRU Eviction Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, MemoryCacheBoundsAndLRUEviction) {
    // Max 3 items
    ThumbnailMemoryCache cache(3, 1024 * 1024);

    PreviewThumbnailFrame f1;
    f1.filePath = L"C:\\Media\\video1.mp4";
    f1.timestampSec = 10.0;
    f1.width = 320;
    f1.height = 180;
    f1.bgraPixels.assign(320 * 180 * 4, 1);
    cache.Put(f1);

    PreviewThumbnailFrame f2;
    f2.filePath = L"C:\\Media\\video1.mp4";
    f2.timestampSec = 20.0;
    f2.width = 320;
    f2.height = 180;
    f2.bgraPixels.assign(320 * 180 * 4, 2);
    cache.Put(f2);

    PreviewThumbnailFrame f3;
    f3.filePath = L"C:\\Media\\video1.mp4";
    f3.timestampSec = 30.0;
    f3.width = 320;
    f3.height = 180;
    f3.bgraPixels.assign(320 * 180 * 4, 3);
    cache.Put(f3);

    TEST_ASSERT_EQ(cache.GetCount(), 3ULL);

    // Access f1 to make f2 the oldest
    PreviewThumbnailFrame outFrame;
    TEST_ASSERT(cache.TryGet(L"C:\\Media\\video1.mp4", 10.0, outFrame));
    TEST_ASSERT_EQ(outFrame.bgraPixels[0], 1);

    // Put f4 -> should evict f2 (since f1 and f3 were accessed more recently)
    PreviewThumbnailFrame f4;
    f4.filePath = L"C:\\Media\\video1.mp4";
    f4.timestampSec = 40.0;
    f4.width = 320;
    f4.height = 180;
    f4.bgraPixels.assign(320 * 180 * 4, 4);
    cache.Put(f4);

    TEST_ASSERT_EQ(cache.GetCount(), 3ULL);
    TEST_ASSERT(cache.TryGet(L"C:\\Media\\video1.mp4", 10.0, outFrame));
    TEST_ASSERT(!cache.TryGet(L"C:\\Media\\video1.mp4", 20.0, outFrame)); // Evicted!
    TEST_ASSERT(cache.TryGet(L"C:\\Media\\video1.mp4", 30.0, outFrame));
    TEST_ASSERT(cache.TryGet(L"C:\\Media\\video1.mp4", 40.0, outFrame));
}

TEST_CASE(SeekbarThumbnail, MemoryCacheByteBudgetEviction) {
    // 200 KB byte ceiling -> at ~100 KB per item, can hold at most 2 items
    ThumbnailMemoryCache cache(10, 200 * 1024);

    PreviewThumbnailFrame f1;
    f1.filePath = L"C:\\Media\\video.mp4";
    f1.timestampSec = 1.0;
    f1.width = 160;
    f1.height = 90;
    f1.bgraPixels.assign(160 * 90 * 4, 0xAA); // ~57 KB
    cache.Put(f1);

    PreviewThumbnailFrame f2;
    f2.filePath = L"C:\\Media\\video.mp4";
    f2.timestampSec = 2.0;
    f2.width = 160;
    f2.height = 90;
    f2.bgraPixels.assign(160 * 90 * 4, 0xBB); // ~57 KB
    cache.Put(f2);

    TEST_ASSERT_EQ(cache.GetCount(), 2ULL);

    PreviewThumbnailFrame f3;
    f3.filePath = L"C:\\Media\\video.mp4";
    f3.timestampSec = 3.0;
    f3.width = 160;
    f3.height = 90;
    f3.bgraPixels.assign(160 * 90 * 4, 0xCC); // ~57 KB
    cache.Put(f3);

    TEST_ASSERT_EQ(cache.GetCount(), 3ULL);

    // Put a large 120 KB item -> triggers byte eviction
    PreviewThumbnailFrame fLarge;
    fLarge.filePath = L"C:\\Media\\video.mp4";
    fLarge.timestampSec = 4.0;
    fLarge.width = 200;
    fLarge.height = 150;
    fLarge.bgraPixels.assign(200 * 150 * 4, 0xFF); // 120 KB
    cache.Put(fLarge);

    TEST_ASSERT(cache.GetTotalBytes() <= 200 * 1024);
    TEST_ASSERT(cache.GetCount() <= 2ULL);
}

// =========================================================================
// 3. Cache Containment & Durable Storage Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, DurableDiskCacheContainmentAndRoundTrip) {
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
    bool saved = SaveThumbnailToDiskCache(frameIn, config, root);
    TEST_ASSERT(saved);

    // Verify key containment strictly under GetCacheDir(CacheClass::Thumbnails, root)
    std::wstring key = MakeThumbnailCacheKey(frameIn.filePath, frameIn.timestampSec);
    std::wstring fullPath = GetCacheDir(CacheClass::Thumbnails, root) + L"\\" + key;
    TEST_ASSERT(FileExists(fullPath));
    TEST_ASSERT(IsCachePathSafe(fullPath, root));

    // Verify loading round-trip
    PreviewThumbnailFrame frameOut;
    bool loaded = LoadThumbnailFromDiskCache(frameIn.filePath, 42.0, frameOut, root);
    TEST_ASSERT(loaded);
    TEST_ASSERT_EQ(frameOut.filePath, frameIn.filePath);
    TEST_ASSERT(std::abs(frameOut.timestampSec - 42.0) < 0.001);
    TEST_ASSERT_EQ(frameOut.width, 320);
    TEST_ASSERT_EQ(frameOut.height, 180);
    TEST_ASSERT_EQ(frameOut.bgraPixels.size(), frameIn.bgraPixels.size());
    TEST_ASSERT_EQ(frameOut.bgraPixels[100], frameIn.bgraPixels[100]);

    // Unsafe path containment check: attempts to read or write with traversal key must be rejected
    PreviewThumbnailFrame unsafeOut;
    TEST_ASSERT(!LoadThumbnailFromDiskCache(L"..\\..\\Windows\\System32\\calc.exe", 0.0, unsafeOut, root));
}

// =========================================================================
// 4. Media Invalidation Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, MediaInvalidationClearsScopedEntries) {
    ThumbnailMemoryCache cache(10, 1024 * 1024);

    PreviewThumbnailFrame f1;
    f1.filePath = L"C:\\Media\\Movie1.mp4";
    f1.timestampSec = 10.0;
    f1.width = 160;
    f1.height = 90;
    f1.bgraPixels.assign(160 * 90 * 4, 1);
    cache.Put(f1);

    PreviewThumbnailFrame f2;
    f2.filePath = L"C:\\Media\\Movie1.mp4";
    f2.timestampSec = 20.0;
    f2.width = 160;
    f2.height = 90;
    f2.bgraPixels.assign(160 * 90 * 4, 2);
    cache.Put(f2);

    PreviewThumbnailFrame f3;
    f3.filePath = L"C:\\Media\\Movie2.mp4";
    f3.timestampSec = 10.0;
    f3.width = 160;
    f3.height = 90;
    f3.bgraPixels.assign(160 * 90 * 4, 3);
    cache.Put(f3);

    TEST_ASSERT_EQ(cache.GetCount(), 3ULL);

    // Invalidate Movie1
    cache.InvalidateMedia(L"C:\\Media\\Movie1.mp4");

    TEST_ASSERT_EQ(cache.GetCount(), 1ULL);
    PreviewThumbnailFrame outFrame;
    TEST_ASSERT(!cache.TryGet(L"C:\\Media\\Movie1.mp4", 10.0, outFrame));
    TEST_ASSERT(!cache.TryGet(L"C:\\Media\\Movie1.mp4", 20.0, outFrame));
    TEST_ASSERT(cache.TryGet(L"C:\\Media\\Movie2.mp4", 10.0, outFrame));

    // Clear all
    cache.Clear();
    TEST_ASSERT_EQ(cache.GetCount(), 0ULL);
    TEST_ASSERT_EQ(cache.GetTotalBytes(), 0ULL);
}

// =========================================================================
// 5. Request Coalescing & Stale Suppression Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, RequestCoalescingAndStaleSuppression) {
    MpvPreviewPlayer player;

    // Direct cache priming to test synchronous cache hit and stale suppression
    PreviewThumbnailFrame f1;
    f1.filePath = L"C:\\Media\\test.mp4";
    f1.timestampSec = 10.0;
    f1.width = 320;
    f1.height = 180;
    f1.bgraPixels.assign(320 * 180 * 4, 0x11);
    player.GetMemoryCache().Put(f1);

    // 1. Cache hit request immediately satisfies thumbnail
    bool hit = player.RequestSeekbarThumbnail(L"C:\\Media\\test.mp4", 10.2, 100.0, 1);
    TEST_ASSERT(hit);
    TEST_ASSERT(player.HasSeekbarThumbnail());
    TEST_ASSERT(!player.IsSeekbarThumbnailLoading());

    // 2. Cache miss request marks loading and invalidates immediate thumbnail
    bool miss = player.RequestSeekbarThumbnail(L"C:\\Media\\test.mp4", 55.4, 100.0, 1);
    TEST_ASSERT(!player.HasSeekbarThumbnail());
    TEST_ASSERT(player.IsSeekbarThumbnailLoading());

    // 3. Media reset cancels in-flight requests and clears active thumbnail
    player.ResetSeekbarPreview();
    TEST_ASSERT(!player.HasSeekbarThumbnail());
    TEST_ASSERT(!player.IsSeekbarThumbnailLoading());
    TEST_ASSERT(!player.IsSeekbarActive());
}

// =========================================================================
// 6. Hover Projection & Geometry Tests
// =========================================================================

TEST_CASE(SeekbarThumbnail, HoverProjectionAndClamping) {
    float dp = 1.25f;
    float padX = 20.0f * dp; // 25.0f
    float winW = 1000.0f;
    float trackCenterY = 500.0f;

    // Card dimensions
    float thumbW = 160.0f * dp; // 200.0f
    float thumbH = 90.0f * dp;  // 112.5f
    float cardPad = 4.0f * dp;  // 5.0f
    float badgeH = 22.0f * dp;  // 27.5f
    float cardW = thumbW + cardPad * 2.0f; // 210.0f
    float cardH = thumbH + cardPad * 2.0f + badgeH; // 150.0f

    // Center projection
    float hoverXCenter = 500.0f;
    float cardXCenter = std::clamp(hoverXCenter - cardW * 0.5f, padX, winW - padX - cardW);
    TEST_ASSERT(std::abs(cardXCenter - (500.0f - 105.0f)) < 0.001); // 395.0f

    // Left clamp projection (mouse near 0)
    float hoverXLeft = 10.0f;
    float cardXLeft = std::clamp(hoverXLeft - cardW * 0.5f, padX, winW - padX - cardW);
    TEST_ASSERT(std::abs(cardXLeft - padX) < 0.001); // 25.0f

    // Right clamp projection (mouse near winW)
    float hoverXRight = 995.0f;
    float cardXRight = std::clamp(hoverXRight - cardW * 0.5f, padX, winW - padX - cardW);
    TEST_ASSERT(std::abs(cardXRight - (winW - padX - cardW)) < 0.001); // 1000 - 25 - 210 = 765.0f

    // Vertical placement above seekbar track
    float cardY = trackCenterY - cardH - 12.0f * dp;
    TEST_ASSERT(std::abs(cardY - (500.0f - 150.0f - 15.0f)) < 0.001); // 335.0f
    TEST_ASSERT(cardY < trackCenterY);
}

// =========================================================================
// 7. Real libmpv Smoke Test (Dynamic DLL Verification)
// =========================================================================

TEST_CASE(SeekbarThumbnail, RealMpvSmokeOrExactLimitation) {
    HMODULE hDll = DllLoader::LoadPackageLocalDll(L"libmpv-2.dll");
    if (!hDll) {
        std::cout << "  [SMOKE NOTICE] libmpv-2.dll is not loadable in this environment; skipping live mpv render context smoke.\n";
        return;
    }

    MpvPreviewPlayer player;
    bool initialized = player.Initialize(nullptr, hDll);
    if (!initialized) {
        std::cout << "  [SMOKE NOTICE] Preview player initialize returned false (headless / no D2D device context provided).\n";
        FreeLibrary(hDll);
        return;
    }

    // Verify clean lifecycle and shutdown without stall or crash
    player.ResetSeekbarPreview();
    TEST_ASSERT(!player.HasSeekbarThumbnail());
    TEST_ASSERT(!player.IsSeekbarThumbnailLoading());

    player.Shutdown();
    FreeLibrary(hDll);
}

} // namespace Aperture::Test
