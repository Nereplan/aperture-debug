#include "OverlayRenderer.h"
#include "Renderer.h"
#include "Localization.h"
#include <cmath>

namespace Aperture {

namespace {

inline float SnapPixel(float value) { return std::round(value); }

inline D2D1_RECT_F SnapRect(const D2D1_RECT_F& rect) {
    return D2D1::RectF( SnapPixel(rect.left), SnapPixel(rect.top), SnapPixel(rect.right), SnapPixel(rect.bottom) );
}

double FrameClockSeconds() {
    static const double frequency = [] { LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return static_cast<double>(value.QuadPart); }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}
}

OverlayRenderer::OverlayRenderer() {}

OverlayRenderer::~OverlayRenderer() { Shutdown(); }

void OverlayRenderer::Shutdown() {
    drawActive = false;
    if (frameTimer) { CancelWaitableTimer(frameTimer); CloseHandle(frameTimer); frameTimer = NULL; }
    framePacer.Reset();
    cadenceMonitor = NULL;
    nextCadenceCheckSeconds = 0.0;
    frameStatistics = {};
    if (dcRenderTarget) { dcRenderTarget.Reset(); }
    if (hdcMem) { if (hOldBitmap) SelectObject(hdcMem, hOldBitmap); DeleteDC(hdcMem); hdcMem = NULL; }
    if (hBitmap) { DeleteObject(hBitmap); hBitmap = NULL; }
    pBitmapBits = nullptr;
    solidBrush.Reset();
    ClearShadowCache();
    ClearPrimitiveGeometryCaches();
    ClearGradientAndHighlightCaches();
    ClearSizedCaches();
    ClearLayoutCaches();
    textRenderingParams.Reset();
    menuTextRenderingParams.Reset();
    subtitleFormat.Reset();
    subtitleLayout.Reset();
    subtitleLayoutText.clear();
    customFontCollection.Reset();
    dwriteFactory.Reset();
    d2dFactory.Reset();
}

bool OverlayRenderer::Initialize(ID2D1Factory1* pFactory, IDWriteFactory* pDWriteFactory, float scale, IDWriteFontCollection1* sharedCollection) {
    if (d2dFactory.Get() != pFactory) svgGeometryCache.clear();
    d2dFactory = pFactory;
    if (pDWriteFactory) { pDWriteFactory->QueryInterface(__uuidof(IDWriteFactory3), (void**)&dwriteFactory); }
    dpiScale = scale;
    if (sharedCollection) { customFontCollection = sharedCollection; }
    if (!RecreateRenderTarget()) return false;
    CreateTextRenderingParams();
    CreateFonts();
    // High-resolution timers avoid changing the process/global timer period.
    // Older Windows versions retain a normal waitable-timer fallback.
    frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!frameTimer) frameTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
    // Allocate the backing DIB for the size already recorded by Resize() calls that arrived before this lazy initialization.
    if (width > 0 && height > 0) { RecreateDIB(); }
    return true;
}

void OverlayRenderer::RefreshCadence(HWND hOverlayWnd, double now) {
    HMONITOR monitor = MonitorFromWindow(hOverlayWnd, MONITOR_DEFAULTTONEAREST);
    if (monitor == cadenceMonitor && now < nextCadenceCheckSeconds) return;
    cadenceMonitor = monitor;
    nextCadenceCheckSeconds = now + 1.0;
    double refreshRate = 60.0;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (monitor && GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        if (mode.dmDisplayFrequency >= 20 && mode.dmDisplayFrequency <= 360) refreshRate = mode.dmDisplayFrequency;
    }
    frameStatistics.displayRefreshRate = refreshRate;
    // Video retains mpv's independent presentation rate. Overlay controls and
    // animations need at most 60 updates/sec even on a high-refresh monitor.
    framePacer.SetRefreshRate(std::min(60.0, refreshRate));
}

void OverlayRenderer::ArmFrameTimer(double now) {
    if (!frameTimer) return;
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(std::ceil(framePacer.SecondsUntilDeadline(now) * 10000000.0)));
    if (!SetWaitableTimer(frameTimer, &due, 0, nullptr, nullptr, FALSE)) {
        CloseHandle(frameTimer);
        frameTimer = NULL; // The message-loop timeout remains a usable fallback.
    }
}

bool OverlayRenderer::BeginFrame(HWND hOverlayWnd) {
    const double now = FrameClockSeconds();
    RefreshCadence(hOverlayWnd, now);
    framePacer.Request();
    if (!framePacer.IsDue(now)) { ArmFrameTimer(now); return false; }
    framePacer.FrameStarted(now);
    ArmFrameTimer(now);
    return true;
}

bool OverlayRenderer::IsFrameDue() const { return framePacer.IsDue(FrameClockSeconds()); }

DWORD OverlayRenderer::GetFrameWaitMs() const {
    if (!framePacer.HasPendingFrame()) return 64;
    return static_cast<DWORD>(std::clamp(std::ceil(framePacer.SecondsUntilDeadline(FrameClockSeconds()) * 1000.0), 1.0, 50.0));
}

OverlayRenderer::FrameStatistics OverlayRenderer::GetFrameStatistics() const {
    auto result = frameStatistics;
    result.refreshRate = framePacer.GetRefreshRate();
    result.surfaceBytes = hBitmap ? static_cast<uint64_t>(width) * height * 4 : 0;
    return result;
}

void OverlayRenderer::CreateFonts(const std::wstring& customFamily) {
    if (!dwriteFactory) return;
    framePacer.Request();
    selectedFontFamily = customFamily.empty() ? L"Inter" : customFamily;
    LoadCustomFontCollection();
    ClearLayoutCaches();
    ClearSizedCaches();
    subtitleFormat.Reset();
    subtitleLayout.Reset();
    subtitleLayoutText.clear();
    subtitleFormatSize = 0.0f;
    CreateFontFormats();
}
void OverlayRenderer::UpdateFontFamily(const std::string& familyName) {
    selectedFontFamily = Utf8ToUtf16(familyName);
    CreateFonts(selectedFontFamily);
}

void OverlayRenderer::SetTextScale(float scale) {
    const float normalized = std::clamp(scale, 0.75f, 2.25f);
    if (std::abs(normalized - textScale) < 0.001f) return;
    textScale = normalized;
    CreateFonts(selectedFontFamily);
}

void OverlayRenderer::CreateTextRenderingParams() {
    textRenderingParams.Reset();
    menuTextRenderingParams.Reset();
    if (!dwriteFactory) return;
    ComPtr<IDWriteRenderingParams> monitorParams;
    HMONITOR hMon = overlayWindow ? MonitorFromWindow(overlayWindow, MONITOR_DEFAULTTONEAREST) : NULL;
    if (hMon) { dwriteFactory->CreateMonitorRenderingParams(hMon, &monitorParams); }
    if (!monitorParams) { dwriteFactory->CreateRenderingParams(&monitorParams); }
    float gamma = monitorParams ? monitorParams->GetGamma() : 1.8f;
    // The overlay is a transparent layered window (premultiplied alpha). ClearType subpixel rendering is invalid on transparent surfaces
    // because it writes un-blended RGB channels assuming an opaque background. Grayscale antialiasing with flat pixel geometry and
    // clearTypeLevel 0 ensures crisp, clean glyph alpha coverage without colored fringes.
    dwriteFactory->CreateCustomRenderingParams( gamma, 1.0f, 1.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, DWRITE_GRID_FIT_MODE_DISABLED, &textRenderingParams );
    // Compact menu labels need grid fitting at small sizes. Keep grayscale
    // coverage for the layered surface and avoid the heavy contrast of OSD text.
    dwriteFactory->CreateCustomRenderingParams( gamma, 0.0f, 0.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE1_NATURAL, DWRITE_GRID_FIT_MODE_ENABLED, &menuTextRenderingParams );
}

void OverlayRenderer::ReleaseRenderTarget() {
    ClearShadowCache();
    dcRenderTarget.Reset();
    solidBrush.Reset();
    ClearGradientAndHighlightCaches();
}

bool OverlayRenderer::RecreateRenderTarget() {
    if (!d2dFactory) return false;
    ReleaseRenderTarget();
    // All overlay geometry is explicitly dp-scaled by UIComponents. A 96 DPI DC target therefore maps one supplied coordinate to one DIB
    // pixel at every monitor scale and keeps layered-window dimensions deterministic.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties( D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE,
        D2D1_FEATURE_LEVEL_DEFAULT );
    HRESULT hr = d2dFactory->CreateDCRenderTarget(&props, &dcRenderTarget);
    if (FAILED(hr) || !dcRenderTarget) return false;
    hr = dcRenderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &solidBrush);
    if (FAILED(hr) || !solidBrush) { dcRenderTarget.Reset(); return false; }
    if (!textRenderingParams) CreateTextRenderingParams();
    return true;
}

void OverlayRenderer::SetDpi(float scale) {
    dpiScale = std::clamp(scale, 0.5f, 8.0f);
    CreateTextRenderingParams();
    ClearLayoutCaches();
    CreateFonts(selectedFontFamily);
    if (dcRenderTarget) {
        // UI callers already provide physical-pixel coordinates (dp-scaled),
        // so keeping the DC target at 96 DPI avoids a second scale transform.
        dcRenderTarget->SetDpi(96.0f, 96.0f);
    }
}

void OverlayRenderer::Resize(UINT w, UINT h) {
    w = std::max(1u, w);
    h = std::max(1u, h);
    if (w == width && h == height && hdcMem && hBitmap) return;
    width = w;
    height = h;
    framePacer.Request();
    // Before the overlay is first initialized (lazy, at first playback) only
    // remember the requested size; the DIB is allocated by the init path.
    if (!d2dFactory) return;
    RecreateDIB();
}

void OverlayRenderer::ReleaseDIB() {
    if (hdcMem) { if (hOldBitmap) SelectObject(hdcMem, hOldBitmap); DeleteDC(hdcMem); hdcMem = NULL; }
    if (hBitmap) { DeleteObject(hBitmap); hBitmap = NULL; }
    hOldBitmap = NULL;
    pBitmapBits = nullptr;
    surfaceHasContent = false;
}

void OverlayRenderer::RecreateDIB() {
    ReleaseDIB();
    if (width == 0 || height == 0) return;
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;
    hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) { ReleaseDC(NULL, hdcScreen); return; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -(LONG)height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBitmapBits, NULL, 0);
    if (hBitmap) { hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap); } else { pBitmapBits = nullptr; }
    ReleaseDC(NULL, hdcScreen);
}

bool OverlayRenderer::ReleaseEmptySurface() {
    if (drawActive || !pBitmapBits || !hBitmap) return false;
    // Fast-path O(1) check: if any draw call or non-zero Clear occurred since
    // BeginDraw, reject immediately without scanning the 32-bit DIB buffer.
    if (surfaceHasContent) return false;
    ReleaseRenderTarget();
    ReleaseDIB();
    return true;
}

void OverlayRenderer::BeginDraw() {
    if (drawActive) return;
    if ((!dcRenderTarget || !solidBrush) && !RecreateRenderTarget()) return;
    if (!hdcMem || !hBitmap) {
        if (width > 0 && height > 0 && d2dFactory) { RecreateDIB(); }
    }
    if (!hdcMem || !hBitmap || width == 0 || height == 0) return;
    drawStartSeconds = FrameClockSeconds();
    RECT rc = { 0, 0, (LONG)width, (LONG)height };
    if (FAILED(dcRenderTarget->BindDC(hdcMem, &rc))) return;
    dcRenderTarget->BeginDraw();
    drawActive = true;
    surfaceHasContent = false;
    BeginShadowFrame();
    dcRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dcRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (textRenderingParams) dcRenderTarget->SetTextRenderingParams(textRenderingParams.Get());
    dcRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
}

bool OverlayRenderer::EndDrawAndPresent(HWND hOverlayWnd, int screenX, int screenY) {
    if (!drawActive || !dcRenderTarget || !hdcMem || !hBitmap) { framePacer.Request(); return false; }
    overlayWindow = hOverlayWnd;
    HRESULT drawHr = dcRenderTarget->EndDraw();
    drawActive = false;
    ++frameStatistics.framesDrawn;
    frameStatistics.drawMilliseconds += (FrameClockSeconds() - drawStartSeconds) * 1000.0;
    if (drawHr == D2DERR_RECREATE_TARGET) { RecreateRenderTarget(); framePacer.Request(); return false; }
    if (FAILED(drawHr)) {
        // A DC render target has no DXGI swap chain, but driver resets can still invalidate it. Releasing/recreating the target also drops
        // all device-dependent brushes and gradient stops without touching the device-independent DirectWrite layouts.
        if (drawHr == D2DERR_RECREATE_TARGET || drawHr == D2DERR_OUTSTANDING_BITMAP_REFERENCES) { RecreateRenderTarget(); }
        framePacer.Request();
        return false;
    }
    POINT ptDst = { screenX, screenY };
    SIZE sizeDst = { (LONG)width, (LONG)height };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const double presentStart = FrameClockSeconds();
    // A null destination DC uses the default palette, sufficient for the
    // 32-bit BGRA DIB, and avoids a GetDC/ReleaseDC pair on every update.
    const bool presented = UpdateLayeredWindow(hOverlayWnd, nullptr, &ptDst, &sizeDst, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA) != FALSE;
    frameStatistics.presentMilliseconds += (FrameClockSeconds() - presentStart) * 1000.0;
    if (presented) ++frameStatistics.framesPresented;
    else { ++frameStatistics.presentationFailures; framePacer.Request(); }
    // The overlay is a sibling layered window and does not receive the main window's WM_DPICHANGED path. Pick up monitor transitions here
    // so the next frame rebuilds fonts and target state at the correct scale.
    typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
    static GetDpiForWindowProc getDpi = []() -> GetDpiForWindowProc {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? (GetDpiForWindowProc)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    }();
    if (getDpi && overlayWindow) {
        float nextScale = (float)getDpi(overlayWindow) / 96.0f;
        if (std::abs(nextScale - dpiScale) > 0.001f) SetDpi(nextScale);
    }
    return presented;
}

void OverlayRenderer::Clear(const D2D1_COLOR_F& color) {
    if (dcRenderTarget) {
        dcRenderTarget->Clear(color);
        surfaceHasContent = (color.a > 0.0f || color.r > 0.0f || color.g > 0.0f || color.b > 0.0f);
    }
}

void OverlayRenderer::DrawBitmap(ID2D1Bitmap* pBitmap, const D2D1_RECT_F& dstRect, float opacity) {
    if (!dcRenderTarget || !pBitmap || opacity <= 0.001f) return;
    dcRenderTarget->DrawBitmap(pBitmap, SnapRect(dstRect), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    surfaceHasContent = true;
}

void OverlayRenderer::DrawIcon(IconType icon, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float) {
    const auto* data = FindIconPath(icon);
    if (!data || !data->overlaySupported) data = FindIconPath(IconType::ApertureLogo);
    DrawSvgPath(data->path, rect, color, data->viewBox, data->fill);
}

void OverlayRenderer::DrawSurface(const D2D1_RECT_F& rect, const SurfaceStyle& style) {
    if (!dcRenderTarget) return;
    D2D1_RECT_F surfaceRect = SnapRect(rect);
    // 1. Outer Drop Shadow
    if (style.shadowColor.a > 0.001f && style.shadowBlur > 0.0f)
        DrawDropShadow(surfaceRect, style.radius, style.shadowBlur, style.shadowOffsetY, style.shadowColor);
    // 2. Accent Glow
    if (style.glowColor.a > 0.001f && style.glowBlur > 0.0f) { DrawGlow(surfaceRect, style.radius, style.glowBlur, style.glowColor); }
    // 3. Surface Base Fill & Gradients directly via rounded primitives (avoids offscreen layer push/pop).
    if (style.fill.a > 0.001f) { FillRoundedRect(surfaceRect, style.radius, style.fill); }
    if (style.hasGradient) { FillRoundedGradientRect(surfaceRect, style.radius, style.gradientStart, style.gradientEnd, false); }
    // 4. Inner Top Highlight (Light source from above)
    if (style.hasInnerHighlight && style.innerHighlight.a > 0.001f)
        DrawInnerHighlight(surfaceRect, style.radius, style.innerHighlight, style.borderWidth);
    // 5. Crisp Translucent Border
    if (style.border.a > 0.001f && style.borderWidth > 0.0f)
        DrawRoundedRect(surfaceRect, style.radius, style.border, style.borderWidth);
}


void OverlayRenderer::DrawSubtitle(const std::wstring& text, const D2D1_RECT_F& bounds, const std::string& style, const D2D1_COLOR_F& textColor, float fontSizePoints, const std::string& position) {
    if (!dcRenderTarget || text.empty()) return;
    float dp = dpiScale;
    float boxW = RectWidth(bounds) * 0.82f;
    float requestedSize = std::max(1.0f, fontSizePoints) * dp;
    if (!subtitleFormat || std::abs(subtitleFormatSize - requestedSize) > 0.01f) {
        subtitleFormat.Reset();
        subtitleLayout.Reset();
        subtitleLayoutText.clear();
        // Custom SRT/VTT follows the clean Web sans; ASS/SSA is handled by
        // libmpv and does not pass through this overlay renderer.
        const wchar_t* family = L"Inter";
        MakeFormat( dwriteFactory.Get(), customFontCollection.Get(), family,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, requestedSize, L"en-US", &subtitleFormat );
        subtitleFormatSize = requestedSize;
    }
    if (!subtitleFormat || !dwriteFactory) return;
    float boxH = std::max(90.0f * dp, requestedSize * 2.2f);
    float bottomY = bounds.bottom - 95.0f * dp;
    if (position == "elevated") bottomY = bounds.bottom - 150.0f * dp;
    else if (position == "top") bottomY = bounds.top + 70.0f * dp;
    D2D1_RECT_F subRect = SnapRect(D2D1::RectF( bounds.left + (RectWidth(bounds) - boxW) * 0.5f, bottomY,
        bounds.left + (RectWidth(bounds) + boxW) * 0.5f, bottomY + boxH ));
    if (!subtitleLayout || subtitleLayoutText != text || std::abs(subtitleLayoutWidth - RectWidth(subRect)) > 0.01f ||
        std::abs(subtitleLayoutHeight - RectHeight(subRect)) > 0.01f) {
        subtitleLayout.Reset();
        dwriteFactory->CreateTextLayout( text.c_str(), (UINT32)text.length(), subtitleFormat.Get(),
            std::max(1.0f, RectWidth(subRect)), std::max(1.0f, RectHeight(subRect)), &subtitleLayout );
        if (!subtitleLayout) return;
        subtitleLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        subtitleLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        subtitleLayoutText = text;
        subtitleLayoutWidth = RectWidth(subRect);
        subtitleLayoutHeight = RectHeight(subRect);
    }
    if (style == "solid" || style == "translucent") {
        float alpha = (style == "solid") ? 0.85f : 0.45f;
        FillRoundedRect(subRect, 8.0f * dp, D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha));
    } else if (style == "vlc-outline") {
        solidBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.95f));
        // Full 1px 8-neighborhood plus 4 cardinal 2px offsets (12 passes vs 24):
        // preserves a continuous, gap-free 2px outline at 96 DPI while cutting
        // software ID2D1DCRenderTarget glyph rasterization passes in half.
        static constexpr int kOffsets[12][2] = {
            {-1, -1}, {-1, 0}, {-1, 1},
            { 0, -1},          { 0, 1},
            { 1, -1}, { 1, 0}, { 1, 1},
            {-2,  0}, { 2, 0}, { 0, -2}, { 0, 2}
        };
        for (const auto& off : kOffsets) {
            dcRenderTarget->DrawTextLayout(
                D2D1::Point2F(subRect.left + off[0] * dp, subRect.top + off[1] * dp),
                subtitleLayout.Get(), solidBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    solidBrush->SetColor(textColor);
    dcRenderTarget->DrawTextLayout(D2D1::Point2F(subRect.left, subRect.top), subtitleLayout.Get(), solidBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    surfaceHasContent = true;
}
} // namespace Aperture
