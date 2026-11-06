#pragma once
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
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
    static size_t LayoutCount(const Renderer& r) { return r.layoutCache.size(); }
    static size_t LayoutLruCount(const Renderer& r) { return r.layoutLru.size(); }
    static uint64_t LayoutBytes(const Renderer& r) { return r.layoutCacheBytes; }
    static size_t SizedFormatCount(const Renderer& r) { return r.sizedFormatCache.size(); }
    static bool HasWic(const Renderer& r) { return r.wicFactory != nullptr; }
    static IDWriteTextFormat* BaseFormat(Renderer& r) { return r.GetTextFormat(FontType::Regular); }
    static uint32_t PngLastPixel(Renderer& r, const std::wstring& path, UINT& width, UINT& height) {
        auto* wic = r.EnsureWicFactory();
        TEST_ASSERT(wic != nullptr);
        ComPtr<IWICBitmapDecoder> decoder;
        TEST_ASSERT(SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, &decoder)));
        ComPtr<IWICBitmapFrameDecode> frame;
        TEST_ASSERT(SUCCEEDED(decoder->GetFrame(0, &frame)));
        TEST_ASSERT(SUCCEEDED(frame->GetSize(&width, &height)));
        ComPtr<IWICFormatConverter> converter;
        TEST_ASSERT(SUCCEEDED(wic->CreateFormatConverter(&converter)));
        TEST_ASSERT(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)));
        WICRect corner{int(width - 1), int(height - 1), 1, 1};
        uint32_t pixel = 0;
        TEST_ASSERT(SUCCEEDED(converter->CopyPixels(&corner, sizeof(pixel), sizeof(pixel), reinterpret_cast<BYTE*>(&pixel))));
        return pixel;
    }
    // This tests the broadcast contract directly. It does not simulate Shell
    // decoding or claim to be a real blocked-producer integration test.
    static bool HiddenTrimBroadcastsToBothWaiters(Renderer& r) {
        std::condition_variable observed;
        size_t ready = 0, released = 0;
        const uint64_t generation = r.thumbnailGeneration;
        auto wait = [&]() {
            std::unique_lock<std::mutex> lock(r.thumbnailMutex);
            ++ready;
            observed.notify_all();
            r.thumbnailCv.wait(lock, [&]() { return generation != r.thumbnailGeneration; });
            ++released;
            observed.notify_all();
        };
        std::thread first(wait), second(wait);
        std::unique_lock<std::mutex> lock(r.thumbnailMutex);
        const bool registered = observed.wait_for(lock, std::chrono::seconds(2), [&]() { return ready == 2; });
        lock.unlock();
        r.TrimHiddenLibraryCaches();
        lock.lock();
        const bool broadcast = registered && observed.wait_for(lock, std::chrono::seconds(1), [&]() { return released == 2; });
        // Always release and join both observers, including on regression.
        ++r.thumbnailGeneration;
        lock.unlock();
        r.thumbnailCv.notify_all();
        first.join();
        second.join();
        return broadcast;
    }
    static void Clear(Renderer& r) { r.ClearThumbnailCacheInternal(); }
    static DXGI_SWAP_CHAIN_DESC1 Buffers(Renderer& r) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        TEST_ASSERT(r.swapChain && SUCCEEDED(r.swapChain->GetDesc1(&description)));
        return description;
    }
    static uint32_t LastPixel(Renderer& r) {
        ComPtr<ID3D11Texture2D> buffer, staging;
        TEST_ASSERT(SUCCEEDED(r.swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))));
        if (!buffer) return 0;
        D3D11_TEXTURE2D_DESC description{};
        buffer->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        TEST_ASSERT(SUCCEEDED(r.d3dDevice->CreateTexture2D(&description, nullptr, &staging)));
        if (!staging) return 0;
        r.d3dContext->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        TEST_ASSERT(SUCCEEDED(r.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        if (!mapped.pData) return 0;
        const auto* lastRow = static_cast<const uint8_t*>(mapped.pData) + (description.Height - 1) * mapped.RowPitch;
        const uint32_t pixel = reinterpret_cast<const uint32_t*>(lastRow)[description.Width - 1];
        r.d3dContext->Unmap(staging.Get(), 0);
        return pixel;
    }
};
}

namespace Aperture::Test {
static void step_RendererCache_UploadBatchesPrioritizeViewportAndRetainDeferredWork() {
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
static void step_RendererCache_UploadByteBudgetMakesProgressAndClearCancelsRemainder() {
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
static void step_RendererCache_VisibleImagesSurviveRepeatedCompletionPressure() {
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
static void step_RendererCache_FullVisibleCacheRejectsBackgroundUntilViewportExpires() {
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
static void step_RendererCache_BitmapIndexTracksInjectionEvictionAndClear() {
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

TEST_CASE(RendererCache, ViewportUploadBudgetAndBitmapEviction) {
    step_RendererCache_UploadBatchesPrioritizeViewportAndRetainDeferredWork();
    step_RendererCache_UploadByteBudgetMakesProgressAndClearCancelsRemainder();
    step_RendererCache_VisibleImagesSurviveRepeatedCompletionPressure();
    step_RendererCache_FullVisibleCacheRejectsBackgroundUntilViewportExpires();
    step_RendererCache_BitmapIndexTracksInjectionEvictionAndClear();
}

TEST_CASE(RendererCache, HiddenLibraryRetainsRecentBitmapsAndDropsQueuedPixels) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Hidden cache test", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer renderer; TEST_ASSERT(renderer.Initialize(window.handle));
    renderer.BeginDraw(); TEST_ASSERT(renderer.GetD2DContext() != nullptr);
    std::vector<uint8_t> pixels(320 * 180 * 4, 255);
    for (int i = 0; i < 8; ++i)
        TEST_ASSERT(renderer.InjectThumbnail(std::to_wstring(i), 320, 180, pixels));
    // Touch an old bitmap to make it part of the set retained for return.
    auto recent = renderer.GetOrCreateVideoThumbnail(L"0"); TEST_ASSERT(recent != nullptr);
    TEST_ASSERT(renderer.EndDraw(false));
    RendererCacheTestAccess::Complete(renderer, 100, 1024 * 1024);
    renderer.TrimHiddenLibraryCaches(2 * pixels.size());
    TEST_ASSERT_EQ(RendererCacheTestAccess::Bytes(renderer), 2 * pixels.size());
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(renderer), size_t(2));
    TEST_ASSERT(RendererCacheTestAccess::Indexed(renderer, recent.Get()));
    TEST_ASSERT_EQ(RendererCacheTestAccess::CompletedBytes(renderer), uint64_t(0));
    TEST_ASSERT(!RendererCacheTestAccess::InFlight(renderer, 100));
    // Returning to the hub can admit images again without resetting the device.
    TEST_ASSERT(renderer.InjectThumbnail(L"1", 320, 180, pixels));
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(renderer), size_t(3));
    renderer.TrimHiddenLibraryCaches(0);
    TEST_ASSERT_EQ(RendererCacheTestAccess::Bytes(renderer), uint64_t(0));
    TEST_ASSERT_EQ(RendererCacheTestAccess::IndexedCount(renderer), size_t(0));
}

TEST_CASE(RendererCache, HiddenTrimBroadcastsGenerationInvalidation) {
    Renderer renderer;
    TEST_ASSERT(RendererCacheTestAccess::HiddenTrimBroadcastsToBothWaiters(renderer));
}

TEST_CASE(RendererCache, HiddenTextAndCaptureCachesRebuildWithoutReplacingSharedResources) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Hidden cache reconstruction", WS_POPUP,
            0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    ScopedTempDir directory(L"Aperture_Hidden_Caches_");
    Renderer renderer;
    TEST_ASSERT(renderer.Initialize(window.handle, "", false));
    auto* d2dFactory = renderer.GetD2DFactory();
    auto* writeFactory = renderer.GetDWriteFactory();
    auto* collection = renderer.GetCustomFontCollection();
    auto* baseFormat = RendererCacheTestAccess::BaseFormat(renderer);
    const std::wstring text = L"Hub title with shaped text: \x03A9 \x0627\x0644\x0639\x0631\x0628\x064A\x0629";
    const auto normal = renderer.MeasureString(text, FontType::Regular);
    const auto sized = renderer.MeasureStringSized(text, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    TEST_ASSERT(normal.width > 0 && sized.width > 0);
    renderer.BeginDraw();
    renderer.Clear(D2D1::ColorF(D2D1::ColorF::Blue));
    renderer.DrawString(text, D2D1::RectF(4, 4, 200, 60), D2D1::ColorF(D2D1::ColorF::White));
    TEST_ASSERT(renderer.EndDraw(false));
    auto* context = renderer.GetD2DContext();
    const auto before = directory.SubPath(L"before.png");
    TEST_ASSERT(renderer.CaptureBackBufferToPng(before));
    UINT width = 0, height = 0;
    TEST_ASSERT_EQ(RendererCacheTestAccess::PngLastPixel(renderer, before, width, height), 0xff0000ffu);
    TEST_ASSERT_EQ(width, 320u); TEST_ASSERT_EQ(height, 240u);
    TEST_ASSERT(RendererCacheTestAccess::LayoutCount(renderer) > 0);
    TEST_ASSERT(RendererCacheTestAccess::SizedFormatCount(renderer) > 0);
    TEST_ASSERT(RendererCacheTestAccess::HasWic(renderer));

    renderer.TrimHiddenLibraryCaches();
    TEST_ASSERT_EQ(RendererCacheTestAccess::LayoutCount(renderer), size_t(0));
    TEST_ASSERT_EQ(RendererCacheTestAccess::LayoutLruCount(renderer), size_t(0));
    TEST_ASSERT_EQ(RendererCacheTestAccess::LayoutBytes(renderer), uint64_t(0));
    TEST_ASSERT_EQ(RendererCacheTestAccess::SizedFormatCount(renderer), size_t(0));
    TEST_ASSERT(!RendererCacheTestAccess::HasWic(renderer));
    TEST_ASSERT(renderer.GetD2DFactory() == d2dFactory && renderer.GetDWriteFactory() == writeFactory);
    TEST_ASSERT(renderer.GetCustomFontCollection() == collection);
    TEST_ASSERT(RendererCacheTestAccess::BaseFormat(renderer) == baseFormat);
    TEST_ASSERT(renderer.GetD2DContext() == context);
    const auto restoredNormal = renderer.MeasureString(text, FontType::Regular);
    const auto restoredSized = renderer.MeasureStringSized(text, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    TEST_ASSERT_EQ(restoredNormal.width, normal.width); TEST_ASSERT_EQ(restoredNormal.height, normal.height);
    TEST_ASSERT_EQ(restoredSized.width, sized.width); TEST_ASSERT_EQ(restoredSized.height, sized.height);
    TEST_ASSERT_EQ(RendererCacheTestAccess::Buffers(renderer).Width, 1u);
    renderer.BeginDraw();
    renderer.Clear(D2D1::ColorF(D2D1::ColorF::Red));
    renderer.DrawString(text, D2D1::RectF(4, 4, 200, 60), D2D1::ColorF(D2D1::ColorF::White));
    TEST_ASSERT(renderer.EndDraw(false));
    const auto after = directory.SubPath(L"after.png");
    TEST_ASSERT(renderer.CaptureBackBufferToPng(after));
    TEST_ASSERT_EQ(RendererCacheTestAccess::PngLastPixel(renderer, after, width, height), 0xffff0000u);
    TEST_ASSERT_EQ(width, 320u); TEST_ASSERT_EQ(height, 240u);
    TEST_ASSERT(renderer.GetD2DContext() == context);
}

TEST_CASE(RendererCache, HiddenBackbuffersRestoreWithoutReplacingDeviceOrThumbnails) {
    struct Window {
        HWND handle = CreateWindowExW(0, L"STATIC", L"Hidden backbuffer test", WS_POPUP,
            0, 0, 1920, 1080, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    TEST_ASSERT(window.handle != nullptr);
    Renderer renderer;
    TEST_ASSERT(renderer.Initialize(window.handle, "", false));
    renderer.BeginDraw();
    renderer.Clear(D2D1::ColorF(D2D1::ColorF::Blue));
    TEST_ASSERT(renderer.EndDraw(false));
    auto* context = renderer.GetD2DContext();
    const HANDLE latency = renderer.GetFrameLatencyWaitableObject();
    std::vector<uint8_t> pixels(320 * 180 * 4, 255);
    TEST_ASSERT(renderer.InjectThumbnail(L"retained", 320, 180, pixels));
    auto retained = renderer.GetOrCreateVideoThumbnail(L"retained");
    const auto original = RendererCacheTestAccess::Buffers(renderer);
    TEST_ASSERT_EQ(original.Width, 1920u);
    TEST_ASSERT_EQ(original.Height, 1080u);
    TEST_ASSERT_EQ(original.BufferCount, 3u);

    for (UINT cycle = 0; cycle < 3; ++cycle) {
        renderer.TrimHiddenLibraryCaches();
        const auto trimmed = RendererCacheTestAccess::Buffers(renderer);
        TEST_ASSERT_EQ(trimmed.Width, 1u);
        TEST_ASSERT_EQ(trimmed.Height, 1u);
        TEST_ASSERT(renderer.GetD2DContext() == context);
        TEST_ASSERT(renderer.GetFrameLatencyWaitableObject() == latency);
        TEST_ASSERT(RendererCacheTestAccess::Indexed(renderer, retained.Get()));
        // Thumbnail/preview intent must not restore full-size buffers in playback.
        TEST_ASSERT(renderer.EnsureGraphicsContext());
        TEST_ASSERT_EQ(RendererCacheTestAccess::Buffers(renderer).Width, 1u);

        const UINT width = cycle == 0 ? 1920u : 640u + cycle * 160u;
        const UINT height = cycle == 0 ? 1080u : 480u + cycle * 90u;
        TEST_ASSERT(SetWindowPos(window.handle, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE));
        renderer.Resize(width, height);
        renderer.SetDpi(144.0f);
        TEST_ASSERT_EQ(RendererCacheTestAccess::Buffers(renderer).Width, 1u);
        renderer.BeginDraw();
        renderer.Clear(D2D1::ColorF(D2D1::ColorF::Red));
        renderer.DrawBitmapRounded(D2D1::RectF(10, 10, 170, 100), 8, retained.Get());
        TEST_ASSERT(renderer.EndDraw(false));
        const auto restored = RendererCacheTestAccess::Buffers(renderer);
        TEST_ASSERT_EQ(restored.Width, width);
        TEST_ASSERT_EQ(restored.Height, height);
        TEST_ASSERT_EQ(restored.BufferCount, original.BufferCount);
        TEST_ASSERT_EQ(restored.Format, original.Format);
        TEST_ASSERT_EQ(RendererCacheTestAccess::LastPixel(renderer), 0xffff0000u);
        TEST_ASSERT(renderer.GetD2DContext() == context);
        TEST_ASSERT(RendererCacheTestAccess::Indexed(renderer, retained.Get()));
    }
}

}
