#pragma once

#include "Common.h"
#include "Settings.h"
#include "Renderer.h"
#include "OverlayFramePacer.h"
#include <string>
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
    HANDLE GetFrameTimerHandle() const { return frameTimer; }
    bool HasAllocatedSurface() const { return hdcMem != NULL && hBitmap != NULL && pBitmapBits != nullptr; }
    bool ShouldRefreshPlayback(bool continuousVisual, std::wstring_view subtitleText) {
        return framePacer.PlaybackContentChanged(continuousVisual, subtitleText);
    }
    bool ShouldRefreshPlayback(bool continuousAnimation, std::wstring_view subtitleText, bool controlsVisible, long long playbackSecond) {
        return framePacer.PlaybackStateChanged(continuousAnimation, subtitleText, controlsVisible, playbackSecond);
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
    void Flush() { if (dcRenderTarget && drawActive) dcRenderTarget->Flush(); }
    // After presentation, discard CPU drawing buffers only for an empty frame.
    // The layered window keeps its presented image; drawing recreates buffers.
    bool ReleaseEmptySurface();
    bool EndDrawAndPresent(HWND hOverlayWnd, int screenX, int screenY);
    float GetDpiScale() const { return dpiScale; }
    // Direct2D Drawing Primitives
    using DrawingPrimitives::ApplyTransform;
    using DrawingPrimitives::FillPolygon;
    void Clear(const D2D1_COLOR_F& color = D2D1::ColorF(0, 0, 0, 0));
    using DrawingPrimitives::FillRect;
    using DrawingPrimitives::FillGradientRect;
    using DrawingPrimitives::FillRoundedRect;
    using DrawingPrimitives::DrawRoundedRect;
    using DrawingPrimitives::FillCircle;
    using DrawingPrimitives::DrawCircle;
    using DrawingPrimitives::DrawLine;
    void DrawBitmap(ID2D1Bitmap* pBitmap, const D2D1_RECT_F& dstRect, float opacity = 1.0f);
    // 5-Primitive Real Rendering Engine
    void DrawSurface(const D2D1_RECT_F& rect, const SurfaceStyle& style);
    using DrawingPrimitives::DrawDropShadow;
    using DrawingPrimitives::DrawGlow;
    using DrawingPrimitives::DrawInnerHighlight;
    // Text & Icons
    using DrawingPrimitives::DrawString;
    using DrawingPrimitives::DrawStringWrapped;
    using FontResources::MeasureString;
    // Text at an arbitrary size (dp), for live previews and scaled OSD pops.
    using DrawingPrimitives::DrawStringSized;
    using FontResources::MeasureStringSized;
    using FontResources::GetOrCreateSizedFormat;
    void DrawIcon(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float strokeWidth = 1.6f);
    using DrawingPrimitives::DrawSvgPath;
    void DrawSubtitle(const std::wstring& text, const D2D1_RECT_F& bounds, const std::string& style, const D2D1_COLOR_F& textColor, float fontSizePoints, const std::string& position);
    using DrawingPrimitives::PushClip;
    using DrawingPrimitives::PopClip;

private:
    friend struct PlaybackUiTestAccess;
    friend class DrawingPrimitives<OverlayRenderer>;
    auto DrawingTarget() const { return dcRenderTarget.Get(); }
    ID2D1Factory* GeometryFactory() const { return d2dFactory.Get(); }
    void OnPrimitiveDrawn() { surfaceHasContent = true; }
    TextLayoutMode SingleLineTextMode() const { return TextLayoutMode::SingleLineEllipsis; }
    void BeforeDrawText(FontType font) {
        if (dcRenderTarget && font >= FontType::MenuLabel && font <= FontType::MenuShortcut && menuTextRenderingParams) {
            dcRenderTarget->SetTextRenderingParams(menuTextRenderingParams.Get());
        }
    }
    void AfterDrawText(FontType font) {
        if (dcRenderTarget && font >= FontType::MenuLabel && font <= FontType::MenuShortcut && menuTextRenderingParams) {
            dcRenderTarget->SetTextRenderingParams(textRenderingParams.Get());
        }
    }
    static constexpr ShadowConfig GetShadowConfig() {
        return { 12, 8ull * 1024ull * 1024ull, 1.0f, 1.0f, 1.8f, 0.45f, false };
    }
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
    ComPtr<IDWriteRenderingParams3> menuTextRenderingParams;
    ComPtr<ID2D1DCRenderTarget> dcRenderTarget;
    void RecreateDIB();
    bool RecreateRenderTarget();
    void ReleaseRenderTarget();
    void ReleaseDIB();
    void CreateTextRenderingParams();
    // Set only while a DC target is between BeginDraw and EndDraw.  This
    // avoids attempting a target rebuild from an invalid draw state.
    bool drawActive = false;
    bool surfaceHasContent = false;
    ComPtr<IDWriteTextFormat> subtitleFormat;
    ComPtr<IDWriteTextLayout> subtitleLayout;
    std::wstring subtitleLayoutText;
    float subtitleLayoutWidth = 0.0f, subtitleLayoutHeight = 0.0f, subtitleFormatSize = 0.0f;
};

} // namespace Aperture
