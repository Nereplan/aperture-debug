#include "SvgGeometry.h"
#include "Renderer.h"
#include "Localization.h"
#include "MpvPlayer.h"
#include "MediaProbeIsolation.h"
#include <chrono>
#include <cmath>

namespace Aperture {

namespace {

// The renderer receives physical-pixel coordinates from UIComponents (all dimensions are already multiplied by the active DPI scale).
// Snapping at the primitive boundary removes the half-pixel fuzz that otherwise accumulates on
// 1px card borders and text origins, while leaving transforms/animations free
// to remain subpixel smooth.

inline uint32_t QuantizeColor(float value) { return (uint32_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f); }

// Shell-miss retry backoff: 8s, 24s, 60s, then permanent for the generation.
inline uint64_t ThumbRetryDelayMs(int attempts) {
    if (attempts <= 0) return 8000;
    if (attempts == 1) return 24000;
    if (attempts == 2) return 60000;
    return UINT64_MAX;
}

}
float Renderer::SnapPixel(float value) const { return primitiveSnapping ? std::round(value) : value; }

D2D1_RECT_F Renderer::SnapRect(const D2D1_RECT_F& rect) const {
    if (!primitiveSnapping) return rect;
    const float left = std::round(rect.left);
    const float top = std::round(rect.top);
    const float w = std::round(rect.right - rect.left);
    const float h = std::round(rect.bottom - rect.top);
    return D2D1::RectF(left, top, left + w, top + h);
}

static bool CreateD3DDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        creationFlags, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (SUCCEEDED(hr)) return true;
    device.Reset();
    context.Reset();
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        creationFlags, featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &device, &featureLevel, &context);
    return SUCCEEDED(hr);
}

struct PrewarmDeviceState {
    std::thread worker;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    bool failed = false;
    std::atomic<bool> started{ false };
    std::atomic<bool> done{ true };
    ~PrewarmDeviceState() { Shutdown(); }
    void Shutdown() {
        if (worker.joinable()) { worker.join(); }
        device.Reset();
        context.Reset();
        failed = false;
        started.store(false);
    }
};

static PrewarmDeviceState s_prewarmState;
// App is constructed before the GDI bootstrap window is shown. Keep construction
// free of graphics initialization; wWinMain starts the D3D prewarm explicitly.
Renderer::Renderer() = default;

Renderer::~Renderer() { Shutdown(); }

void Renderer::Shutdown() {
    deferredThumbnailUploads = false;
    // The async thumbnail worker must be signaled and joined before releasing state.
    CancelThumbnailWork();
    for (auto& worker : thumbnailWorkers) { if (worker.joinable()) worker.join(); }
    // The async device worker must never outlive the renderer object.
    ShutdownPrewarm();
    if (deviceWorker.joinable()) deviceWorker.join();
    ReleaseDeviceResources();
    svgGeometryCache.clear();
    asyncD3dDevice.Reset();
    asyncD3dContext.Reset();
    layoutCache.clear();
    layoutLru.clear();
    layoutCacheBytes = 0;
    sizedFormatCache.clear();
    textRenderingParams.Reset();
    customFontCollection.Reset();
    wicFactory.Reset();
}

void Renderer::CancelThumbnailWork() {
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailWorkerStopping = true;
        thumbnailGeneration++;
        pendingThumbnailRequests.clear();
        inFlightThumbnailKeys.clear();
        completedThumbnailResults.clear();
        completedThumbnailBytes = 0;
    }
    thumbnailCv.notify_all();
    for (auto& worker : thumbnailWorkers) if (worker.joinable()) CancelSynchronousIo(worker.native_handle());
}

bool Renderer::AreDeviceWorkersRunning() const {
    return !deviceWorkerDone.load(std::memory_order_acquire) ||
        (s_prewarmState.started.load(std::memory_order_acquire) && !s_prewarmState.done.load(std::memory_order_acquire));
}

size_t Renderer::ClearThumbnailCacheInternal() {
    deferredThumbnailUploads = false;
    const size_t evicted = thumbnailCache.size();
    thumbnailByBitmap.clear();
    thumbnailCache.clear();
    thumbnailDemandFrames.clear();
    thumbnailLru.clear();
    thumbnailCacheBytes = 0;
    negativeCache.clear();
    negativeLru.clear();
    negativeInfo.clear();
    negativeAttempts.clear();
    hubExtractAttempted.clear();
    thumbnailNotificationPosted.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailGeneration++;
        pendingThumbnailRequests.clear();
        inFlightThumbnailKeys.clear();
        completedThumbnailResults.clear();
        completedThumbnailBytes = 0;
    }
    thumbnailCv.notify_all();
    return evicted;
}

size_t Renderer::ClearThumbnailCache() {
    const size_t evicted = ClearThumbnailCacheInternal();
    // A worker may already be inside the persistence path when the generation
    // changes. Wait for that write to finish; workers reaching this gate later
    // re-check the generation and skip their stale write.
    { std::lock_guard<std::mutex> persistenceLock(thumbnailPersistenceMutex); }
    if (hWnd && IsWindow(hWnd)) { InvalidateRect(hWnd, nullptr, FALSE); PostMessageW(hWnd, WM_NULL, 0, 0); }
    return evicted;
}

void Renderer::TouchThumbnail(std::map<ThumbnailKey, ThumbnailEntry>::iterator it) {
    if (it == thumbnailCache.end()) return;
    it->second.lastDrawnFrame = thumbnailRequestFrame;
    thumbnailLru.splice(thumbnailLru.begin(), thumbnailLru, it->second.lruIt);
    it->second.lruIt = thumbnailLru.begin();
}

void Renderer::IndexThumbnailBitmap(std::map<ThumbnailKey, ThumbnailEntry>::iterator it) {
    if (it != thumbnailCache.end() && it->second.bitmap)
        thumbnailByBitmap[it->second.bitmap.Get()] = &it->second;
}

Renderer::ThumbnailEntry* Renderer::FindThumbnailByBitmap(ID2D1Bitmap* bitmap) {
    auto it = thumbnailByBitmap.find(bitmap);
    return it == thumbnailByBitmap.end() ? nullptr : it->second;
}

bool Renderer::TrimThumbnailCache(uint64_t incomingBytes) {
    while (!thumbnailLru.empty() && (thumbnailCache.size() >= kThumbnailCacheEntryLimit ||
            thumbnailCacheBytes + incomingBytes > kThumbnailCacheByteLimit)) {
        bool evictedAny = false;
        for (auto itLru = thumbnailLru.rbegin(); itLru != thumbnailLru.rend(); ++itLru) {
            auto it = thumbnailCache.find(*itLru);
            if (it != thumbnailCache.end()) {
                if (it->second.lastDrawnFrame != 0 && thumbnailRequestFrame - it->second.lastDrawnFrame <= 1) {
                    continue; // Preserve the previous viewport during BeginDraw's drain.
                }
                thumbnailCacheBytes -= std::min(thumbnailCacheBytes, it->second.approximateBytes);
                thumbnailByBitmap.erase(it->second.bitmap.Get());
                thumbnailCache.erase(it);
                thumbnailLru.erase(std::next(itLru).base());
                evictedAny = true;
                break;
            }
        }
        if (!evictedAny) {
            return false; // Defer admission rather than evict visible images.
        }
    }
    return thumbnailCacheBytes + incomingBytes <= kThumbnailCacheByteLimit;
}

void Renderer::TrimHiddenLibraryCaches(uint64_t retainedThumbnailBytes, bool releaseDevice) {
    // Playback uses a separate video/overlay surface. The hub can recreate its
    // graphics device on the next draw; minimized hubs retain their device.
    if (releaseDevice) ReleaseDeviceResources();
    else if (swapChain && d2dContext && !buffersTrimmed) {
        if (ResizeRenderTarget(1, 1)) buffersTrimmed = true;
        else RecreateDeviceResources();
    }
    deferredThumbnailUploads = false;
    thumbnailNotificationPosted.store(false, std::memory_order_release);
    // The hub is hidden. Keep its most recently used images for a quick return,
    // but do not protect the old viewport indefinitely while a movie plays.
    while (!thumbnailLru.empty() && thumbnailCacheBytes > retainedThumbnailBytes) {
        auto entry = thumbnailCache.find(thumbnailLru.back());
        if (entry != thumbnailCache.end()) {
            thumbnailCacheBytes -= std::min(thumbnailCacheBytes, entry->second.approximateBytes);
            thumbnailByBitmap.erase(entry->second.bitmap.Get());
            thumbnailCache.erase(entry);
        }
        thumbnailLru.pop_back();
    }
    ClearShadowCache();
    layoutCache.clear();
    layoutLru.clear();
    layoutCacheBytes = 0;
    sizedFormatCache.clear();
    wicFactory.Reset();
    // Completed software thumbnails can otherwise remain queued for the whole
    // movie. In-flight results become stale; workers and disk entries survive.
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        ++thumbnailGeneration;
        pendingThumbnailRequests.clear();
        inFlightThumbnailKeys.clear();
        completedThumbnailResults.clear();
        completedThumbnailBytes = 0;
    }
    // Wake both producers waiting for result space and idle workers holding a
    // probe from the old generation. They must observe the invalidation now.
    thumbnailCv.notify_all();
}

void Renderer::ReleaseDeviceResources() {
    if (d2dContext) { d2dContext->SetTarget(nullptr); }
    d2dTargetBitmap.Reset();
    solidBrush.Reset();
    ClearThumbnailCacheInternal();
    ClearShadowCache();
    gradientStopCache.clear();
    gradientBrushCache.clear();
    innerHighlightCache.clear();
    innerHighlightBrushCache.clear();
    // A fresh device has not presented anything yet; re-arm the pre-frame
    // background erase so a device transition cannot show stale/white content.
    hasPresentedFrame = false;
    buffersTrimmed = false;
    if (frameLatencyWaitableObject) { CloseHandle(frameLatencyWaitableObject); frameLatencyWaitableObject = NULL; }
    if (d3dContext) { d3dContext->ClearState(); d3dContext->Flush(); }
    d2dContext.Reset();
    d2dDevice.Reset();
    swapChain.Reset();
    d3dContext.Reset();
    d3dDevice.Reset();
}

bool Renderer::IsDeviceLost(HRESULT hr) const {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR || hr == DXGI_ERROR_DEVICE_HUNG) {
        return true;
    }
    if (d3dDevice) {
        HRESULT reason = d3dDevice->GetDeviceRemovedReason();
        return reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
               reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR || reason == DXGI_ERROR_DEVICE_HUNG;
    }
    return false;
}

void Renderer::CreateFonts(const std::wstring& customFamily) {
    if (!dwriteFactory) return;
    selectedFontFamily = customFamily.empty() ? L"Inter" : customFamily;
    LoadCustomFontCollection();
    layoutCache.clear();
    layoutLru.clear();
    layoutCacheBytes = 0;
    sizedFormatCache.clear();
    CreateFontFormats();
}
void Renderer::UpdateFontFamily(const std::string& familyName) {
    selectedFontFamily = Utf8ToUtf16(familyName);
    CreateFonts(selectedFontFamily);
}

void Renderer::SetTextScale(float scale) {
    const float normalized = std::clamp(scale, 0.75f, 2.25f);
    if (std::abs(normalized - textScale) < 0.001f) return;
    textScale = normalized;
    CreateFonts(selectedFontFamily);
}

void Renderer::CreateTextRenderingParams() {
    textRenderingParams.Reset();
    if (!dwriteFactory) return;
    ComPtr<IDWriteFactory3> factory3;
    if (FAILED(dwriteFactory.As(&factory3)) || !factory3) return;
    ComPtr<IDWriteRenderingParams> monitorParams;
    HMONITOR hMon = hWnd ? MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST) : NULL;
    if (hMon) { dwriteFactory->CreateMonitorRenderingParams(hMon, &monitorParams); }
    if (!monitorParams) { dwriteFactory->CreateRenderingParams(&monitorParams); }
    float gamma = monitorParams ? monitorParams->GetGamma() : 1.8f;
    float enhancedContrast = monitorParams ? monitorParams->GetEnhancedContrast() : 0.5f;
    float clearTypeLevel = monitorParams ? monitorParams->GetClearTypeLevel() : 1.0f;
    DWRITE_PIXEL_GEOMETRY pixelGeometry = monitorParams ? monitorParams->GetPixelGeometry() : DWRITE_PIXEL_GEOMETRY_RGB;
    // Natural symmetric ClearType text rendering on opaque swap chain matching Chromium/WebView Subpixel text rasterization.
    factory3->CreateCustomRenderingParams( gamma, enhancedContrast, enhancedContrast, clearTypeLevel, pixelGeometry,
        DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, DWRITE_GRID_FIT_MODE_DISABLED, &textRenderingParams );
}

bool Renderer::Initialize(HWND hwnd, const std::string& initialFont, bool prewarmDevice) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    // Standalone renderers still overlap device creation with font setup. In the
    // app, this reuses the prewarm already started before window creation.
    if (prewarmDevice) BeginCreateDeviceAsync();
    if (!d2dFactory) {
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory1), &options, &d2dFactory))) return false;
    }
    if (!dwriteFactory) {
        ComPtr<IDWriteFactory> baseFactory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &baseFactory)) ||
                FAILED(baseFactory.As(&dwriteFactory))) return false;
    }
    hWnd = hwnd;
    // 1. Precise Per-Monitor V2 DPI detection
    typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowProc pGetDpiForWindow = hUser32 ? (GetDpiForWindowProc)GetProcAddress(hUser32, "GetDpiForWindow") : nullptr;
    if (pGetDpiForWindow) {
        dpi = (float)pGetDpiForWindow(hWnd);
    } else { HDC hdc = GetDC(hWnd); dpi = (float)GetDeviceCaps(hdc, LOGPIXELSX); ReleaseDC(hWnd, hdc); }
    if (dpi < 48.0f) dpi = 96.0f;
    dpiScale = dpi / 96.0f;
    RECT rc;
    GetClientRect(hWnd, &rc);
    width = std::max(1u, (UINT)(rc.right - rc.left));
    height = std::max(1u, (UINT)(rc.bottom - rc.top));
    CreateTextRenderingParams();
    CreateFonts(!initialFont.empty() ? Utf8ToUtf16(initialFont) : L"Inter");
    return true;
}

void Renderer::PrewarmD3DDeviceAsync() {
    if (s_prewarmState.started.exchange(true)) return;
    s_prewarmState.done.store(false, std::memory_order_release);
    try { s_prewarmState.worker = std::thread([]() {
        s_prewarmState.failed = !CreateD3DDevice(s_prewarmState.device, s_prewarmState.context);
        s_prewarmState.done.store(true, std::memory_order_release);
    }); } catch (...) {
        s_prewarmState.failed = true;
        s_prewarmState.done.store(true, std::memory_order_release);
        s_prewarmState.started.store(false, std::memory_order_release);
    }
}

void Renderer::ShutdownPrewarm() { s_prewarmState.Shutdown(); }

void Renderer::BeginCreateDeviceAsync() {
    if (s_prewarmState.started.load() || deviceWorker.joinable() || d3dDevice || asyncDeviceFailed) return;
    deviceWorkerDone.store(false, std::memory_order_release);
    try { deviceWorker = std::thread([this]() {
        asyncDeviceFailed = !CreateD3DDevice(asyncD3dDevice, asyncD3dContext);
        deviceWorkerDone.store(true, std::memory_order_release);
    }); } catch (...) {
        asyncDeviceFailed = true;
        deviceWorkerDone.store(true, std::memory_order_release);
    }
}

bool Renderer::EnsureD3DDevice() {
    if (d3dDevice && d3dContext) return true;
    if (s_prewarmState.started.load()) {
        if (s_prewarmState.worker.joinable()) s_prewarmState.worker.join();
        if (!s_prewarmState.failed && s_prewarmState.device && s_prewarmState.context) {
            d3dDevice = std::move(s_prewarmState.device);
            d3dContext = std::move(s_prewarmState.context);
            return true;
        }
    }
    if (deviceWorker.joinable()) deviceWorker.join();
    if (!asyncDeviceFailed && asyncD3dDevice && asyncD3dContext) {
        // Transfer ownership so a later device-loss recreate does not re-adopt the dead async device.
        d3dDevice = std::move(asyncD3dDevice);
        d3dContext = std::move(asyncD3dContext);
        return true;
    }
    // Synchronous path: device-loss recreation after the async device was consumed, or the async attempt failed.
    return CreateD3DDevice(d3dDevice, d3dContext);
}

bool Renderer::CreateDeviceResources() {
    if (!hWnd || !d2dFactory) return false;
    if (!EnsureD3DDevice()) return false;
    RECT rc = { 0 };
    if (GetClientRect(hWnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
        width = (UINT)(rc.right - rc.left);
        height = (UINT)(rc.bottom - rc.top);
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIDevice1> dxgiDevice1;
    ComPtr<IDXGIAdapter> dxgiAdapter;
    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(d3dDevice.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&dxgiAdapter)) ||
        FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
        ReleaseDeviceResources();
        return false;
    }
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(dxgiAdapter->GetDesc(&adapterDesc))) {
        adapterName = adapterDesc.Description;
        // WARP exposes the Microsoft Basic Render Driver (vendor 0x1414,
        // device 0x008c); the name check also covers software adapter variants.
        softwareDevice = (adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) ||
            adapterName.find(L"Basic Render") != std::wstring::npos;
    }
    if (SUCCEEDED(d3dDevice.As(&dxgiDevice1)) && dxgiDevice1) { dxgiDevice1->SetMaximumFrameLatency(1); }
    if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice)) ||
        FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext))) {
        ReleaseDeviceResources();
        return false;
    }
    // UIComponents supplies physical-pixel coordinates; 96 DPI prevents a
    // second implicit scale from changing layout during monitor transitions.
    d2dContext->SetDpi(96.0f, 96.0f);
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = std::max(1u, width);
    scDesc.Height = std::max(1u, height);
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_NONE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    if (FAILED(hr)) {
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    }
    if (FAILED(hr)) {
        scDesc.Flags = 0;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    }
    if (FAILED(hr)) {
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    }
    if (FAILED(hr)) {
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    }
    if (FAILED(hr)) {
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        hr = dxgiFactory->CreateSwapChainForHwnd(d3dDevice.Get(), hWnd, &scDesc, nullptr, nullptr, &swapChain);
    }
    if (FAILED(hr)) { ReleaseDeviceResources(); return false; }
    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain.As(&swapChain2)) && swapChain2) {
        swapChain2->SetMaximumFrameLatency(1);
        frameLatencyWaitableObject = swapChain2->GetFrameLatencyWaitableObject();
    }
    dxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &solidBrush))) {
        ReleaseDeviceResources();
        return false;
    }
    return RecreateRenderTarget();
}

bool Renderer::RecreateDeviceResources() {
    if (recoveringDevice || !hWnd) return false;
    recoveringDevice = true;
    ReleaseDeviceResources();
    bool ok = CreateDeviceResources();
    recoveringDevice = false;
    return ok;
}

bool Renderer::EnsureGraphicsContext() {
    if (d2dContext && d2dTargetBitmap) return true;
    return RecreateDeviceResources();
}

void Renderer::SetDpi(float newDpi) {
    dpi = std::clamp(newDpi, 48.0f, 768.0f);
    dpiScale = dpi / 96.0f;
    if (d2dContext) { d2dContext->SetDpi(96.0f, 96.0f); }
    CreateTextRenderingParams();
    layoutCache.clear();
    layoutLru.clear();
    layoutCacheBytes = 0;
    CreateFonts(selectedFontFamily);
}

bool Renderer::RecreateRenderTarget() {
    if (!swapChain || !d2dContext) return false;
    d2dContext->SetTarget(nullptr);
    d2dTargetBitmap.Reset();
    // Brushes, gradient stops, and decoded thumbnails all belong to the D2D
    // device/target.  Text formats/layouts intentionally remain alive:
    // DirectWrite resources are device-independent and are expensive to rebuild.
    solidBrush.Reset();
    ClearShadowCache();
    gradientStopCache.clear();
    gradientBrushCache.clear();
    innerHighlightCache.clear();
    innerHighlightBrushCache.clear();
    ComPtr<IDXGISurface> backBuffer;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1( D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f );
        hr = d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bp, &d2dTargetBitmap);
        if (FAILED(hr)) return false;
        d2dContext->SetTarget(d2dTargetBitmap.Get());
        if (FAILED(d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &solidBrush))) {
            d2dContext->SetTarget(nullptr);
            d2dTargetBitmap.Reset();
            return false;
        }
        return true;
    }
    return false;
}

void Renderer::Resize(UINT w, UINT h) {
    if (w == 0 || h == 0 || (w == width && h == height)) return;
    width = w;
    height = h;
    if (swapChain && d2dContext && !buffersTrimmed && !ResizeRenderTarget(width, height)) RecreateDeviceResources();
}

bool Renderer::ResizeRenderTarget(UINT w, UINT h) {
    d2dContext->SetTarget(nullptr);
    d2dTargetBitmap.Reset();
    const UINT flags = frameLatencyWaitableObject ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    return SUCCEEDED(swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags)) && RecreateRenderTarget();
}

void Renderer::BeginDraw() {
    if (buffersTrimmed) {
        buffersTrimmed = false;
        if (!ResizeRenderTarget(width, height)) RecreateDeviceResources();
    }
    if ((!d2dContext || !d2dTargetBitmap) && hWnd && !recoveringDevice) RecreateDeviceResources();
    ++thumbnailRequestFrame;
    prevFrameShadowKeys.swap(currFrameShadowKeys);
    currFrameShadowKeys.clear();
    for (auto it = thumbnailDemandFrames.begin(); it != thumbnailDemandFrames.end();) {
        if (thumbnailRequestFrame - it->second > 1) it = thumbnailDemandFrames.erase(it);
        else ++it;
    }
    thumbnailRequestOrder = 0;
    DrainCompletedThumbnails();
    if (d2dContext && d2dTargetBitmap) {
        d2dContext->BeginDraw();
        d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        d2dContext->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        if (textRenderingParams) d2dContext->SetTextRenderingParams(textRenderingParams.Get());
    }
}

bool Renderer::EndDraw(bool present) {
    if (!d2dContext || !d2dTargetBitmap) return false;
    HRESULT hr = d2dContext->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { if (!RecreateRenderTarget()) RecreateDeviceResources(); return false; }
    if (FAILED(hr)) { if (IsDeviceLost(hr)) RecreateDeviceResources(); return false; }
    if (swapChain && present) {
        DXGI_PRESENT_PARAMETERS params = {};
        // The very first present skips the vblank wait: it only gates how quickly the first composed frame appears, never pacing.
        const UINT interval = hasPresentedFrame ? 1u : 0u;
        const auto presentStart = std::chrono::steady_clock::now();
        HRESULT presentHr = swapChain->Present1(interval, 0, &params);
        lastPresentMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
        maxPresentMs = std::max(maxPresentMs, lastPresentMs);
        isOccluded = (presentHr == DXGI_STATUS_OCCLUDED);
        if (IsDeviceLost(presentHr)) { RecreateDeviceResources(); return false; }
        if (FAILED(presentHr) && presentHr != DXGI_STATUS_OCCLUDED) return false;
        hasPresentedFrame = true;
    }
    return SUCCEEDED(hr);
}

void Renderer::Clear(const D2D1_COLOR_F& color) { if (d2dContext) d2dContext->Clear(color); }

void Renderer::FillGradientRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor, bool vertical) {
    if (!d2dContext) return;
    D2D1_RECT_F snappedRect = SnapRect(rect);
    GradientKey key;
    key.startR = QuantizeColor(startColor.r); key.startG = QuantizeColor(startColor.g);
    key.startB = QuantizeColor(startColor.b); key.startA = QuantizeColor(startColor.a);
    key.endR = QuantizeColor(endColor.r); key.endG = QuantizeColor(endColor.g);
    key.endB = QuantizeColor(endColor.b); key.endA = QuantizeColor(endColor.a);
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    HRESULT hr = S_OK;
    auto cached = gradientStopCache.find(key);
    if (cached != gradientStopCache.end()) { stopCollection = cached->second; } else {
        D2D1_GRADIENT_STOP stops[2];
        stops[0].color = startColor;
        stops[0].position = 0.0f;
        stops[1].color = endColor;
        stops[1].position = 1.0f;
        hr = d2dContext->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection);
        if (FAILED(hr)) return;
        // Evict one entry instead of clearing: a wholesale clear forces D2D stop/brush recreation for every gradient in the theme.
        if (gradientStopCache.size() >= 64) gradientStopCache.erase(gradientStopCache.begin());
        gradientStopCache.emplace(key, stopCollection);
    }
    D2D1_POINT_2F startPt = D2D1::Point2F(snappedRect.left, snappedRect.top);
    D2D1_POINT_2F endPt = vertical ? D2D1::Point2F(snappedRect.left, snappedRect.bottom) : D2D1::Point2F(snappedRect.right, snappedRect.top);
    // Gradient brushes are device resources; reuse one brush per color key and
    // just retarget its endpoints instead of allocating a brush per fill.
    ComPtr<ID2D1LinearGradientBrush> gradBrush;
    auto brushIt = gradientBrushCache.find(key);
    if (brushIt != gradientBrushCache.end()) { gradBrush = brushIt->second; } else {
        hr = d2dContext->CreateLinearGradientBrush( D2D1::LinearGradientBrushProperties(startPt, endPt), stopCollection.Get(), &gradBrush );
        if (FAILED(hr)) return;
        // Evict one entry instead of clearing: a wholesale clear forces D2D stop/brush recreation for every gradient in the theme.
        if (gradientBrushCache.size() >= 64) gradientBrushCache.erase(gradientBrushCache.begin());
        gradientBrushCache.emplace(key, gradBrush);
    }
    if (gradBrush) {
        gradBrush->SetStartPoint(startPt);
        gradBrush->SetEndPoint(endPt);
        d2dContext->FillRectangle(snappedRect, gradBrush.Get());
    }
}

void Renderer::DrawDashedRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float strokeWidth) {
    if (!d2dContext || !solidBrush || !d2dFactory) return;
    solidBrush->SetColor(color);
    float halfStroke = strokeWidth * 0.5f;
    D2D1_RECT_F insetRect = D2D1::RectF( rect.left + halfStroke, rect.top + halfStroke, rect.right - halfStroke, rect.bottom - halfStroke );
    float adjRadius = std::max(0.0f, radius - halfStroke);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(insetRect, adjRadius, adjRadius);
    float dashes[] = { 4.0f, 4.0f };
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties( D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f );
    if (!dashedStrokeStyle) { d2dFactory->CreateStrokeStyle(sp, dashes, 2, &dashedStrokeStyle); }
    if (dashedStrokeStyle) {
        d2dContext->DrawRoundedRectangle(rr, solidBrush.Get(), strokeWidth, dashedStrokeStyle.Get());
    } else { d2dContext->DrawRoundedRectangle(rr, solidBrush.Get(), strokeWidth); }
}

void Renderer::DrawRingProgress(float cx, float cy, float radius, float thickness, float progress, const D2D1_COLOR_F& trackCol, const D2D1_COLOR_F& accentCol) {
    if (!d2dContext || !solidBrush || !d2dFactory) return;
    // Background track ring
    solidBrush->SetColor(trackCol);
    d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), solidBrush.Get(), thickness);
    if (progress <= 0.001f) return;
    // Active progress arc
    solidBrush->SetColor(accentCol);
    if (progress >= 0.999f) {
        d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), solidBrush.Get(), thickness);
        return;
    }
    ComPtr<ID2D1PathGeometry> pathGeom;
    if (FAILED(d2dFactory->CreatePathGeometry(&pathGeom))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(pathGeom->Open(&sink))) return;
    float startAngle = -3.14159265f * 0.5f; // 12 o'clock
    float endAngle = startAngle + progress * 3.14159265f * 2.0f;
    float startX = cx + radius * cosf(startAngle);
    float startY = cy + radius * sinf(startAngle);
    float endX = cx + radius * cosf(endAngle);
    float endY = cy + radius * sinf(endAngle);
    sink->BeginFigure(D2D1::Point2F(startX, startY), D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT arc = {D2D1::Point2F(endX, endY), D2D1::SizeF(radius, radius), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            (progress > 0.5f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
    sink->AddArc(arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties( D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f );
    if (!ringStrokeStyle) { d2dFactory->CreateStrokeStyle(sp, nullptr, 0, &ringStrokeStyle); }
    d2dContext->DrawGeometry(pathGeom.Get(), solidBrush.Get(), thickness, ringStrokeStyle.Get());
}

ComPtr<IDWriteTextLayout> Renderer::GetOrCreateTextLayout( const std::wstring& text, FontType font, float maxW, float maxH,
    DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign, TextLayoutMode mode ) {
    if (!dwriteFactory || text.empty()) return nullptr;
    // Rapidly changing time/progress strings (e.g. "00:03 / 01:23", "75%") must not occupy long-lived cache slots: create them directly.
    bool isTransient = false;
    if (text.size() <= 32) {
        bool hasDigit = false, hasColon = false;
        for (wchar_t c : text) { if (c >= L'0' && c <= L'9') hasDigit = true; if (c == L':') hasColon = true; }
        if (hasDigit && hasColon) { isTransient = true; } else if (hasDigit && !text.empty() && text.back() == L'%') { isTransient = true; }
    }
    IDWriteTextFormat* tf = GetTextFormat(font);
    if (!tf) return nullptr;
    auto createLayout = [&]() -> ComPtr<IDWriteTextLayout> {
        ComPtr<IDWriteTextLayout> layout;
        const HRESULT hr = dwriteFactory->CreateTextLayout( text.c_str(), (UINT32)text.length(), tf, maxW, maxH, &layout);
        if (FAILED(hr) || !layout) return nullptr;
        const bool isRtl = Localization::Instance().IsRtl();
        layout->SetReadingDirection(isRtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        layout->SetFlowDirection(DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM);
        layout->SetTextAlignment(align);
        layout->SetParagraphAlignment(valign);
        // Wrapping/trimming belong to the cache key. Mutating them for each
        // draw invalidates DirectWrite's layout work even on a cache hit.
        if (mode != TextLayoutMode::SingleLine) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
        }
        if (mode == TextLayoutMode::WrappedEllipsis) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwriteFactory->CreateEllipsisTrimmingSign(tf, &ellipsis))) {
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        return layout;
    };
    if (isTransient) return createLayout();
    // Copy-free probe: the map's transparent comparator matches on a view, so
    // a cache hit never copies the text into a LayoutKey.
    const LayoutKeyProbe probe{ std::wstring_view(text), font,
        (int)std::lround(maxW * 4.0f), (int)std::lround(maxH * 4.0f), align, valign, mode };
    auto it = layoutCache.find(probe);
    if (it != layoutCache.end()) {
        layoutLru.splice(layoutLru.begin(), layoutLru, it->second.lruIt);
        it->second.lruIt = layoutLru.begin();
        return it->second.layout;
    }
    auto layout = createLayout();
    if (layout) {
        auto estimateBytes = [](const LayoutKey& k) -> uint64_t {
            // Key text twice (map + LRU) + shaped glyph runs (~2 KiB base).
            return (uint64_t)k.text.size() * sizeof(wchar_t) * 2ull + 2048ull + sizeof(LayoutKey);
        };
        LayoutKey key{ std::wstring(text), font, probe.maxW, probe.maxH, probe.align, probe.valign, probe.mode };
        uint64_t incoming = estimateBytes(key);
        while ((!layoutCache.empty()) && (layoutCache.size() >= kLayoutCacheEntryLimit ||
                layoutCacheBytes + incoming > kLayoutCacheByteLimit) && !layoutLru.empty()) {
            const LayoutKey& oldKey = layoutLru.back();
            auto oldIt = layoutCache.find(oldKey);
            if (oldIt != layoutCache.end()) {
                uint64_t oldBytes = estimateBytes(oldIt->first);
                layoutCacheBytes = (oldBytes > layoutCacheBytes) ? 0 : (layoutCacheBytes - oldBytes);
                layoutCache.erase(oldIt);
            }
            layoutLru.pop_back();
        }
        layoutLru.push_front(key);
        LayoutCacheEntry entry;
        entry.layout = layout;
        entry.lruIt = layoutLru.begin();
        layoutCacheBytes += incoming;
        layoutCache.emplace(key, std::move(entry));
        return layout;
    }
    return nullptr;
}

void Renderer::DrawString(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, FontType font, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!d2dContext || !solidBrush || text.empty()) return;
    D2D1_RECT_F snappedRect = SnapRect(rect);
    ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout(
        text, font, std::max(1.0f, RectWidth(snappedRect)), std::max(1.0f, RectHeight(snappedRect)), align, valign );
    if (!layout) return;
    solidBrush->SetColor(color);
    d2dContext->DrawTextLayout( D2D1::Point2F(snappedRect.left, snappedRect.top), layout.Get(), solidBrush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT );
}

void Renderer::DrawStringSized(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float sizeDp,
                               DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!d2dContext || !solidBrush || text.empty()) return;
    IDWriteTextFormat* tf = GetOrCreateSizedFormat(sizeDp, weight);
    if (!tf) return;
    tf->SetTextAlignment(align);
    tf->SetParagraphAlignment(valign);
    D2D1_RECT_F snappedRect = SnapRect(rect);
    solidBrush->SetColor(color);
    d2dContext->DrawTextW(text.c_str(), (UINT32)text.length(), tf, snappedRect, solidBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Renderer::DrawStringWrapped(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, FontType font, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (!d2dContext || !solidBrush || text.empty()) return;
    D2D1_RECT_F snappedRect = SnapRect(rect);
    ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout(
        text, font, std::max(1.0f, RectWidth(snappedRect)), std::max(1.0f, RectHeight(snappedRect)), align, valign,
        TextLayoutMode::WrappedEllipsis );
    if (!layout) return;
    solidBrush->SetColor(color);
    d2dContext->DrawTextLayout( D2D1::Point2F(snappedRect.left, snappedRect.top), layout.Get(), solidBrush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP );
}

void Renderer::DrawStringWithShadow(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, const D2D1_COLOR_F& shadowColor, FontType font, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign, float shadowOffsetY) {
    if (shadowColor.a > 0.001f) {
        D2D1_RECT_F sRect = D2D1::RectF(rect.left, rect.top + shadowOffsetY * dpiScale, rect.right, rect.bottom + shadowOffsetY * dpiScale);
        DrawString(text, sRect, shadowColor, font, align, valign);
    }
    DrawString(text, rect, color, font, align, valign);
}

D2D1_SIZE_F Renderer::MeasureString(const std::wstring& text, FontType font, float maxW, float maxH) {
    ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout( text, font, maxW, maxH, DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT_NEAR );
    if (!layout) return D2D1::SizeF(0, 0);
    DWRITE_TEXT_METRICS tm;
    layout->GetMetrics(&tm);
    return D2D1::SizeF(tm.widthIncludingTrailingWhitespace, tm.height);
}

float Renderer::MeasureStringWrappedHeight(const std::wstring& text, FontType font, float maxWidth) {
    if (!dwriteFactory || text.empty() || maxWidth <= 1.0f) return 0.0f;
    ComPtr<IDWriteTextLayout> layout = GetOrCreateTextLayout(
        text, font, maxWidth, 10000.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
        TextLayoutMode::Wrapped);
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    return tm.height;
}

void Renderer::DrawProgressBar(const D2D1_RECT_F& rect, double progress, const D2D1_COLOR_F& trackCol, const D2D1_COLOR_F& fillCol, float radius) {
    float dp = dpiScale, r = radius * dp;
    // Background groove
    FillRoundedRect(rect, r, trackCol);
    DrawRoundedRect(rect, r, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), 1.0f);
    float fillRatio = (float)std::clamp(progress, 0.0, 1.0);
    if (fillRatio > 0.001f) {
        float fillW = std::max(r * 2.0f, RectWidth(rect) * fillRatio);
        D2D1_RECT_F fillRect = D2D1::RectF(rect.left, rect.top, rect.left + fillW, rect.bottom);
        FillRoundedRect(fillRect, r, fillCol);
    }
}

void Renderer::DrawBitmapRounded(const D2D1_RECT_F& rect, float radius, ID2D1Bitmap* pBitmap, float opacity, bool cropToFill) {
    if (!d2dContext || !pBitmap || opacity <= 0.001f) return;
    D2D1_SIZE_F imgSz = pBitmap->GetSize();
    if (imgSz.width <= 0.0f || imgSz.height <= 0.0f) return;
    D2D1_RECT_F snappedRect = SnapRect(rect);
    float scaleX = RectWidth(snappedRect) / imgSz.width;
    float scaleY = RectHeight(snappedRect) / imgSz.height;
    float scale = cropToFill ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    float drawW = imgSz.width * scale, drawH = imgSz.height * scale;
    float drawX = snappedRect.left + (RectWidth(snappedRect) - drawW) * 0.5f;
    float drawY = snappedRect.top + (RectHeight(snappedRect) - drawH) * 0.5f;
    // Thumbnails are owned by thumbnailCache, so their brush can be reused safely. Filling a rounded rectangle with a transformed bitmap
    // brush preserves the crop while avoiding a geometry allocation and PushLayer for every visible card.
    if (cropToFill && radius > 0.0f) {
        ThumbnailEntry* thumbnailEntry = FindThumbnailByBitmap(pBitmap);
        if (thumbnailEntry) {
            if (!thumbnailEntry->brush) {
                const D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties( D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                d2dContext->CreateBitmapBrush(pBitmap, brushProps, &thumbnailEntry->brush);
            }
            if (thumbnailEntry->brush) {
                thumbnailEntry->brush->SetTransform( D2D1::Matrix3x2F(scale, 0.0f, 0.0f, scale, drawX, drawY));
                thumbnailEntry->brush->SetOpacity(opacity);
                d2dContext->FillRoundedRectangle( D2D1::RoundedRect(snappedRect, radius, radius), thumbnailEntry->brush.Get());
                thumbnailEntry->brush->SetOpacity(1.0f);
                return;
            }
        }
    }
    if (radius <= 0.0f) {
        d2dContext->DrawBitmap( pBitmap, D2D1::RectF(drawX, drawY, drawX + drawW, drawY + drawH), opacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR );
        return;
    }
    // DrawBitmap is cheaper than creating a bitmap brush for every thumbnail. A rounded geometry layer remains the compatibility path for
    // external bitmaps and for the non-cropping letterbox mode.
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (!d2dFactory || FAILED(d2dFactory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(snappedRect, radius, radius), &geometry)) || !geometry) return;
    d2dContext->PushLayer(D2D1::LayerParameters(snappedRect, geometry.Get()), nullptr);
    d2dContext->DrawBitmap( pBitmap, D2D1::RectF(drawX, drawY, drawX + drawW, drawY + drawH), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
    );
    d2dContext->PopLayer();
}

void Renderer::DrawThumbnailCroppedBottom(const D2D1_RECT_F& visibleRect, const D2D1_RECT_F& originalRect, float radius, ID2D1Bitmap* pBitmap, float opacity) {
    if (!d2dContext || !pBitmap || opacity <= 0.001f) return;
    D2D1_SIZE_F imgSz = pBitmap->GetSize();
    if (imgSz.width <= 0.0f || imgSz.height <= 0.0f) return;
    D2D1_RECT_F snappedOrig = SnapRect(originalRect);
    D2D1_RECT_F snappedVis = SnapRect(visibleRect);
    float scaleX = RectWidth(snappedOrig) / imgSz.width;
    float scaleY = RectHeight(snappedOrig) / imgSz.height;
    float scale = std::max(scaleX, scaleY);
    float drawW = imgSz.width * scale, drawH = imgSz.height * scale;
    float drawX = snappedOrig.left + (RectWidth(snappedOrig) - drawW) * 0.5f;
    float drawY = snappedOrig.top + (RectHeight(snappedOrig) - drawH) * 0.5f;

    ThumbnailEntry* thumbnailEntry = FindThumbnailByBitmap(pBitmap);
    if (thumbnailEntry) {
        if (!thumbnailEntry->brush) {
            const D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties(
                D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            d2dContext->CreateBitmapBrush(pBitmap, brushProps, &thumbnailEntry->brush);
        }
        if (thumbnailEntry->brush) {
            thumbnailEntry->brush->SetTransform(D2D1::Matrix3x2F(scale, 0.0f, 0.0f, scale, drawX, drawY));
            thumbnailEntry->brush->SetOpacity(opacity);
            d2dContext->FillRoundedRectangle(D2D1::RoundedRect(snappedVis, radius, radius), thumbnailEntry->brush.Get());
            thumbnailEntry->brush->SetOpacity(1.0f);
            return;
        }
    }
    ComPtr<ID2D1BitmapBrush> tempBrush;
    const D2D1_BITMAP_BRUSH_PROPERTIES brushProps = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (SUCCEEDED(d2dContext->CreateBitmapBrush(pBitmap, brushProps, &tempBrush)) && tempBrush) {
        tempBrush->SetTransform(D2D1::Matrix3x2F(scale, 0.0f, 0.0f, scale, drawX, drawY));
        tempBrush->SetOpacity(opacity);
        d2dContext->FillRoundedRectangle(D2D1::RoundedRect(snappedVis, radius, radius), tempBrush.Get());
    }
}

void Renderer::PushClip(const D2D1_RECT_F& rect) {
    if (d2dContext) { d2dContext->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
}

void Renderer::PopClip() {
    if (d2dContext) { d2dContext->PopAxisAlignedClip(); }
}

void Renderer::DrawCardShadow(const D2D1_RECT_F& rect, float radius, float elevation) {
    float dp = dpiScale;
    DrawDropShadow(rect, radius, elevation * 3.5f * dp, elevation * 0.8f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.32f));
}

void Renderer::TouchShadow(std::map<ShadowKey, ShadowEntry>::iterator it) {
    if (it == shadowCache.end()) return;
    shadowLru.splice(shadowLru.begin(), shadowLru, it->second.lruIt);
    it->second.lruIt = shadowLru.begin();
}

void Renderer::ClearShadowCache() {
    shadowCache.clear();
    shadowLru.clear();
    prevFrameShadowKeys.clear();
    currFrameShadowKeys.clear();
    shadowCacheBytes = 0;
}

ComPtr<ID2D1Bitmap> Renderer::GetOrCreateShadowBitmap(const ShadowKey& key) {
    if (!d2dContext || key.widthPx <= 0 || key.heightPx <= 0 || key.blurQ <= 0) return nullptr;
    currFrameShadowKeys.insert(key);
    auto cached = shadowCache.find(key);
    if (cached != shadowCache.end()) { TouchShadow(cached); return cached->second.bitmap; }
    // Only allocate an off-screen D2D bitmap render target when the shadow key
    // has held its exact dimensions for at least two consecutive frames.
    // Actively resizing or morphing surfaces (card hover expansion, sliding nav
    // pills between different-width tabs, dynamic island morphs) use the direct
    // 6-pass draw path without thrashing GPU texture allocations every frame.
    if (prevFrameShadowKeys.find(key) == prevFrameShadowKeys.end()) return nullptr;
    const float radius = std::max(0.0f, key.radiusQ / 4.0f);
    const float blur = std::max(0.0f, key.blurQ / 4.0f);
    const float yOffset = key.offsetYQ / 4.0f;
    const float padXf = std::ceil(blur) + 2.0f;
    const float topPadf = std::ceil(blur * 0.20f + std::max(0.0f, -yOffset)) + 2.0f;
    const float bottomPadf = std::ceil(blur * 1.20f + std::max(0.0f, yOffset)) + 2.0f;
    const UINT padX = static_cast<UINT>(std::max(2.0f, padXf));
    const UINT topPad = static_cast<UINT>(std::max(2.0f, topPadf));
    const UINT bottomPad = static_cast<UINT>(std::max(2.0f, bottomPadf));
    const UINT pixelWidth = static_cast<UINT>(key.widthPx) + padX * 2u;
    const UINT pixelHeight = static_cast<UINT>(key.heightPx) + topPad + bottomPad;
    // Do not allow a pathological window size or blur setting to turn the cache into a large off-screen allocation. The caller has a
    // correct immediate-render fallback for this case.
    if (pixelWidth == 0 || pixelHeight == 0 || pixelWidth > 4096 || pixelHeight > 4096) return nullptr;
    D2D1_SIZE_U pixelSize = D2D1::SizeU(pixelWidth, pixelHeight);
    const D2D1_PIXEL_FORMAT shadowPixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1BitmapRenderTarget> shadowTarget;
    HRESULT hr = d2dContext->CreateCompatibleRenderTarget( nullptr, &pixelSize, &shadowPixelFormat,
        D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &shadowTarget );
    if (FAILED(hr) || !shadowTarget) return nullptr;
    shadowTarget->SetDpi(96.0f, 96.0f);
    shadowTarget->BeginDraw();
    shadowTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    D2D1_COLOR_F shadowColor = D2D1::ColorF( key.colorR / 65535.0f, key.colorG / 65535.0f, key.colorB / 65535.0f, key.colorA / 65535.0f );
    ComPtr<ID2D1SolidColorBrush> shadowBrush;
    hr = shadowTarget->CreateSolidColorBrush(shadowColor, &shadowBrush);
    if (FAILED(hr) || !shadowBrush) { shadowTarget->EndDraw(); return nullptr; }
    const D2D1_RECT_F baseRect = D2D1::RectF( static_cast<float>(padX), static_cast<float>(topPad),
        static_cast<float>(padX + static_cast<UINT>(key.widthPx)), static_cast<float>(topPad + static_cast<UINT>(key.heightPx)) );
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float grow = blur * t;
        D2D1_ROUNDED_RECT s = D2D1::RoundedRect(baseRect, radius + grow, radius + grow);
        s.rect.left -= grow;
        s.rect.right += grow;
        s.rect.top -= grow * 0.20f;
        s.rect.bottom += grow * 1.20f;
        s.rect.top += yOffset;
        s.rect.bottom += yOffset;
        const float falloff = std::pow(1.0f - t, 1.65f);
        shadowBrush->SetColor(D2D1::ColorF( shadowColor.r, shadowColor.g, shadowColor.b, shadowColor.a * falloff * 0.62f ));
        shadowTarget->FillRoundedRectangle(s, shadowBrush.Get());
    }
    hr = shadowTarget->EndDraw();
    if (FAILED(hr)) return nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(shadowTarget->GetBitmap(&bitmap)) || !bitmap) return nullptr;
    const uint64_t approximateBytes = static_cast<uint64_t>(pixelWidth) * pixelHeight * 4ull;
    if (approximateBytes > kShadowCacheByteLimit) {
        // Draw this unusually large shadow once, but never retain it.
        return bitmap;
    }
    while (!shadowLru.empty() && (shadowCache.size() >= kShadowCacheEntryLimit ||
            shadowCacheBytes + approximateBytes > kShadowCacheByteLimit)) {
        const ShadowKey& oldKey = shadowLru.back();
        auto old = shadowCache.find(oldKey);
        if (old != shadowCache.end()) {
            shadowCacheBytes -= std::min(shadowCacheBytes, old->second.approximateBytes);
            shadowCache.erase(old);
        }
        shadowLru.pop_back();
    }
    shadowLru.push_front(key);
    ShadowEntry entry;
    entry.bitmap = bitmap;
    entry.approximateBytes = approximateBytes;
    entry.lruIt = shadowLru.begin();
    auto inserted = shadowCache.emplace(key, std::move(entry));
    if (inserted.second) { shadowCacheBytes += approximateBytes; return bitmap; }
    shadowLru.pop_front();
    TouchShadow(inserted.first);
    return inserted.first->second.bitmap;
}

void Renderer::DrawDropShadow(const D2D1_RECT_F& rect, float radius, float blur, float yOffset, const D2D1_COLOR_F& color) {
    if (!d2dContext || !solidBrush || color.a <= 0.001f || blur <= 0.0f) return;
    ShadowKey key;
    key.widthPx = std::max(1, static_cast<int>(std::lround(RectWidth(rect))));
    key.heightPx = std::max(1, static_cast<int>(std::lround(RectHeight(rect))));
    key.radiusQ = std::max(0, static_cast<int>(std::lround(radius * 4.0f)));
    key.blurQ = std::max(1, static_cast<int>(std::lround(blur * 4.0f)));
    key.offsetYQ = static_cast<int>(std::lround(yOffset * 4.0f));
    key.colorR = QuantizeColor(color.r);
    key.colorG = QuantizeColor(color.g);
    key.colorB = QuantizeColor(color.b);
    key.colorA = QuantizeColor(color.a);
    if (ComPtr<ID2D1Bitmap> shadow = GetOrCreateShadowBitmap(key)) {
        const float blurQ = key.blurQ / 4.0f, offsetQ = key.offsetYQ / 4.0f;
        const float padX = std::ceil(blurQ) + 2.0f;
        const float topPad = std::ceil(blurQ * 0.20f + std::max(0.0f, -offsetQ)) + 2.0f;
        const D2D1_SIZE_F shadowSize = shadow->GetSize();
        d2dContext->DrawBitmap( shadow.Get(), D2D1::RectF(rect.left - padX, rect.top - topPad, rect.left - padX + shadowSize.width,
                        rect.top - topPad + shadowSize.height), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR );
        return;
    }
    // A small fixed kernel approximates CSS box-shadow falloff without allocating an effect graph. Six passes are enough for a soft edge
    // and remain substantially cheaper than a full Gaussian effect per card.
    const int steps = 6;
    for (int i = steps; i >= 1; --i) {
        float t = (float)i / (float)steps;
        float grow = blur * t;
        D2D1_ROUNDED_RECT s = D2D1::RoundedRect(rect, radius + grow, radius + grow);
        s.rect.left -= grow;   s.rect.right += grow;
        s.rect.top  -= grow * 0.20f; s.rect.bottom += grow * 1.20f;
        s.rect.top += yOffset; s.rect.bottom += yOffset;
        float falloff = std::pow(1.0f - t, 1.65f);
        float a = color.a * falloff * 0.62f;
        solidBrush->SetColor(D2D1::ColorF(color.r, color.g, color.b, a));
        d2dContext->FillRoundedRectangle(s, solidBrush.Get());
    }
}

void Renderer::DrawGlow(const D2D1_RECT_F& rect, float radius, float blur, const D2D1_COLOR_F& glowColor) {
    if (glowColor.a <= 0.001f || blur <= 0.0f) return;
    DrawDropShadow(rect, radius, blur, 0.0f, glowColor);
}

void Renderer::DrawSurface(const D2D1_RECT_F& rect, const SurfaceStyle& style) {
    if (!d2dContext) return;
    float dp = dpiScale;
    D2D1_RECT_F surfaceRect = SnapRect(rect);
    // 1. Soft Drop Shadow matching luxury acrylic depth
    if (style.shadowColor.a > 0.001f && style.shadowBlur > 0.0f)
        DrawDropShadow(surfaceRect, style.radius, style.shadowBlur, style.shadowOffsetY, style.shadowColor);
    // 2. Accent Glow (when active/hovered)
    if (style.glowColor.a > 0.001f && style.glowBlur > 0.0f) { DrawGlow(surfaceRect, style.radius, style.glowBlur, style.glowColor); }
    // Only content that cannot be expressed as a rounded primitive needs a
    // geometric layer. Plain fills and cached bitmap brushes stay layer-free.
    const bool needsRoundedLayer = style.hasGradient || style.pLivePreviewBitmap;
    ComPtr<ID2D1RoundedRectangleGeometry> rgeom;
    if (needsRoundedLayer && d2dFactory)
        d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(surfaceRect, style.radius, style.radius), &rgeom);
    if (rgeom) { d2dContext->PushLayer(D2D1::LayerParameters(surfaceRect, rgeom.Get()), nullptr); }
    // 3. Base Fill with native anti-aliasing
    if (style.fill.a > 0.001f) { FillRoundedRect(surfaceRect, style.radius, style.fill); }
    // 4. Static thumbnail backdrop (medium quality last frame even when not hovering)
    if (!style.backdropFile.empty()) {
        ComPtr<ID2D1Bitmap> bm = GetOrCreateVideoThumbnail(style.backdropFile, 320, 180, style.backdropStartSec);
        if (bm)
            DrawBitmapRounded(surfaceRect, style.radius, bm.Get(), style.backdropOpacity > 0.0f ? style.backdropOpacity : 0.35f, true);
        // Gradient overlay wash over backdrop so text is always crisp and high contrast
        if (needsRoundedLayer) {
            FillGradientRect(surfaceRect, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), false);
        } else {
            FillRoundedGradientRect(surfaceRect, style.radius, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), false);
        }
    }
    // 5. Live preview video frame (fading in on hover)
    if (style.pLivePreviewBitmap && style.livePreviewOpacity > 0.001f) {
        D2D1_SIZE_F imgSz = style.pLivePreviewBitmap->GetSize();
        if (imgSz.width > 0.0f && imgSz.height > 0.0f) {
            float scaleX = RectWidth(surfaceRect) / imgSz.width;
            float scaleY = RectHeight(surfaceRect) / imgSz.height;
            float scale = std::max(scaleX, scaleY);
            float drawW = imgSz.width * scale, drawH = imgSz.height * scale;
            float drawX = surfaceRect.left + (RectWidth(surfaceRect) - drawW) * 0.5f;
            float drawY = surfaceRect.top + (RectHeight(surfaceRect) - drawH) * 0.5f;
            D2D1_RECT_F dstRect = D2D1::RectF(drawX, drawY, drawX + drawW, drawY + drawH);
            d2dContext->DrawBitmap(style.pLivePreviewBitmap, dstRect, style.livePreviewOpacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        FillGradientRect(surfaceRect, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.70f * style.livePreviewOpacity), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f * style.livePreviewOpacity), false);
    }
    // 6. Subtle horizontal accent gradient wash
    if (style.hasGradient) {
        D2D1_RECT_F gradRect = D2D1::RectF(surfaceRect.left, surfaceRect.top, surfaceRect.left + RectWidth(surfaceRect) * 0.55f, surfaceRect.bottom);
        FillGradientRect(gradRect, style.gradientStart, style.gradientEnd, false);
    }
    // 7. Left Accent Bar with smooth rounded pill ends matching the card curvature
    if (style.leftAccentBarAlpha > 0.001f && style.leftAccentBarWidth > 0.0f) {
        float barW = style.leftAccentBarWidth * dp, insetY = 3.0f * dp, insetX = 1.0f * dp;
        D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
            D2D1::RectF(surfaceRect.left + insetX, surfaceRect.top + insetY, surfaceRect.left + insetX + barW, surfaceRect.bottom - insetY),
            barW * 0.5f, barW * 0.5f );
        solidBrush->SetColor(ColorWithAlpha(style.leftAccentBarColor, style.leftAccentBarAlpha));
        d2dContext->FillRoundedRectangle(pill, solidBrush.Get());
    }
    if (rgeom) { d2dContext->PopLayer(); }
    // 8. Specular Top Inner Highlight Shimmer
    if (style.hasInnerHighlight && style.innerHighlight.a > 0.001f && style.radius > 1.0f)
        DrawInnerHighlight(surfaceRect, style.radius, style.innerHighlight, style.borderWidth);
    // 9. Crisp Inset Border drawn on top for razor-sharp corners
    if (style.border.a > 0.001f && style.borderWidth > 0.0f)
        DrawRoundedRect(surfaceRect, style.radius, style.border, style.borderWidth);
}

SurfaceStyle Renderer::CreateSurfaceStyle(Elevation elevation, const Settings& s, bool isHovered, bool isSelected, bool isPinned) {
    const auto& th = s.currentTheme;
    float dp = dpiScale;
    SurfaceStyle style;
    style.radius = th.cardRadius * dp;
    switch (elevation) {
        case Elevation::Background: {
            style.fill = th.bg;
            style.border = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
            style.borderWidth = 0.0f;
            style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
            style.hasInnerHighlight = false;
            break;
        }
        case Elevation::Surface: {
            style.fill = D2D1::ColorF(th.cardBg.r, th.cardBg.g, th.cardBg.b, 0.95f);
            style.border = th.border;
            style.borderWidth = 1.0f;
            style.radius = th.cardRadius * dp;
            style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f);
            style.shadowBlur = 4.0f * dp;
            style.shadowOffsetY = 1.5f * dp;
            break;
        }
        case Elevation::Card: {
            if (isPinned) {
                style.fill = D2D1::ColorF(th.cardBg.r, th.cardBg.g, th.cardBg.b, 0.95f);
                style.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.20f : 0.12f);
                style.borderWidth = (isHovered || isSelected) ? 1.5f : 1.0f;
                style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, isHovered ? 0.20f : 0.10f);
                style.shadowBlur = (isHovered ? 8.0f : 4.0f) * dp;
                style.shadowOffsetY = (isHovered ? 2.5f : 1.5f) * dp;
                // [Disabled per user request] Pinned card gold hue glow effect
                // style.glowColor = ColorWithAlpha(th.accent, isHovered ? 0.16f : 0.08f);
                // style.glowBlur = (isHovered ? 16.0f : 10.0f) * dp;
                // style.hasGradient = true;
                // style.gradientStart = ColorWithAlpha(th.accent, 0.16f);
                // style.gradientEnd = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowBlur = 0.0f;
                style.hasGradient = false;
                style.leftAccentBarAlpha = 0.85f;
                style.leftAccentBarColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
                style.leftAccentBarWidth = 6.0f;
            } else {
                style.fill = isHovered ? D2D1::ColorF(th.cardBg.r * 1.05f, th.cardBg.g * 1.05f, th.cardBg.b * 1.05f, 1.0f) : th.cardBg;
                style.border = isSelected ? th.accent : (isHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f) : th.border);
                style.borderWidth = (isHovered || isSelected) ? 1.5f : 1.0f;
                style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, isHovered ? 0.18f : 0.06f);
                style.shadowBlur = (isHovered ? 6.0f : 3.0f) * dp;
                style.shadowOffsetY = (isHovered ? 2.0f : 1.0f) * dp;
                style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowBlur = 0.0f;
                style.leftAccentBarAlpha = 0.0f;
                style.leftAccentBarColor = th.accent;
                style.leftAccentBarWidth = 0.0f;
            }
            break;
        }
        case Elevation::RaisedCard: {
            if (isPinned) {
                style.fill = D2D1::ColorF(th.cardBg.r, th.cardBg.g, th.cardBg.b, 0.95f);
                style.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.22f : 0.14f);
                style.borderWidth = (isHovered || isSelected) ? 1.5f : 1.0f;
                style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.40f);
                style.shadowBlur = 18.0f * dp;
                style.shadowOffsetY = 4.0f * dp;
                // [Disabled per user request] Pinned card gold hue glow effect
                // style.glowColor = ColorWithAlpha(th.accent, 0.18f);
                // style.glowBlur = 16.0f * dp;
                // style.hasGradient = true;
                // style.gradientStart = ColorWithAlpha(th.accent, 0.18f);
                // style.gradientEnd = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowBlur = 0.0f;
                style.hasGradient = false;
                style.leftAccentBarAlpha = 0.85f;
                style.leftAccentBarColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
                style.leftAccentBarWidth = 6.0f;
            } else {
                style.fill = D2D1::ColorF(th.cardBg.r * 1.05f, th.cardBg.g * 1.05f, th.cardBg.b * 1.05f, 1.0f);
                style.border = isSelected ? th.accent : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
                style.borderWidth = (isHovered || isSelected) ? 1.5f : 1.0f;
                style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f);
                style.shadowBlur = 14.0f * dp;
                style.shadowOffsetY = 3.0f * dp;
                style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
                style.glowBlur = 0.0f;
                style.leftAccentBarAlpha = 0.0f;
                style.leftAccentBarColor = th.accent;
                style.leftAccentBarWidth = 0.0f;
            }
            break;
        }
        case Elevation::ExpandedCard: {
            style.fill = th.cardBg;
            style.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f);
            style.borderWidth = 1.0f;
            style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f);
            style.shadowBlur = 45.0f * dp;
            style.shadowOffsetY = 15.0f * dp;
            style.glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
            style.glowBlur = 0.0f;
            style.leftAccentBarAlpha = 0.0f;
            style.leftAccentBarColor = th.accent;
            style.leftAccentBarWidth = 4.0f;
            break;
        }
        case Elevation::Floating: {
            style.fill = D2D1::ColorF(0.07f, 0.08f, 0.11f, 0.96f);
            style.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
            style.borderWidth = 1.0f;
            style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f);
            style.shadowBlur = 28.0f * dp;
            style.shadowOffsetY = 8.0f * dp;
            break;
        }
        case Elevation::Modal: {
            style.fill = D2D1::ColorF(th.cardBg.r, th.cardBg.g, th.cardBg.b, 0.98f);
            style.border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f);
            style.borderWidth = 1.0f;
            style.shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f);
            style.shadowBlur = 36.0f * dp;
            style.shadowOffsetY = 12.0f * dp;
            break;
        }
    }
    return style;
}

void Renderer::DrawAmbientBackground(const D2D1_RECT_F&, const Settings& s) {
    if (!d2dContext) return;
    const auto& th = s.currentTheme;
    Clear(th.bg);
}

void Renderer::DrawSvgPath(const char* svgPath, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float viewBox, bool fill, float strokeWidth, bool cacheGeometry) {
    if (!d2dContext || !solidBrush || !d2dFactory || !svgPath) return;
    ComPtr<ID2D1PathGeometry> transientGeom;
    ID2D1PathGeometry* geom = nullptr;
    if (cacheGeometry) {
        auto it = svgGeometryCache.find(svgPath);
        if (it == svgGeometryCache.end()) {
            if (svgGeometryCache.size() >= 512) svgGeometryCache.clear();
            auto built = SvgGeometry::Build(d2dFactory.Get(), svgPath);
            if (!built) return;
            it = svgGeometryCache.emplace(svgPath, std::move(built)).first;
        }
        geom = it->second.Get();
    } else {
        transientGeom = SvgGeometry::Build(d2dFactory.Get(), svgPath);
        geom = transientGeom.Get();
    }
    if (!geom) return;
    solidBrush->SetColor(color);
    float destW = RectWidth(rect);
    float destH = RectHeight(rect);
    float scale = std::min(destW, destH) / viewBox;
    float offsetX = rect.left + (destW - viewBox * scale) * 0.5f;
    float offsetY = rect.top + (destH - viewBox * scale) * 0.5f;
    D2D1_MATRIX_3X2_F oldTransform;
    d2dContext->GetTransform(&oldTransform);
    D2D1_MATRIX_3X2_F newTransform = D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(offsetX, offsetY) * oldTransform;
    d2dContext->SetTransform(newTransform);
    if (fill) {
        d2dContext->FillGeometry(geom, solidBrush.Get());
    } else { d2dContext->DrawGeometry(geom, solidBrush.Get(), strokeWidth / scale); }
    d2dContext->SetTransform(oldTransform);
}

void Renderer::DrawIcon(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float strokeWidth) {
    if (icon == IconType::DragHandle) {
        float cx = (rect.left + rect.right) * 0.5f;
        float cy = (rect.top + rect.bottom) * 0.5f;
        float colSpacing = 2.4f * dpiScale, rowSpacing = 4.2f * dpiScale, dotRadius = 1.6f * dpiScale;
        solidBrush->SetColor(color);
        float colX[2] = { cx - colSpacing, cx + colSpacing };
        float rowY[3] = { cy - rowSpacing, cy, cy + rowSpacing };
        for (int col = 0; col < 2; ++col) {
            for (int row = 0; row < 3; ++row) {
                d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(colX[col], rowY[row]), dotRadius, dotRadius), solidBrush.Get());
            }
        }
        return;
    }
    if (const auto* data = FindIconPath(icon))
        DrawSvgPath(data->path, rect, color, data->viewBox, data->fill, data->fill ? 1.4f : strokeWidth);
}

void Renderer::DrawIconRotated(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float angleDegrees, float strokeWidth) {
    if (!d2dContext) return;
    if (std::abs(angleDegrees) < 0.01f) { DrawIcon(icon, rect, color, strokeWidth); return; }
    D2D1_MATRIX_3X2_F oldTransform;
    d2dContext->GetTransform(&oldTransform);
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;
    D2D1_MATRIX_3X2_F rotMatrix = D2D1::Matrix3x2F::Rotation(angleDegrees, D2D1::Point2F(cx, cy)) * oldTransform;
    d2dContext->SetTransform(rotMatrix);
    DrawIcon(icon, rect, color, strokeWidth);
    d2dContext->SetTransform(oldTransform);
}


IWICImagingFactory* Renderer::EnsureWicFactory() {
    if (!wicFactory && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)))) return nullptr;
    return wicFactory.Get();
}

uint64_t Renderer::EstimateThumbnailRequestBytes(const ThumbnailKey& key) {
    uint64_t w = key.width ? key.width : 320, h = key.height ? key.height : 180, bytes = w * h * 4ull;
    if (bytes == 0) bytes = 320ull * 180ull * 4ull;
    return bytes;
}

void Renderer::EnsureThumbnailWorkers() {
    std::lock_guard<std::mutex> lock(thumbnailMutex);
    if (thumbnailWorkerStopping) return;
    const size_t depth = pendingThumbnailRequests.size();
    for (size_t i = 0; i < thumbnailWorkers.size(); ++i) {
        if (depth >= i * 4 && !thumbnailWorkers[i].joinable()) {
            thumbnailWorkerStopping = false;
            activeThumbnailWorkers.fetch_add(1, std::memory_order_acq_rel);
            try { thumbnailWorkers[i] = std::thread(&Renderer::ThumbnailWorkerThread, this); }
            catch (...) { activeThumbnailWorkers.fetch_sub(1, std::memory_order_acq_rel); break; }
        }
    }
}

void Renderer::ThumbnailWorkerThread() {
    struct CompletionGuard {
        std::atomic<size_t>& count;
        ~CompletionGuard() { count.fetch_sub(1, std::memory_order_release); }
    } completed{ activeThumbnailWorkers };
    ThumbnailRequest req;
    try {
        uint64_t activeGeneration = 0;
        MediaProbeIsolation probes([this, &activeGeneration]() {
            std::lock_guard<std::mutex> lock(thumbnailMutex);
            return thumbnailWorkerStopping || activeGeneration != thumbnailGeneration;
        });
        while (true) {
            {
                std::unique_lock<std::mutex> lock(thumbnailMutex);
                if (probes.HelperProcessId()) {
                    bool releaseProbe = activeGeneration != thumbnailGeneration;
                    if (!releaseProbe && pendingThumbnailRequests.empty()) {
                        const uint64_t idleGeneration = thumbnailGeneration;
                        const bool woke = thumbnailCv.wait_for(lock, std::chrono::seconds(5), [&]() {
                            return thumbnailWorkerStopping || !pendingThumbnailRequests.empty() ||
                                idleGeneration != thumbnailGeneration;
                        });
                        releaseProbe = !woke || idleGeneration != thumbnailGeneration;
                    }
                    if (releaseProbe) {
                        // Stop may wait for the helper. Keep renderer queue and
                        // persistence locks free while releasing its mapping.
                        lock.unlock();
                        probes.ReleaseIdleResources();
                        lock.lock();
                    }
                }
                thumbnailCv.wait(lock, [this]() { return thumbnailWorkerStopping || !pendingThumbnailRequests.empty(); });
                if (thumbnailWorkerStopping) { break; }
                if (pendingThumbnailRequests.empty()) { continue; }
                auto best = pendingThumbnailRequests.begin();
                for (auto it = std::next(best); it != pendingThumbnailRequests.end(); ++it) {
                    if (it->priorityFrame > best->priorityFrame ||
                        (it->priorityFrame == best->priorityFrame && it->priorityOrder < best->priorityOrder)) {
                        best = it;
                    }
                }
                req = std::move(*best);
                activeGeneration = req.generation;
                pendingThumbnailRequests.erase(best);
            }
            ThumbnailResult result;
            result.key = req.key;
            result.generation = req.generation;
            result.success = false;
            // Tier 1: our own persistent hub disk tier (plain file read, no COM). Shell hits are persisted here after first decode, so restarts
            // and repeat scrolls never touch the shell for known files.
            PreviewThumbnailFrame diskFrame;
            if (LoadHubThumbnailFromDiskCache(req.key.path, (int)req.key.width, (int)req.key.height, diskFrame, GetExeDir(), !thumbnailPrivateMode.load())) {
                if (diskFrame.width > 0 && diskFrame.height > 0 && !diskFrame.bgraPixels.empty() &&
                    diskFrame.bgraPixels.size() == (size_t)(uint32_t)diskFrame.width * (uint32_t)diskFrame.height * 4u) {
                    result.pixels = std::move(diskFrame.bgraPixels);
                    result.pixelWidth = (UINT)diskFrame.width;
                    result.pixelHeight = (UINT)diskFrame.height;
                    result.success = true;
                }
            }
            // Tier 2: preserve Shell cache lookup, provider extraction and WIC
            // cubic downscaling, but isolate the provider in a disposable process.
            // The cancellation poll kills only that process; this Renderer-owning
            // thread always unwinds its normal C++ lifetime.
            if (!result.success && probes.GetThumbnail(req.key.path, req.key.width, req.key.height,
                    result.pixels, result.pixelWidth, result.pixelHeight)) {
                result.success = true;
                if (!thumbnailPrivateMode.load()) {
                    PreviewThumbnailFrame persist;
                    persist.filePath = req.key.path;
                    persist.timestampSec = 0.0;
                    persist.width = static_cast<int>(result.pixelWidth);
                    persist.height = static_cast<int>(result.pixelHeight);
                    persist.bgraPixels = result.pixels;
                    std::lock_guard<std::mutex> persistenceLock(thumbnailPersistenceMutex);
                    bool requestIsCurrent = false;
                    {
                        std::lock_guard<std::mutex> lock(thumbnailMutex);
                        requestIsCurrent = !thumbnailWorkerStopping && req.generation == thumbnailGeneration;
                    }
                    if (requestIsCurrent && !thumbnailPrivateMode.load()) SaveHubThumbnailToDiskCache(persist);
                }
            }
            HWND notifyHwnd = NULL;
            {
                std::unique_lock<std::mutex> lock(thumbnailMutex);
                thumbnailCv.wait(lock, [&]() {
                    return thumbnailWorkerStopping || req.generation != thumbnailGeneration ||
                        (completedThumbnailResults.size() < kMaxCompletedThumbnailResults &&
                         completedThumbnailBytes + result.pixels.size() <= kMaxCompletedThumbnailBytes);
                });
                if (!thumbnailWorkerStopping && req.generation == thumbnailGeneration) {
                    completedThumbnailBytes += result.pixels.size();
                    completedThumbnailResults.push_back(std::move(result));
                    notifyHwnd = hWnd;
                }
            }
            if (notifyHwnd && IsWindow(notifyHwnd) && !thumbnailNotificationPosted.exchange(true, std::memory_order_acq_rel)) {
                if (!PostMessageW(notifyHwnd, kThumbnailReadyMessage, 0, 0))
                    thumbnailNotificationPosted.store(false, std::memory_order_release);
            }
        }
    } catch (...) {
        // Resource exhaustion discards this worker's current request. Other
        // workers remain usable, and close still observes a completed owner.
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        inFlightThumbnailKeys.erase(req.key);
    }
}


void Renderer::EnqueueThumbnailRequest(const ThumbnailKey& key, bool lowPriority) {
    // Demand-driven startup: the first queued request creates the first
    // worker; pressure scales to a second. No workers exist for an idle hub.
    EnsureThumbnailWorkers();
    std::unique_lock<std::mutex> lock(thumbnailMutex);
    if (thumbnailWorkerStopping) return;
    uint64_t priorityFrame = thumbnailRequestFrame;
    // Overscan prefetch sits one frame below visible demand so the worker's best-pick always serves on-screen cards first, and queue
    // pressure evicts prefetch entries before visible ones.
    if (lowPriority && priorityFrame > 0) --priorityFrame;
    const uint64_t priorityOrder = thumbnailRequestOrder++;
    for (auto& pending : pendingThumbnailRequests) {
        if (pending.key == key) {
            // Same-frame re-requests (one per rendered frame per card) carry
            // no new information; skip the wakeup to avoid a notify storm
            // while a card stays visible but undecodable yet.
            if (pending.priorityFrame == priorityFrame) return;
            pending.priorityFrame = priorityFrame;
            pending.priorityOrder = priorityOrder;
            thumbnailCv.notify_one();
            return;
        }
    }
    // A key absent from the pending queue but present in the set is already
    // decoding. Let that work finish instead of launching a duplicate.
    if (inFlightThumbnailKeys.find(key) != inFlightThumbnailKeys.end()) return;
    if (pendingThumbnailRequests.size() >= kMaxPendingThumbnailRequests) {
        auto worst = pendingThumbnailRequests.begin();
        for (auto it = std::next(worst); it != pendingThumbnailRequests.end(); ++it) {
            if (it->priorityFrame < worst->priorityFrame ||
                (it->priorityFrame == worst->priorityFrame && it->priorityOrder > worst->priorityOrder)) {
                worst = it;
            }
        }
        const bool newRequestHasPriority = priorityFrame > worst->priorityFrame ||
            (priorityFrame == worst->priorityFrame && priorityOrder < worst->priorityOrder);
        if (!newRequestHasPriority) return;
        inFlightThumbnailKeys.erase(worst->key);
        pendingThumbnailRequests.erase(worst);
    }
    // Byte-bound in addition to the count bound: estimate pending bytes and evict lowest-priority entries until the new request fits.
    {
        uint64_t pendingBytes = 0;
        for (const auto& pending : pendingThumbnailRequests) { pendingBytes += EstimateThumbnailRequestBytes(pending.key); }
        uint64_t newBytes = EstimateThumbnailRequestBytes(key);
        while (!pendingThumbnailRequests.empty() && pendingBytes + newBytes > kMaxPendingThumbnailBytes) {
            auto worst = pendingThumbnailRequests.begin();
            for (auto it = std::next(worst); it != pendingThumbnailRequests.end(); ++it) {
                if (it->priorityFrame < worst->priorityFrame ||
                    (it->priorityFrame == worst->priorityFrame && it->priorityOrder > worst->priorityOrder)) {
                    worst = it;
                }
            }
            const bool newRequestHasPriority = priorityFrame > worst->priorityFrame ||
                (priorityFrame == worst->priorityFrame && priorityOrder < worst->priorityOrder);
            if (!newRequestHasPriority) return;
            pendingBytes -= EstimateThumbnailRequestBytes(worst->key);
            inFlightThumbnailKeys.erase(worst->key);
            pendingThumbnailRequests.erase(worst);
        }
    }
    inFlightThumbnailKeys.insert(key);
    pendingThumbnailRequests.push_back({ key, thumbnailGeneration, priorityFrame, priorityOrder });
    // Recheck worker demand when the queue reaches the existing eight-request trigger.
    bool needSecond = pendingThumbnailRequests.size() >= 8 && !thumbnailWorkers[1].joinable();
    thumbnailCv.notify_one();
    if (needSecond) { lock.unlock(); EnsureThumbnailWorkers(); }
}

std::vector<Renderer::ThumbnailResult> Renderer::TakeCompletedThumbnailBatch() {
    std::vector<ThumbnailResult> results;
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        // Keep visible misses ahead of old offscreen work after a fast scroll.
        std::stable_partition(completedThumbnailResults.begin(), completedThumbnailResults.end(),
            [this](const ThumbnailResult& result) {
                auto demand = thumbnailDemandFrames.find(result.key);
                return demand != thumbnailDemandFrames.end() && thumbnailRequestFrame - demand->second <= 1;
            });
        uint64_t bytes = 0;
        size_t count = 0;
        while (count < completedThumbnailResults.size() && count < kThumbnailUploadsPerFrame) {
            const uint64_t nextBytes = completedThumbnailResults[count].pixels.size();
            // Always allow one result to make progress, even if its size alone exceeds the budget.
            if (count > 0 && bytes + nextBytes > kThumbnailUploadBytesPerFrame) break;
            bytes += nextBytes;
            ++count;
        }
        results.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            inFlightThumbnailKeys.erase(completedThumbnailResults[i].key);
            results.push_back(std::move(completedThumbnailResults[i]));
        }
        completedThumbnailResults.erase(completedThumbnailResults.begin(), completedThumbnailResults.begin() + count);
        completedThumbnailBytes -= bytes;
        deferredThumbnailUploads = !completedThumbnailResults.empty();
    }
    if (!results.empty()) thumbnailCv.notify_all();
    return results;
}

void Renderer::DrainCompletedThumbnails() {
    if (!d2dContext) return;
    auto results = TakeCompletedThumbnailBatch();
    for (auto& res : results) {
        if (res.generation != thumbnailGeneration) { continue; }
        auto it = thumbnailCache.find(res.key);
        if (it != thumbnailCache.end()) { continue; }
        if (res.success && res.pixelWidth > 0 && res.pixelHeight > 0 && !res.pixels.empty()) {
            D2D1_BITMAP_PROPERTIES props;
            props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            props.dpiX = 96.0f;
            props.dpiY = 96.0f;
            ComPtr<ID2D1Bitmap> d2dBitmap;
            HRESULT hr = d2dContext->CreateBitmap( D2D1::SizeU(res.pixelWidth, res.pixelHeight), res.pixels.data(), res.pixelWidth * 4,
                &props, &d2dBitmap );
            if (SUCCEEDED(hr) && d2dBitmap) {
                const uint64_t approximateBytes = static_cast<uint64_t>(std::max<UINT>(1, res.pixelWidth)) *
                    static_cast<uint64_t>(std::max<UINT>(1, res.pixelHeight)) * 4ull;
                if (approximateBytes <= kThumbnailCacheByteLimit) {
                    if (!TrimThumbnailCache(approximateBytes)) continue;
                    thumbnailLru.push_front(res.key);
                    ThumbnailEntry entry;
                    entry.bitmap = d2dBitmap;
                    entry.approximateBytes = approximateBytes;
                    entry.lruIt = thumbnailLru.begin();
                    auto demand = thumbnailDemandFrames.find(res.key);
                    entry.lastDrawnFrame = demand == thumbnailDemandFrames.end() ? 0 : demand->second;
                    auto inserted = thumbnailCache.emplace(res.key, std::move(entry));
                    if (inserted.second) {
                        thumbnailCacheBytes += approximateBytes;
                        IndexThumbnailBitmap(inserted.first);
                    } else { thumbnailLru.pop_front(); TouchThumbnail(inserted.first); }
                    continue;
                }
            }
        }
        // Permanent decode failure: quarantine separately so files the shell cannot thumbnail never occupy bitmap LRU slots nor get
        // re-enqueued every frame. The placeholder icon stays stable instead of flashing through repeated decode attempts.
        if (negativeCache.find(res.key) == negativeCache.end()) {
            while (negativeLru.size() >= kNegativeCacheEntryLimit && !negativeLru.empty()) {
                negativeCache.erase(negativeLru.back());
                negativeInfo.erase(negativeLru.back());
                negativeLru.pop_back();
            }
            int attempts = 0;
            auto attIt = negativeAttempts.find(res.key.path);
            if (attIt != negativeAttempts.end()) attempts = attIt->second;
            NegativeInfo info;
            const uint64_t delay = ThumbRetryDelayMs(attempts);
            info.retryTickMs = (delay == UINT64_MAX) ? UINT64_MAX : GetTickCount64() + delay;
            negativeLru.push_front(res.key);
            negativeCache.insert(res.key);
            negativeInfo.emplace(res.key, info);
        }
    }
}

ComPtr<ID2D1Bitmap> Renderer::GetOrCreateVideoThumbnail(const std::wstring& filePath, UINT reqW, UINT reqH, double startSec) {
    if (filePath.empty() || !d2dContext) return nullptr;
    // Single per-item static-thumbnail resolution point: a registered cover art
    // image replaces the extracted thumbnail for every consumer (media cards,
    // playlist resume card, playlist rows). A stale registration (deleted or
    // missing file) falls back to the normal thumbnail pipeline.
    if (!coverArtByMediaPath.empty()) {
        const auto coverIt = coverArtByMediaPath.find(filePath);
        if (coverIt != coverArtByMediaPath.end()) {
            if (FileExists(coverIt->second)) return GetOrCreateVideoThumbnailInternal(coverIt->second, reqW, reqH, 0);
            coverArtByMediaPath.erase(coverIt);
        }
    }
    return GetOrCreateVideoThumbnailInternal(filePath, reqW, reqH, ThumbnailPositionBucket(startSec));
}

ComPtr<ID2D1Bitmap> Renderer::GetOrCreateVideoThumbnailInternal(const std::wstring& filePath, UINT reqW, UINT reqH, uint32_t positionBucket) {
    // Canonicalize to one bucket per usage class. Hub cards historically requested 360x204 / 240x135 / 180x100 for the same file (view-mode
    // toggle = full re-decode storm); all now share a single 320x180 entry
    // that DrawBitmapRounded scales losslessly at draw time. Large requests (backdrop art) share 640x360.
    if (reqW <= 400 && reqH <= 240) { reqW = 320; reqH = 180; } else { reqW = 640; reqH = 360; }
    // Copy-free probes: this runs once per visible card per rendered frame.
    const ThumbnailKeyProbe key{ &filePath, reqW, reqH, positionBucket };
    auto it = thumbnailCache.find(key);
    if (it != thumbnailCache.end()) { it->second.lastDrawnFrame = thumbnailRequestFrame; TouchThumbnail(it); return it->second.bitmap; }
    // Shared key fallback: if looking for 640x360, check if 320x180 is already cached.
    // Reusing the 320x180 bitmap avoids redundant decodes and memory overhead.
    if (reqW == 640 && reqH == 360) {
        const ThumbnailKeyProbe key320{ &filePath, 320, 180, positionBucket };
        auto it320 = thumbnailCache.find(key320);
        if (it320 != thumbnailCache.end()) {
            it320->second.lastDrawnFrame = thumbnailRequestFrame;
            TouchThumbnail(it320);
            return it320->second.bitmap;
        }
    }
    if (positionBucket != 0) {
        // Pinned frame: queue for the mpv extractor (deduped; the extractor
        // serves the shared 320x180 bucket for both card and backdrop sizes).
        for (const auto& pending : pinnedExtractRequests) {
            if (pending.path == filePath && pending.positionBucket == positionBucket) return nullptr;
        }
        if (thumbnailCache.find(ThumbnailKeyProbe{ &filePath, 320, 180, positionBucket }) != thumbnailCache.end()) return nullptr;
        pinnedExtractRequests.push_back(ThumbnailKey{ filePath, 320, 180, positionBucket });
        return nullptr;
    }
    // Demand is only meaningful for an actual miss: recording it on every cache
    // hit cost a map probe + path copy per card per frame.
    thumbnailDemandFrames[ThumbnailKey{ filePath, reqW, reqH }] = thumbnailRequestFrame;
    // Quarantined failure: stable placeholder without a re-request storm, but NOT permanent — the Explorer cache warms asynchronously, so
    // each miss only suppresses retries until its backoff deadline, then the shell gets another chance (mpv extraction runs independently).
    auto negIt = negativeCache.find(key);
    if (negIt != negativeCache.end()) {
        const uint64_t now = GetTickCount64();
        auto infoIt = negativeInfo.find(key);
        const uint64_t retryTick = (infoIt != negativeInfo.end()) ? infoIt->second.retryTickMs : 0;
        if (now < retryTick) return nullptr;
        int attempts = 0;
        auto attIt = negativeAttempts.find(filePath);
        if (attIt != negativeAttempts.end()) attempts = attIt->second;
        negativeAttempts[filePath] = attempts + 1;
        ThumbnailKey missKey{ filePath, negIt->width, negIt->height };
        negativeCache.erase(negIt);
        negativeLru.remove(missKey);
        negativeInfo.erase(missKey);
    }
    EnqueueThumbnailRequest(ThumbnailKey{ filePath, reqW, reqH });
    return nullptr;
}

void Renderer::SetCoverArtPath(const std::wstring& mediaPath, const std::wstring& coverPath) {
    if (mediaPath.empty()) return;
    // Re-picks overwrite the stored file under the same name, and the previous
    // image may live under a different extension: evict both cached images.
    const auto prev = coverArtByMediaPath.find(mediaPath);
    if (prev != coverArtByMediaPath.end() && prev->second != coverPath) EvictThumbnailForPath(prev->second);
    coverArtByMediaPath[mediaPath] = coverPath;
    EvictThumbnailForPath(coverPath);
}

void Renderer::ClearCoverArtPath(const std::wstring& mediaPath) {
    const auto it = coverArtByMediaPath.find(mediaPath);
    if (it == coverArtByMediaPath.end()) return;
    EvictThumbnailForPath(it->second);
    coverArtByMediaPath.erase(it);
}

void Renderer::EvictThumbnailForPath(const std::wstring& path) {
    if (path.empty()) return;
    // ThumbnailKey orders by path first, so every bucket for this path is a
    // contiguous map range (width 0 probe = first entry with the path).
    const ThumbnailKeyProbe probe{ &path, 0, 0 };
    for (auto it = thumbnailCache.lower_bound(probe); it != thumbnailCache.end() && it->first.path == path;) {
        thumbnailCacheBytes -= std::min(thumbnailCacheBytes, it->second.approximateBytes);
        thumbnailByBitmap.erase(it->second.bitmap.Get());
        thumbnailLru.erase(it->second.lruIt);
        it = thumbnailCache.erase(it);
    }
    for (auto it = thumbnailDemandFrames.lower_bound(probe); it != thumbnailDemandFrames.end() && it->first.path == path;) {
        it = thumbnailDemandFrames.erase(it);
    }
    for (auto it = negativeCache.lower_bound(probe); it != negativeCache.end() && it->path == path;) {
        negativeLru.remove(*it);
        negativeInfo.erase(*it);
        it = negativeCache.erase(it);
    }
    negativeAttempts.erase(path);
    for (const ThumbnailKey key : std::array<ThumbnailKey, 2>{ ThumbnailKey{ path, 320, 180 }, ThumbnailKey{ path, 640, 360 } }) {
        inFlightThumbnailKeys.erase(key);
    }
    // The persistent hub disk tier is keyed by path only; drop the stored image
    // so an overwritten cover file is not served from disk on the next decode.
    DeleteFileW((GetCacheDir(CacheClass::Thumbnails) + L"\\" + MakeHubThumbnailCacheKey(path, 0, 0)).c_str());
}

void Renderer::PrefetchThumbnails(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        const ThumbnailKeyProbe probe{ &path, 320, 180 };
        if (thumbnailCache.find(probe) != thumbnailCache.end()) continue;
        if (negativeCache.find(probe) != negativeCache.end()) continue;
        EnqueueThumbnailRequest(ThumbnailKey{ path, 320, 180 }, true);
    }
}

bool Renderer::DequeuePinnedExtractCandidate(std::wstring& outPath, uint32_t& outBucket) {
    auto it = pinnedExtractRequests.begin();
    while (it != pinnedExtractRequests.end()) {
        // Skip entries a previous extraction already satisfied.
        if (thumbnailCache.find(ThumbnailKeyProbe{ &it->path, 320, 180, it->positionBucket }) != thumbnailCache.end()) {
            it = pinnedExtractRequests.erase(it);
            continue;
        }
        outPath = it->path;
        outBucket = it->positionBucket;
        pinnedExtractRequests.erase(it);
        return true;
    }
    return false;
}

bool Renderer::DequeueHubExtractCandidate(std::wstring& outPath) {
    // Oldest shell miss first; each path is offered once per generation so a
    // file mpv cannot open never loops through the extractor.
    for (auto it = negativeLru.rbegin(); it != negativeLru.rend(); ++it) {
        if (hubExtractAttempted.find(it->path) == hubExtractAttempted.end()) {
            hubExtractAttempted.insert(it->path);
            outPath = it->path;
            return true;
        }
    }
    return false;
}

bool Renderer::DequeueHubExtractCandidate(const std::vector<std::wstring>& priorityPaths, std::wstring& outPath) {
    // Caller order wins (visible top-down): the first quarantined, unattempted path is offered. Falls back to oldest-first below.
    // Copy-free probes: the idle hub calls this every frame per extractor.
    for (const auto& path : priorityPaths) {
        if (path.empty()) continue;
        if (negativeCache.find(ThumbnailKeyProbe{ &path, 320, 180 }) == negativeCache.end()) continue;
        if (hubExtractAttempted.find(path) != hubExtractAttempted.end()) continue;
        hubExtractAttempted.insert(path);
        outPath = path;
        return true;
    }
    return DequeueHubExtractCandidate(outPath);
}

bool Renderer::InjectThumbnail(const std::wstring& filePath, UINT w, UINT h, const std::vector<uint8_t>& pixels, uint32_t positionBucket) {
    if (filePath.empty() || !d2dContext || w == 0 || h == 0 || w > 1920 || h > 1080) return false;
    if (pixels.size() != (size_t)w * h * 4u) return false;
    ThumbnailKey key{ filePath, w, h, positionBucket };
    if (thumbnailCache.find(key) != thumbnailCache.end()) return true;
    D2D1_BITMAP_PROPERTIES props;
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap> d2dBitmap;
    if (FAILED(d2dContext->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, &props, &d2dBitmap)) || !d2dBitmap) { return false; }
    const uint64_t approximateBytes = (uint64_t)w * h * 4ull;
    if (!TrimThumbnailCache(approximateBytes)) return false;
    thumbnailLru.push_front(key);
    ThumbnailEntry entry;
    entry.bitmap = d2dBitmap;
    entry.approximateBytes = approximateBytes;
    entry.lruIt = thumbnailLru.begin();
    auto demand = thumbnailDemandFrames.find(key);
    entry.lastDrawnFrame = demand == thumbnailDemandFrames.end() ? 0 : demand->second;
    auto inserted = thumbnailCache.emplace(key, std::move(entry));
    if (!inserted.second) { thumbnailLru.pop_front(); return true; }
    thumbnailCacheBytes += approximateBytes;
    IndexThumbnailBitmap(inserted.first);
    // Promote out of the failure quarantine so future frames hit memory.
    negativeCache.erase(key);
    negativeLru.remove(key);
    negativeInfo.erase(key);
    return true;
}

bool Renderer::CaptureBackBufferToPng(const std::wstring& outputPath) {
    if (!swapChain || !d3dDevice || !d3dContext) return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr) || !stagingTexture) return false;
    d3dContext->CopyResource(stagingTexture.Get(), backBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3dContext->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) return false;
    IWICImagingFactory* wic = EnsureWicFactory();
    if (!wic) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    size_t lastSlash = outputPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        std::wstring parentDir = outputPath.substr(0, lastSlash);
        SHCreateDirectoryExW(NULL, parentDir.c_str(), NULL);
    }
    ComPtr<IWICStream> stream;
    hr = wic->CreateStream(&stream);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> propBag;
    hr = encoder->CreateNewFrame(&frame, &propBag);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    hr = frame->Initialize(propBag.Get());
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    hr = frame->SetSize(desc.Width, desc.Height);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) { d3dContext->Unmap(stagingTexture.Get(), 0); return false; }
    hr = frame->WritePixels(desc.Height, mapped.RowPitch, mapped.RowPitch * desc.Height, static_cast<BYTE*>(mapped.pData));
    d3dContext->Unmap(stagingTexture.Get(), 0);
    if (FAILED(hr)) return false;
    hr = frame->Commit();
    if (FAILED(hr)) return false;
    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

} // namespace Aperture
