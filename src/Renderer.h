#pragma once

#include "Common.h"
#include "IconPaths.h"
#include "Settings.h"
#include "FontResources.h"
#include "DrawingPrimitives.h"
#include <string>
#include <memory>
#include <map>
#include <list>
#include <deque>
#include <set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <thread>
#include <array>
#include <unordered_map>
namespace Aperture {

enum class Elevation {
    Background,    // 0: Page canvas (#140e0a or theme bg)
    Surface,       // 1: Rail / Content pane background (0-2px perceived depth)
    Card,          // 2: Media item / folder row (soft diffuse shadow: 0 4px 16px rgba(0,0,0,0.4), lighter bg: cardBg)
    RaisedCard,    // 3: Hovered card / Pinned card (translateY(-1.5px), 0 10px 28px shadow, accent glow)
    ExpandedCard,  // 4: Hover-expanded card (230px, translateY(-3px), 0 20px 50px shadow, 0 0 24px accent glow)
    Floating,      // 5: Context menu / Tooltip / Settings flyout (0 12px 36px shadow, translucent bg with border)
    Modal          // 6: Dialog / Backdrop dim + elevated dialog box
};

struct SurfaceStyle {
    D2D1_COLOR_F fill = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    D2D1_COLOR_F border = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    float borderWidth = 1.0f, radius = 10.0f;
    // Lighting & Material Depth
    D2D1_COLOR_F innerHighlight = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
    D2D1_COLOR_F gradientStart = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    D2D1_COLOR_F gradientEnd = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    bool hasGradient = false, hasInnerHighlight = true;
    // Left Accent Bar (Seamlessly clipped inside rounded card geometry)
    float leftAccentBarAlpha = 0.0f;
    D2D1_COLOR_F leftAccentBarColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    float leftAccentBarWidth = 6.0f;
    // Shadows & Glow
    D2D1_COLOR_F shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.35f);
    float shadowBlur = 16.0f, shadowOffsetY = 4.0f;
    D2D1_COLOR_F glowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f);
    float glowBlur = 0.0f;
    // Optional Backdrop Art & Live Video Frame
    std::wstring backdropFile = L"";
    float backdropOpacity = 0.0f;
    ID2D1Bitmap* pLivePreviewBitmap = nullptr;
    float livePreviewOpacity = 0.0f;
};

class Renderer : private FontResources, private DrawingPrimitives<Renderer> {
public:
    Renderer();
    ~Renderer();
    static void PrewarmD3DDeviceAsync();
    static void ShutdownPrewarm();
    bool Initialize(HWND hWnd, const std::string& initialFont = "", bool prewarmDevice = true);
    // Playback overlays need the factories/fonts, but not a second GPU device.
    // Hub painting and thumbnail intent can create that device on demand.
    bool EnsureGraphicsContext();
    void Shutdown();
    // Close first requests cancellation, keeps the message loop responsive,
    // and joins only after these App-owned workers have finished.
    void CancelThumbnailWork();
    bool AreThumbnailWorkersRunning() const { return activeThumbnailWorkers.load(std::memory_order_acquire) != 0; }
    bool AreDeviceWorkersRunning() const;
    void Resize(UINT width, UINT height);
    void SetDpi(float dpi);
    void BeginDraw();
    bool EndDraw(bool present = true);
    ID2D1DeviceContext* GetD2DContext() const { return d2dContext.Get(); }
    IDWriteFactory* GetDWriteFactory() const { return dwriteFactory.Get(); }
    ID2D1Factory1* GetD2DFactory() const { return d2dFactory.Get(); }
    // Shared with OverlayRenderer so the private TTFs are parsed once.
    IDWriteFontCollection1* GetCustomFontCollection() const { return customFontCollection.Get(); }
    // True once the swap chain has presented at least one complete frame.
    bool HasPresentedFrame() const { return hasPresentedFrame; }
    HANDLE GetFrameLatencyWaitableObject() const { return frameLatencyWaitableObject; }
    bool IsOccluded() const { return isOccluded; }
    float GetDpiScale() const { return dpiScale; }
    float Scale(float val) const { return val * dpiScale; }
    void SetPrimitiveSnapping(bool enabled) { primitiveSnapping = enabled; }
    bool IsPrimitiveSnapping() const { return primitiveSnapping; }
    struct ScopedPrimitiveSnapping {
        Renderer& r;
        bool prev;
        ScopedPrimitiveSnapping(Renderer& renderer, bool enabled) : r(renderer), prev(renderer.IsPrimitiveSnapping()) {
            r.SetPrimitiveSnapping(enabled);
        }
        ~ScopedPrimitiveSnapping() { r.SetPrimitiveSnapping(prev); }
    };
    // Primitives
    using DrawingPrimitives::ApplyTransform;
    using DrawingPrimitives::FillPolygon;
    void Clear(const D2D1_COLOR_F& color);
    using DrawingPrimitives::FillRect;
    void FillGradientRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor, bool vertical = true);
    using DrawingPrimitives::FillRoundedRect;
    using DrawingPrimitives::FillRoundedGradientRect;
    using DrawingPrimitives::DrawRoundedRect;
    using DrawingPrimitives::FillCircle;
    using DrawingPrimitives::DrawCircle;
    using DrawingPrimitives::DrawLine;
    void DrawDashedRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f);
    void DrawRingProgress(float cx, float cy, float radius, float thickness, float progress, const D2D1_COLOR_F& trackCol, const D2D1_COLOR_F& accentCol);
    // 5-Primitive Real Rendering Engine
    void DrawSurface(const D2D1_RECT_F& rect, const SurfaceStyle& style);
    void DrawDropShadow(const D2D1_RECT_F& rect, float radius, float blur, float yOffset, const D2D1_COLOR_F& color);
    void DrawGlow(const D2D1_RECT_F& rect, float radius, float blur, const D2D1_COLOR_F& glowColor);
    using DrawingPrimitives::DrawInnerHighlight;
    void CreateFonts(const std::wstring& customFamily = L"");
    void UpdateFontFamily(const std::string& familyName);
    void SetTextScale(float scale);
    SurfaceStyle CreateSurfaceStyle(Elevation elevation, const Settings& s, bool isHovered = false, bool isSelected = false, bool isPinned = false);
    // Text & Badges
    void DrawString(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void DrawStringWrapped(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    void DrawStringWithShadow(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, const D2D1_COLOR_F& shadowColor, FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, float shadowOffsetY = 1.0f);
    // Text at an arbitrary pixel size (dp units), for live previews that must
    // track a slider continuously instead of stepping through font roles.
    void DrawStringSized(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float sizeDp,
                         DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
                         DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    using FontResources::MeasureStringSized;
    D2D1_SIZE_F MeasureString(const std::wstring& text, FontType font, float maxW = 10000.0f, float maxH = 10000.0f);
    float MeasureStringWrappedHeight(const std::wstring& text, FontType font, float maxWidth);
    D2D1_SIZE_F GetRenderTargetSize() const { return d2dContext ? d2dContext->GetSize() : D2D1::SizeF(0, 0); }
    void DrawProgressBar(const D2D1_RECT_F& rect, double progress, const D2D1_COLOR_F& trackCol, const D2D1_COLOR_F& fillCol, float radius = 3.0f);
    // Vector Icons
    void DrawIcon(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float strokeWidth = 1.6f);
    void DrawIconRotated(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float angleDegrees, float strokeWidth = 1.6f);
    void DrawSvgPath(const char* svgPath, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float viewBox = 24.0f, bool fill = true, float strokeWidth = 1.4f);
    // Shadows & Effects
    void DrawCardShadow(const D2D1_RECT_F& rect, float radius, float elevation = 4.0f);
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();
    // Thumbnails & Backdrop Art Requests canonicalize internally (hub -> 320x180, backdrop -> 640x360).
    ComPtr<ID2D1Bitmap> GetOrCreateVideoThumbnail(const std::wstring& filePath, UINT reqW = 320, UINT reqH = 180);
    // Private sessions read (but never write) the persistent hub disk tier.
    void SetThumbnailPrivateMode(bool enabled) { thumbnailPrivateMode.store(enabled); }
    // Pops the oldest shell-miss path not yet offered for mpv extraction. UI thread only (App idle driver).
    bool DequeueHubExtractCandidate(std::wstring& outPath);
    // Priority variant: first path (in caller order, e.g. visible top-down)
    // that is quarantined and unattempted wins; falls back to oldest-first.
    bool DequeueHubExtractCandidate(const std::vector<std::wstring>& priorityPaths, std::wstring& outPath);
    // Releases a path back to the extraction pool (used when an in-flight extraction is cancelled because the user scrolled away).
    // Low-priority overscan: enqueue shell/disk requests for paths that are near (but not) visible so scrolling reveals warm entries. Never
    // enqueues already-cached or freshly-failed keys.
    void PrefetchThumbnails(const std::vector<std::wstring>& paths);
    // Inserts freshly extracted pixels (mpv fallback) directly into the memory cache. UI thread only (creates a device bitmap).
    bool InjectThumbnail(const std::wstring& filePath, UINT width, UINT height, const std::vector<uint8_t>& pixels);
    size_t ClearThumbnailCache();
    // Reclaim hidden hub resources without deleting the portable disk cache.
    void TrimHiddenLibraryCaches(uint64_t retainedThumbnailBytes = 8ull * 1024 * 1024, bool releaseDevice = false);
    static constexpr UINT kThumbnailReadyMessage = WM_APP + 0x50;
    void AcknowledgeThumbnailNotification() { thumbnailNotificationPosted.store(false, std::memory_order_release); }
    bool HasDeferredThumbnailUploads() const { return deferredThumbnailUploads; }
    void DrawBitmapRounded(const D2D1_RECT_F& rect, float radius, ID2D1Bitmap* pBitmap, float opacity = 1.0f, bool cropToFill = true);
    void DrawThumbnailCroppedBottom(const D2D1_RECT_F& visibleRect, const D2D1_RECT_F& originalRect, float radius, ID2D1Bitmap* pBitmap, float opacity = 1.0f);
    // Subtitle Rendering
    // Luxury Ambient Atmospheric Lighting
    void DrawAmbientBackground(const D2D1_RECT_F& bounds, const Settings& s);
    // Visual Regression Capture
    bool CaptureBackBufferToPng(const std::wstring& outputPath);

private:
    friend class DrawingPrimitives<Renderer>;
    auto DrawingTarget() const { return d2dContext.Get(); }
    friend struct RendererCacheTestAccess;
    float SnapPixel(float value) const;
    D2D1_RECT_F SnapRect(const D2D1_RECT_F& rect) const;
    struct ShadowKey {
        int widthPx = 0, heightPx = 0, radiusQ = 0, blurQ = 0;
        int offsetYQ = 0;
        uint32_t colorR = 0, colorG = 0, colorB = 0, colorA = 0;
        bool operator<(const ShadowKey& o) const {
            if (widthPx != o.widthPx) return widthPx < o.widthPx;
            if (heightPx != o.heightPx) return heightPx < o.heightPx;
            if (radiusQ != o.radiusQ) return radiusQ < o.radiusQ;
            if (blurQ != o.blurQ) return blurQ < o.blurQ;
            if (offsetYQ != o.offsetYQ) return offsetYQ < o.offsetYQ;
            if (colorR != o.colorR) return colorR < o.colorR;
            if (colorG != o.colorG) return colorG < o.colorG;
            if (colorB != o.colorB) return colorB < o.colorB;
            return colorA < o.colorA;
        }
    };
    struct ShadowEntry { ComPtr<ID2D1Bitmap> bitmap; uint64_t approximateBytes = 0; std::list<ShadowKey>::iterator lruIt; };
    static constexpr size_t kShadowCacheEntryLimit = 24;
    static constexpr uint64_t kShadowCacheByteLimit = 16ull * 1024ull * 1024ull;
    std::map<ShadowKey, ShadowEntry> shadowCache;
    std::list<ShadowKey> shadowLru;
    uint64_t shadowCacheBytes = 0;
    ComPtr<ID2D1Bitmap> GetOrCreateShadowBitmap(const ShadowKey& key);
    void TouchShadow(std::map<ShadowKey, ShadowEntry>::iterator it);
    void ClearShadowCache();
    bool primitiveSnapping = true;
    HWND hWnd = NULL;
    HANDLE frameLatencyWaitableObject = NULL;
    bool isOccluded = false;
    bool buffersTrimmed = false;
    float dpi = 96.0f;
    UINT width = 0, height = 0;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory1> d2dFactory;
    // Geometry belongs to this factory and survives device-only retirement.
    std::unordered_map<std::string, ComPtr<ID2D1PathGeometry>> svgGeometryCache;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> d2dTargetBitmap;
    ComPtr<IDWriteRenderingParams3> textRenderingParams;
    ComPtr<IWICImagingFactory> wicFactory;
    struct ThumbnailKey {
        std::wstring path;
        UINT width = 0, height = 0;
        bool operator<(const ThumbnailKey& o) const {
            if (path != o.path) return path < o.path;
            if (width != o.width) return width < o.width;
            return height < o.height;
        }
        bool operator==(const ThumbnailKey& o) const { return path == o.path && width == o.width && height == o.height; }
    };
    // Copy-free lookup key: GetOrCreateVideoThumbnail probes the cache once per
    // visible card per frame, and probing with a full ThumbnailKey would copy
    // the path string every time. The transparent comparator keeps the
    // (path, width, height) order.
    struct ThumbnailKeyProbe {
        const std::wstring* path = nullptr;
        UINT width = 0, height = 0;
    };
    struct ThumbnailKeyLess {
        using is_transparent = void;
        static const std::wstring& PathOf(const ThumbnailKey& k) { return k.path; }
        static const std::wstring& PathOf(const ThumbnailKeyProbe& k) { return *k.path; }
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            const int c = PathOf(a).compare(PathOf(b));
            if (c != 0) return c < 0;
            if (a.width != b.width) return a.width < b.width;
            return a.height < b.height;
        }
    };
    struct ThumbnailEntry {
        ComPtr<ID2D1Bitmap> bitmap;
        // Keep the cached bitmap brush with the thumbnail entry so external live-preview frames are never retained by the thumbnail cache.
        ComPtr<ID2D1BitmapBrush> brush;
        uint64_t approximateBytes = 0;
        std::list<ThumbnailKey>::iterator lruIt;
        uint64_t lastDrawnFrame = 0;
    };
    // Hub thumbnails are visual accelerators, not media state. Keep their device-backed footprint bounded so a large library cannot compete
    // with the active decoder for RAM/VRAM. All hub requests canonicalize to one
    // 320x180 bucket (backdrop uses 640x360). Scaled 320x180 thumbnails take ~230 KiB,
    // Keep a warm working set and protect the previous viewport across BeginDraw.
    static constexpr size_t kThumbnailCacheEntryLimit = 1024;
    static constexpr uint64_t kThumbnailCacheByteLimit = 64ull * 1024ull * 1024ull;
    std::map<ThumbnailKey, ThumbnailEntry, ThumbnailKeyLess> thumbnailCache;
    // Rounded draws receive only a bitmap pointer. Keep a direct index to its
    // owning entry so external preview frames never scan the full cache.
    std::unordered_map<ID2D1Bitmap*, ThumbnailEntry*> thumbnailByBitmap;
    // Include visible misses: a decoded visible image must survive the rest of the same completion batch before its first actual draw.
    std::map<ThumbnailKey, uint64_t, ThumbnailKeyLess> thumbnailDemandFrames;
    std::list<ThumbnailKey> thumbnailLru;
    uint64_t thumbnailCacheBytes = 0;
    // Permanent-decode-failure quarantine: files the shell cannot thumbnail (usually videos without a registered handler) must neither
    // occupy bitmap LRU slots nor be re-requested every frame. Negatives are tracked separately with their own small cap and cleared on
    // generation changes alongside the main cache.
    static constexpr size_t kNegativeCacheEntryLimit = 64;
    // Shell misses are retried, not stuck: the Explorer cache warms up asynchronously (and may be cold on first run), so a failure only
    // suppresses re-requests until retryTickMs, with backoff 8s/24s/60s and then permanent for the generation. mpv extraction candidates
    // are offered independently of this timer.
    struct NegativeInfo { uint64_t retryTickMs = 0; };
    std::set<ThumbnailKey, ThumbnailKeyLess> negativeCache;
    std::list<ThumbnailKey> negativeLru;
    std::map<ThumbnailKey, NegativeInfo, ThumbnailKeyLess> negativeInfo;
    std::map<std::wstring, int> negativeAttempts;
    // Paths already offered to the mpv fallback extractor in this
    // generation; prevents re-queueing a file whose extraction failed.
    std::set<std::wstring> hubExtractAttempted;
    // Private sessions read (but never write) the persistent hub disk tier. (Setter, candidate dequeue, and injection live in the public
    // thumbnail API above alongside GetOrCreateVideoThumbnail.)
    std::atomic<bool> thumbnailPrivateMode{ false };
    struct ThumbnailRequest { ThumbnailKey key; uint64_t generation = 0; uint64_t priorityFrame = 0; uint64_t priorityOrder = 0; };
    struct ThumbnailResult {
        ThumbnailKey key;
        std::vector<uint8_t> pixels;
        UINT pixelWidth = 0, pixelHeight = 0;
        bool success = false;
        uint64_t generation = 0;
    };
    static constexpr size_t kMaxPendingThumbnailRequests = 512;
    // Byte-bound for the pending decode queue: completed pixel buffers are
    // ~230 KiB at 320x180 and ~900 KiB at 640x360, so a pure count limit can
    // still retain tens of MiB of in-flight work. Evict lowest-priority entries once either bound is crossed.
    static constexpr uint64_t kMaxPendingThumbnailBytes = 64ull * 1024ull * 1024ull;
    // Start workers on demand, expanding to four at queue depths 4, 8 and 12. Idle startup creates none.
    void EnsureThumbnailWorkers();
    static uint64_t EstimateThumbnailRequestBytes(const ThumbnailKey& key);
    void ThumbnailWorkerThread();
    void EnqueueThumbnailRequest(const ThumbnailKey& key, bool lowPriority = false);
    void DrainCompletedThumbnails();
    std::vector<ThumbnailResult> TakeCompletedThumbnailBatch();
    static constexpr size_t kThumbnailUploadsPerFrame = 4;
    static constexpr uint64_t kThumbnailUploadBytesPerFrame = 2ull * 1024ull * 1024ull;
    // UI-thread state: workers use the existing coalesced ready notification.
    bool deferredThumbnailUploads = false;
    std::array<std::thread, 4> thumbnailWorkers;
    std::atomic<size_t> activeThumbnailWorkers{ 0 };
    std::mutex thumbnailMutex;
    // Serializes persistent hub writes with ClearThumbnailCache so a stale
    // decode cannot recreate a file after library removal has purged it.
    std::mutex thumbnailPersistenceMutex;
    std::condition_variable thumbnailCv;
    bool thumbnailWorkerStopping = false;
    uint64_t thumbnailGeneration = 0, thumbnailRequestFrame = 0, thumbnailRequestOrder = 0;
    std::deque<ThumbnailRequest> pendingThumbnailRequests;
    std::set<ThumbnailKey, ThumbnailKeyLess> inFlightThumbnailKeys;
    std::vector<ThumbnailResult> completedThumbnailResults;
    static constexpr size_t kMaxCompletedThumbnailResults = 64;
    static constexpr uint64_t kMaxCompletedThumbnailBytes = 16ull * 1024ull * 1024ull;
    uint64_t completedThumbnailBytes = 0;
    std::atomic<bool> thumbnailNotificationPosted{ false };
    // Stop collections are device resources and can be shared by all of the short-lived gradient brushes used during a frame.
    ComPtr<ID2D1StrokeStyle> dashedStrokeStyle, ringStrokeStyle;
    // Font Formats
    enum class TextLayoutMode { SingleLine, Wrapped, WrappedEllipsis };
    struct LayoutKey {
        std::wstring text;
        FontType font;
        int maxW, maxH;
        DWRITE_TEXT_ALIGNMENT align;
        DWRITE_PARAGRAPH_ALIGNMENT valign;
        TextLayoutMode mode;
    };
    // Copy-free lookup key: GetOrCreateTextLayout is called hundreds of times
    // per rendered frame, and probing with a full LayoutKey would copy the text
    // every time. The transparent comparator also keeps the existing order.
    struct LayoutKeyProbe {
        std::wstring_view text;
        FontType font;
        int maxW, maxH;
        DWRITE_TEXT_ALIGNMENT align;
        DWRITE_PARAGRAPH_ALIGNMENT valign;
        TextLayoutMode mode;
    };
    struct LayoutKeyLess {
        using is_transparent = void;
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            if (a.font != b.font) return a.font < b.font;
            if (a.maxW != b.maxW) return a.maxW < b.maxW;
            if (a.maxH != b.maxH) return a.maxH < b.maxH;
            if (a.align != b.align) return a.align < b.align;
            if (a.valign != b.valign) return a.valign < b.valign;
            if (a.mode != b.mode) return a.mode < b.mode;
            return a.text < b.text;
        }
    };
    struct LayoutCacheEntry { ComPtr<IDWriteTextLayout> layout; std::list<LayoutKey>::iterator lruIt; };
    static constexpr size_t kLayoutCacheEntryLimit = 512;
    // Byte-bound in addition to the entry limit: each layout owns shaped glyph
    // runs plus its key text (stored twice: map + LRU). Default 3 MiB covers
    // stable UI labels; rapidly changing time/progress strings bypass the
    // cache entirely (see GetOrCreateTextLayout).
    static constexpr uint64_t kLayoutCacheByteLimit = 3ull * 1024ull * 1024ull;
    std::map<LayoutKey, LayoutCacheEntry, LayoutKeyLess> layoutCache;
    std::list<LayoutKey> layoutLru;
    uint64_t layoutCacheBytes = 0;
    ComPtr<IDWriteTextLayout> GetOrCreateTextLayout( const std::wstring& text, FontType font, float maxW, float maxH,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
        TextLayoutMode mode = TextLayoutMode::SingleLine );
    IWICImagingFactory* EnsureWicFactory();
    // D3D11CreateDevice is pure driver initialization (~190 ms) and does not need the HWND, so it runs on a worker kicked off at startup
    // and is joined by the first CreateDeviceResources call.
    void BeginCreateDeviceAsync();
    bool EnsureD3DDevice();
    size_t ClearThumbnailCacheInternal();
    void TouchThumbnail(std::map<ThumbnailKey, ThumbnailEntry>::iterator it);
    void IndexThumbnailBitmap(std::map<ThumbnailKey, ThumbnailEntry>::iterator it);
    ThumbnailEntry* FindThumbnailByBitmap(ID2D1Bitmap* bitmap);
    bool TrimThumbnailCache(uint64_t incomingBytes);
    void ReleaseDeviceResources();
    bool CreateDeviceResources();
    bool IsDeviceLost(HRESULT hr) const;
    bool RecreateDeviceResources();
    bool RecreateRenderTarget();
    bool ResizeRenderTarget(UINT width, UINT height);
    void CreateTextRenderingParams();
    // Prevent an error reported by EndDraw/Present from recursively rebuilding
    // the device while the message loop is still inside a paint pass.
    bool recoveringDevice = false;
    // Flipped false on device loss so the pre-first-frame background erase covers the gap until a new frame has been presented.
    bool hasPresentedFrame = false;
    // Worker that pre-creates the D3D11 device during startup.
    std::thread deviceWorker;
    std::atomic<bool> deviceWorkerDone{ true };
    ComPtr<ID3D11Device> asyncD3dDevice;
    ComPtr<ID3D11DeviceContext> asyncD3dContext;
    bool asyncDeviceFailed = false;
};

} // namespace Aperture
