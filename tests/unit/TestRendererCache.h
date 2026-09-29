#pragma once
#include "../support/TestFramework.h"
#include "../../src/Renderer.h"

namespace Aperture {
struct RendererCacheTestAccess {
    static void Frame(Renderer& r, uint64_t frame) { r.thumbnailRequestFrame = frame; }
    static bool Contains(Renderer& r, int id) {
        return r.thumbnailCache.count({std::to_wstring(id), 320, 180}) != 0;
    }
    static void Draw(Renderer& r, int id) {
        auto it = r.thumbnailCache.find({std::to_wstring(id), 320, 180});
        if (it != r.thumbnailCache.end()) r.TouchThumbnail(it);
    }
    static bool Admit(Renderer& r, int id, uint64_t bytes = 320 * 180 * 4) {
        if (Contains(r, id)) return true;
        if (!r.TrimThumbnailCache(bytes)) return false;
        Renderer::ThumbnailKey key{std::to_wstring(id), 320, 180};
        r.thumbnailLru.push_front(key);
        Renderer::ThumbnailEntry entry;
        entry.approximateBytes = bytes;
        entry.lruIt = r.thumbnailLru.begin();
        r.thumbnailCache.emplace(key, std::move(entry));
        r.thumbnailCacheBytes += bytes;
        return true;
    }
    static uint64_t Bytes(Renderer& r) { return r.thumbnailCacheBytes; }
    static size_t IndexedCount(Renderer& r) { return r.thumbnailByBitmap.size(); }
    static bool Indexed(Renderer& r, ID2D1Bitmap* bitmap) { return r.FindThumbnailByBitmap(bitmap) != nullptr; }
    static bool Trim(Renderer& r, uint64_t incomingBytes) { return r.TrimThumbnailCache(incomingBytes); }
    static void Complete(Renderer& r, int id, size_t bytes, bool visible = false) {
        Renderer::ThumbnailResult result;
        result.key = {std::to_wstring(id), 320, 180};
        result.generation = r.thumbnailGeneration;
        result.pixels.resize(bytes);
        r.completedThumbnailBytes += bytes;
        r.inFlightThumbnailKeys.insert(result.key);
        if (visible) r.thumbnailDemandFrames[result.key] = r.thumbnailRequestFrame;
        r.completedThumbnailResults.push_back(std::move(result));
    }
    static std::vector<int> TakeBatch(Renderer& r) {
        auto batch = r.TakeCompletedThumbnailBatch();
        std::vector<int> ids;
        for (const auto& result : batch) ids.push_back(std::stoi(result.key.path));
        return ids;
    }
    static uint64_t CompletedBytes(Renderer& r) { return r.completedThumbnailBytes; }
    static bool InFlight(Renderer& r, int id) {
        return r.inFlightThumbnailKeys.count({std::to_wstring(id), 320, 180}) != 0;
    }
    static void Clear(Renderer& r) { r.ClearThumbnailCacheInternal(); }
};
}

namespace Aperture::Test {
TEST_CASE(RendererCache, UploadBatchesPrioritizeViewportAndRetainDeferredWork) {
    Renderer r;
    RendererCacheTestAccess::Frame(r, 10);
    constexpr size_t bytes = 320 * 180 * 4;
    for (int i = 0; i < 10; ++i) RendererCacheTestAccess::Complete(r, i, bytes, i >= 8);
    // BeginDraw advances the frame; previous-frame demand is still visible.
    RendererCacheTestAccess::Frame(r, 11);
    auto first = RendererCacheTestAccess::TakeBatch(r);
    TEST_ASSERT(first == std::vector<int>({8, 9, 0, 1}));
    TEST_ASSERT(r.HasDeferredThumbnailUploads());
    TEST_ASSERT_EQ(RendererCacheTestAccess::CompletedBytes(r), uint64_t(6 * bytes));
    TEST_ASSERT(!RendererCacheTestAccess::InFlight(r, 8));
    TEST_ASSERT(RendererCacheTestAccess::InFlight(r, 2));
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r) == std::vector<int>({2, 3, 4, 5}));
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r) == std::vector<int>({6, 7}));
    TEST_ASSERT(!r.HasDeferredThumbnailUploads());
    TEST_ASSERT_EQ(RendererCacheTestAccess::CompletedBytes(r), uint64_t(0));
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r).empty());
}

TEST_CASE(RendererCache, UploadByteBudgetMakesProgressAndClearCancelsRemainder) {
    Renderer r;
    constexpr size_t mib = 1024 * 1024;
    RendererCacheTestAccess::Complete(r, 0, 3 * mib);
    RendererCacheTestAccess::Complete(r, 1, mib);
    RendererCacheTestAccess::Complete(r, 2, mib);
    RendererCacheTestAccess::Complete(r, 3, mib);
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r) == std::vector<int>({0}));
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r) == std::vector<int>({1, 2}));
    TEST_ASSERT(r.HasDeferredThumbnailUploads());
    TEST_ASSERT_EQ(RendererCacheTestAccess::CompletedBytes(r), uint64_t(mib));
    RendererCacheTestAccess::Clear(r);
    TEST_ASSERT(!r.HasDeferredThumbnailUploads());
    TEST_ASSERT(!RendererCacheTestAccess::InFlight(r, 3));
    TEST_ASSERT_EQ(RendererCacheTestAccess::CompletedBytes(r), uint64_t(0));
    TEST_ASSERT(RendererCacheTestAccess::TakeBatch(r).empty());
}

TEST_CASE(RendererCache, VisibleImagesSurviveRepeatedCompletionPressure) {
    Renderer r;
    RendererCacheTestAccess::Frame(r, 1);
    for (int i = 0; i < 20; ++i) {
        TEST_ASSERT(RendererCacheTestAccess::Admit(r, i));
        RendererCacheTestAccess::Draw(r, i);
    }
    // BeginDraw advances the frame BEFORE admitting background completions.
    for (uint64_t frame = 2; frame < 52; ++frame) {
        RendererCacheTestAccess::Frame(r, frame);
        for (int i = 20; i < 420; ++i) RendererCacheTestAccess::Admit(r, i);
        for (int i = 0; i < 20; ++i) {
            TEST_ASSERT(RendererCacheTestAccess::Contains(r, i));
            RendererCacheTestAccess::Draw(r, i);
        }
        TEST_ASSERT(RendererCacheTestAccess::Bytes(r) <= 64ull * 1024 * 1024);
    }
}

TEST_CASE(RendererCache, FullVisibleCacheRejectsBackgroundUntilViewportExpires) {
    Renderer r;
    RendererCacheTestAccess::Frame(r, 1);
    TEST_ASSERT(RendererCacheTestAccess::Admit(r, 1, 64ull * 1024 * 1024));
    RendererCacheTestAccess::Draw(r, 1);
    RendererCacheTestAccess::Frame(r, 2);
    TEST_ASSERT(!RendererCacheTestAccess::Admit(r, 2));
    TEST_ASSERT(RendererCacheTestAccess::Contains(r, 1));
    RendererCacheTestAccess::Frame(r, 3);
    TEST_ASSERT(RendererCacheTestAccess::Admit(r, 2));
    TEST_ASSERT(!RendererCacheTestAccess::Contains(r, 1));
}

TEST_CASE(RendererCache, BitmapIndexTracksInjectionEvictionAndClear) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Thumbnail index test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer r;
    TEST_ASSERT(r.Initialize(window.handle));
    r.BeginDraw(); // Device creation is deferred until the first frame.
    TEST_ASSERT(r.GetD2DContext() != nullptr);
    std::vector<uint8_t> pixels(320 * 180 * 4, 255);
    TEST_ASSERT(r.InjectThumbnail(L"index-a", 320, 180, pixels));
    TEST_ASSERT(r.InjectThumbnail(L"index-b", 320, 180, pixels));
    auto a = r.GetOrCreateVideoThumbnail(L"index-a");
    auto b = r.GetOrCreateVideoThumbnail(L"index-b");
    TEST_ASSERT(a && b);
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(r), size_t(2));
    TEST_ASSERT(RendererCacheTestAccess::Indexed(r, a.Get()));
    TEST_ASSERT(RendererCacheTestAccess::Indexed(r, b.Get()));
    r.DrawBitmapRounded(D2D1::RectF(0, 0, 160, 90), 8, a.Get());
    r.DrawThumbnailCroppedBottom(D2D1::RectF(0, 100, 160, 140),
        D2D1::RectF(0, 100, 160, 190), 8, b.Get());
    TEST_ASSERT(r.EndDraw(false));
    RendererCacheTestAccess::Frame(r, 3);
    TEST_ASSERT(RendererCacheTestAccess::Trim(r, 64ull * 1024 * 1024));
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(r), size_t(0));
    TEST_ASSERT(!RendererCacheTestAccess::Indexed(r, a.Get()));
    TEST_ASSERT(!RendererCacheTestAccess::Indexed(r, b.Get()));
    TEST_ASSERT(r.InjectThumbnail(L"index-c", 320, 180, pixels));
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(r), size_t(1));
    RendererCacheTestAccess::Clear(r);
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(r), size_t(0));
}
}
