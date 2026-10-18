#pragma once

#include "Common.h"
#include "Settings.h"
#include "Renderer.h"
#include "OverlayFramePacer.h"
#include <string>
#include <memory>
#include <map>
#include <cstdint>

namespace Aperture {

class OverlayRenderer : private FontResources, private DrawingPrimitives<OverlayRenderer> {
public:
    OverlayRenderer();
    ~OverlayRenderer();
    // sharedCollection lets the overlay reuse the main Renderer's parsed
    // private-font set instead of re-reading and re-parsing the TTF files.
    bool Initialize(ID2D1Factory1* pFactory, IDWriteFactory* pDWriteFactory, float dpiScale = 1.0f, IDWriteFontCollection1* sharedCollection = nullptr);
    void Shutdown();
    void Resize(UINT w, UINT h);
    void SetDpi(float dpiScale);
    void CreateFonts(const std::wstring& customFamily = L"");
    void UpdateFontFamily(const std::string& familyName);
    void SetTextScale(float scale);
    // All playback renders, including input-driven renders, coalesce at the
    // current monitor rate. The timer wakes the UI loop for a pending frame.
    bool BeginFrame(HWND hOverlayWnd);
    void RequestRedraw() { framePacer.Request(); }
    bool HasPendingFrame() const { return framePacer.HasPendingFrame(); }
    bool IsFrameDue() const;
    DWORD GetFrameWaitMs() const;
    HANDLE GetFrameLatencyWaitableObject() const { return frameTimer; }
    bool ShouldRefreshPlayback(bool continuousVisual, std::wstring_view subtitleText) {
        return framePacer.PlaybackContentChanged(continuousVisual, subtitleText);
    }
    struct FrameStatistics {
        uint64_t framesDrawn = 0, framesPresented = 0, presentationFailures = 0;
        double drawMilliseconds = 0.0, presentMilliseconds = 0.0;
        double refreshRate = 60.0;
        double displayRefreshRate = 60.0;
        uint64_t surfaceBytes = 0;
    };
    FrameStatistics GetFrameStatistics() const;
    void BeginDraw();
    // After presentation, discard CPU drawing buffers only for an empty frame.
    // The layered window keeps its presented image; drawing recreates buffers.
    bool ReleaseEmptySurface();
    bool EndDrawAndPresent(HWND hOverlayWnd, int screenX, int screenY);
    float GetDpiScale() const { return dpiScale; }
    // Direct2D Drawing Primitives
    void Clear(const D2D1_COLOR_F& color = D2D1::ColorF(0, 0, 0, 0));
    using DrawingPrimitives::FillRect;
    void FillGradientRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor, bool vertical = true);
    using DrawingPrimitives::FillRoundedRect;
    using DrawingPrimitives::DrawRoundedRect;
    using DrawingPrimitives::FillCircle;
    using DrawingPrimitives::DrawCircle;
    using DrawingPrimitives::DrawLine;
    void DrawBitmap(ID2D1Bitmap* pBitmap, const D2D1_RECT_F& dstRect, float opacity = 1.0f);
    // 5-Primitive Real Rendering Engine
    void DrawSurface(const D2D1_RECT_F& rect, const SurfaceStyle& style);
    void DrawDropShadow(const D2D1_RECT_F& rect, float radius, float blur, float yOffset, const D2D1_COLOR_F& color);
    void DrawGlow(const D2D1_RECT_F& rect, float radius, float blur, const D2D1_COLOR_F& glowColor);
    using DrawingPrimitives::DrawInnerHighlight;
    // Text & Icons
    void DrawString(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_SIZE_F MeasureString(const std::wstring& text, FontType font = FontType::Regular);
    // Text at an arbitrary size (dp), for live previews and scaled OSD pops.
    void DrawStringSized(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float sizeDp,
                         DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
                         DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    using FontResources::MeasureStringSized;
    using FontResources::GetOrCreateSizedFormat;
    void DrawIcon(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float strokeWidth = 1.6f);
    void DrawSvgPath(const char* svgPath, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float viewBox = 24.0f, bool fill = true, float strokeWidth = 1.4f);
    void DrawSubtitle(const std::wstring& text, const D2D1_RECT_F& bounds, const std::string& style, const D2D1_COLOR_F& textColor, float fontSizePoints, const std::string& position);
    void PushClip(const D2D1_RECT_F& rect);
    void PopClip();

private:
    friend class DrawingPrimitives<OverlayRenderer>;
    auto DrawingTarget() const { return dcRenderTarget.Get(); }
    using DrawingPrimitives::FillRoundedGradientRect;
    UINT width = 0, height = 0;
    HDC hdcMem = NULL;
    HBITMAP hBitmap = NULL;
    HBITMAP hOldBitmap = NULL;
    void* pBitmapBits = nullptr;
    HWND overlayWindow = NULL;
    HANDLE frameTimer = NULL;
    OverlayFramePacer framePacer;
    HMONITOR cadenceMonitor = NULL;
    double nextCadenceCheckSeconds = 0.0;
    double drawStartSeconds = 0.0;
    FrameStatistics frameStatistics;
    void RefreshCadence(HWND hOverlayWnd, double now);
    void ArmFrameTimer(double now);
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<IDWriteRenderingParams3> textRenderingParams;
    ComPtr<ID2D1DCRenderTarget> dcRenderTarget;
    struct LayoutKey {
        std::wstring text;
        FontType font;
        int maxW = 0, maxH = 0;
        DWRITE_TEXT_ALIGNMENT align;
        DWRITE_PARAGRAPH_ALIGNMENT valign;
    };
    // Copy-free lookup key (see Renderer::LayoutKeyProbe): probing with a full
    // LayoutKey would copy the text on every hit of every drawn control label.
    struct LayoutKeyProbe {
        std::wstring_view text;
        FontType font;
        int maxW, maxH;
        DWRITE_TEXT_ALIGNMENT align;
        DWRITE_PARAGRAPH_ALIGNMENT valign;
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
            return a.text < b.text;
        }
    };
    struct LayoutCacheEntry { ComPtr<IDWriteTextLayout> layout; std::list<LayoutKey>::iterator lruIt; };
    static constexpr size_t kLayoutCacheEntryLimit = 512;
    std::map<LayoutKey, LayoutCacheEntry, LayoutKeyLess> layoutCache;
    std::list<LayoutKey> layoutLru;
    ComPtr<IDWriteTextLayout> GetOrCreateTextLayout( const std::wstring& text, FontType font, float maxW, float maxH,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR );
    // Font Formats
    void RecreateDIB();
    bool RecreateRenderTarget();
    void ReleaseRenderTarget();
    void ReleaseDIB();
    void CreateTextRenderingParams();
    // Set only while a DC target is between BeginDraw and EndDraw.  This
    // avoids attempting a target rebuild from an invalid draw state.
    bool drawActive = false;
    ComPtr<IDWriteTextFormat> subtitleFormat;
    ComPtr<IDWriteTextLayout> subtitleLayout;
    std::wstring subtitleLayoutText;
    float subtitleLayoutWidth = 0.0f, subtitleLayoutHeight = 0.0f, subtitleFormatSize = 0.0f;
};

} // namespace Aperture
