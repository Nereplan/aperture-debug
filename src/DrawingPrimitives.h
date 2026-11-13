#pragma once

#include "Common.h"
#include "FontResources.h"
#include "SvgGeometry.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aperture {

// Shared primitives are instantiated for each target without virtual dispatch.
template <class Backend> class DrawingPrimitives {
protected:
    ComPtr<ID2D1SolidColorBrush> solidBrush;
    struct GradientKey {
        uint32_t startR = 0, startG = 0, startB = 0, startA = 0;
        uint32_t endR = 0, endG = 0, endB = 0, endA = 0;
        bool operator<(const GradientKey& o) const {
            if (startR != o.startR) return startR < o.startR;
            if (startG != o.startG) return startG < o.startG;
            if (startB != o.startB) return startB < o.startB;
            if (startA != o.startA) return startA < o.startA;
            if (endR != o.endR) return endR < o.endR;
            if (endG != o.endG) return endG < o.endG;
            if (endB != o.endB) return endB < o.endB;
            return endA < o.endA;
        }
    };
    std::map<GradientKey, ComPtr<ID2D1GradientStopCollection>> gradientStopCache;
    std::map<GradientKey, ComPtr<ID2D1LinearGradientBrush>> gradientBrushCache;
    std::map<uint64_t, ComPtr<ID2D1GradientStopCollection>> innerHighlightCache;
    std::map<uint64_t, ComPtr<ID2D1LinearGradientBrush>> innerHighlightBrushCache;
    struct PolygonKey {
        std::vector<int> relCoordsQ;
        bool operator<(const PolygonKey& o) const { return relCoordsQ < o.relCoordsQ; }
    };
    std::map<PolygonKey, ComPtr<ID2D1PathGeometry>> polygonGeometryCache;
    ID2D1Factory* cachedPolygonFactory = nullptr;
    std::unordered_map<std::string, ComPtr<ID2D1PathGeometry>> svgGeometryCache;
    struct ShadowKey {
        int widthPx = 0, heightPx = 0, radiusQ = 0, blurQ = 0, offsetYQ = 0;
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
    struct ShadowEntry {
        ComPtr<ID2D1Bitmap> bitmap;
        uint64_t approximateBytes = 0;
        UINT padX = 0, topPad = 0, bottomPad = 0;
        typename std::list<ShadowKey>::iterator lruIt;
    };
    struct ShadowConfig {
        size_t entryLimit;
        uint64_t byteLimit;
        float topGrowScale;
        float bottomGrowScale;
        float falloffExponent;
        float alphaMultiplier;
        bool allowUncachedLargeBitmap;
    };
    std::map<ShadowKey, ShadowEntry> shadowCache;
    std::list<ShadowKey> shadowLru;
    std::set<ShadowKey> prevFrameShadowKeys;
    std::set<ShadowKey> currFrameShadowKeys;
    uint64_t shadowCacheBytes = 0;
    void ClearPrimitiveGeometryCaches() {
        polygonGeometryCache.clear();
        cachedPolygonFactory = nullptr;
        svgGeometryCache.clear();
    }
    void ClearGradientAndHighlightCaches() {
        gradientStopCache.clear();
        gradientBrushCache.clear();
        innerHighlightCache.clear();
        innerHighlightBrushCache.clear();
    }
    static D2D1_RECT_F SnapDrawingRect(const D2D1_RECT_F& r) {
        return D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
    }
    D2D1_RECT_F SnapTargetRect(const D2D1_RECT_F& r) const { return SnapDrawingRect(r); }
    ID2D1Factory* GeometryFactory() const { return nullptr; }
    void BeforeDrawText(FontType) {}
    void AfterDrawText(FontType) {}
    TextLayoutMode SingleLineTextMode() const { return TextLayoutMode::SingleLine; }
    static constexpr ShadowConfig GetShadowConfig() {
        return { 16, 8ull * 1024ull * 1024ull, 1.0f, 1.0f, 1.8f, 0.45f, false };
    }
    static uint32_t QuantizeDrawingColor(float value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    }
    class TransformScope {
        ID2D1RenderTarget* target;
        D2D1_MATRIX_3X2_F previous{};
    public:
        TransformScope(ID2D1RenderTarget* renderTarget, const D2D1_MATRIX_3X2_F& transform) : target(renderTarget) {
            if (!target) return;
            target->GetTransform(&previous);
            target->SetTransform(transform * previous);
        }
        TransformScope(const TransformScope&) = delete;
        TransformScope& operator=(const TransformScope&) = delete;
        ~TransformScope() { if (target) target->SetTransform(previous); }
    };
    [[nodiscard]] TransformScope ApplyTransform(const D2D1_MATRIX_3X2_F& transform) {
        return TransformScope(static_cast<Backend*>(this)->DrawingTarget(), transform);
    }
    void OnPrimitiveDrawn() {}
    void FillPolygon(const D2D1_POINT_2F* points, UINT32 count, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush || count < 3) return;
        ComPtr<ID2D1Factory> factory;
        target->GetFactory(&factory);
        if (!factory) return;
        if (factory.Get() != cachedPolygonFactory) {
            polygonGeometryCache.clear();
            cachedPolygonFactory = factory.Get();
        }
        const D2D1_POINT_2F origin = points[0];
        PolygonKey key;
        key.relCoordsQ.reserve((size_t)(count - 1) * 2);
        for (UINT32 i = 1; i < count; ++i) {
            key.relCoordsQ.push_back(static_cast<int>(std::lround((points[i].x - origin.x) * 16.0f)));
            key.relCoordsQ.push_back(static_cast<int>(std::lround((points[i].y - origin.y) * 16.0f)));
        }
        ComPtr<ID2D1PathGeometry> path;
        auto it = polygonGeometryCache.find(key);
        if (it != polygonGeometryCache.end()) {
            path = it->second;
        } else {
            if (FAILED(factory->CreatePathGeometry(&path))) return;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(path->Open(&sink))) return;
            std::vector<D2D1_POINT_2F> relPoints;
            relPoints.reserve(count - 1);
            for (UINT32 i = 1; i < count; ++i) {
                relPoints.push_back(D2D1::Point2F(points[i].x - origin.x, points[i].y - origin.y));
            }
            sink->BeginFigure(D2D1::Point2F(0.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(relPoints.data(), count - 1);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return;
            if (polygonGeometryCache.size() >= 64) polygonGeometryCache.erase(polygonGeometryCache.begin());
            polygonGeometryCache.emplace(std::move(key), path);
        }
        TransformScope scope(target, D2D1::Matrix3x2F::Translation(origin.x, origin.y));
        solidBrush->SetColor(color);
        target->FillGeometry(path.Get(), solidBrush.Get());
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        target->FillRectangle(SnapDrawingRect(rect), solidBrush.Get());
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), solidBrush.Get(), strokeWidth);
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        target->FillEllipse(e, solidBrush.Get());
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void DrawCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        target->DrawEllipse(e, solidBrush.Get(), strokeWidth);
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void FillRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ROUNDED_RECT r = D2D1::RoundedRect(SnapDrawingRect(rect), radius, radius);
        target->FillRoundedRectangle(r, solidBrush.Get());
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    void DrawRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_RECT_F snappedRect = SnapDrawingRect(rect);
        float halfStroke = strokeWidth * 0.5f;
        D2D1_RECT_F insetRect = D2D1::RectF( snappedRect.left + halfStroke, snappedRect.top + halfStroke, snappedRect.right - halfStroke,
            snappedRect.bottom - halfStroke );
        float adjustedRadius = std::max(0.0f, radius - halfStroke);
        D2D1_ROUNDED_RECT r = D2D1::RoundedRect(insetRect, adjustedRadius, adjustedRadius);
        target->DrawRoundedRectangle(r, solidBrush.Get(), strokeWidth);
        static_cast<Backend*>(this)->OnPrimitiveDrawn();
    }
    ComPtr<ID2D1LinearGradientBrush> GetOrCreateLinearGradientBrush(
        ID2D1RenderTarget* target, const D2D1_RECT_F& snappedRect, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor, bool vertical) {
        GradientKey key;
        key.startR = QuantizeDrawingColor(startColor.r); key.startG = QuantizeDrawingColor(startColor.g);
        key.startB = QuantizeDrawingColor(startColor.b); key.startA = QuantizeDrawingColor(startColor.a);
        key.endR = QuantizeDrawingColor(endColor.r); key.endG = QuantizeDrawingColor(endColor.g);
        key.endB = QuantizeDrawingColor(endColor.b); key.endA = QuantizeDrawingColor(endColor.a);
        ComPtr<ID2D1GradientStopCollection> stopCollection;
        auto cachedStops = gradientStopCache.find(key);
        if (cachedStops != gradientStopCache.end()) {
            stopCollection = cachedStops->second;
        } else {
            D2D1_GRADIENT_STOP stops[2] = { { 0.0f, startColor }, { 1.0f, endColor } };
            if (FAILED(target->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection))) return nullptr;
            if (gradientStopCache.size() >= 64) gradientStopCache.erase(gradientStopCache.begin());
            gradientStopCache.emplace(key, stopCollection);
        }
        D2D1_POINT_2F startPt = D2D1::Point2F(snappedRect.left, snappedRect.top);
        D2D1_POINT_2F endPt = vertical ? D2D1::Point2F(snappedRect.left, snappedRect.bottom)
                                       : D2D1::Point2F(snappedRect.right, snappedRect.top);
        ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto cachedBrush = gradientBrushCache.find(key);
        if (cachedBrush != gradientBrushCache.end()) {
            gradBrush = cachedBrush->second;
        } else if (SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(startPt, endPt), stopCollection.Get(), &gradBrush))) {
            if (gradientBrushCache.size() >= 64) gradientBrushCache.erase(gradientBrushCache.begin());
            gradientBrushCache.emplace(key, gradBrush);
        }
        if (gradBrush) {
            gradBrush->SetStartPoint(startPt);
            gradBrush->SetEndPoint(endPt);
        }
        return gradBrush;
    }
    void FillGradientRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor, bool vertical = true) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target) return;
        D2D1_RECT_F snappedRect = self->SnapTargetRect(rect);
        if (auto gradBrush = GetOrCreateLinearGradientBrush(target, snappedRect, startColor, endColor, vertical)) {
            target->FillRectangle(snappedRect, gradBrush.Get());
            self->OnPrimitiveDrawn();
        }
    }
    void FillRoundedGradientRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor,
                                 bool vertical) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target) return;
        D2D1_RECT_F snappedRect = SnapDrawingRect(rect);
        if (auto gradBrush = GetOrCreateLinearGradientBrush(target, snappedRect, startColor, endColor, vertical)) {
            target->FillRoundedRectangle(D2D1::RoundedRect(snappedRect, radius, radius), gradBrush.Get());
            self->OnPrimitiveDrawn();
        }
    }
    void DrawInnerHighlight(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& highlightColor, float borderWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || highlightColor.a <= 0.001f) return;
        float inset = std::max(1.0f, radius * 0.35f);
        float highlightW = RectWidth(rect) - inset * 2.0f;
        if (highlightW <= 4.0f) return;
        D2D1_RECT_F lineRect = D2D1::RectF(rect.left + inset, rect.top + borderWidth, rect.right - inset, rect.top + borderWidth + 1.0f);
        uint64_t key = (uint64_t)QuantizeDrawingColor(highlightColor.r) | ((uint64_t)QuantizeDrawingColor(highlightColor.g) << 16) |
                       ((uint64_t)QuantizeDrawingColor(highlightColor.b) << 32) | ((uint64_t)QuantizeDrawingColor(highlightColor.a) << 48);
        ComPtr<ID2D1GradientStopCollection> stopCollection;
        auto cached = innerHighlightCache.find(key);
        if (cached != innerHighlightCache.end()) { stopCollection = cached->second; } else {
            D2D1_GRADIENT_STOP stops[3];
            stops[0].position = 0.0f;
            stops[0].color = D2D1::ColorF(highlightColor.r, highlightColor.g, highlightColor.b, 0.0f);
            stops[1].position = 0.5f;
            stops[1].color = highlightColor;
            stops[2].position = 1.0f;
            stops[2].color = D2D1::ColorF(highlightColor.r, highlightColor.g, highlightColor.b, 0.0f);
            if (FAILED(target->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection))) return;
            if (innerHighlightCache.size() >= 32) innerHighlightCache.erase(innerHighlightCache.begin());
            innerHighlightCache.emplace(key, stopCollection);
        }
        D2D1_POINT_2F startPt = D2D1::Point2F(lineRect.left, lineRect.top);
        D2D1_POINT_2F endPt = D2D1::Point2F(lineRect.right, lineRect.top);
        ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto brushCached = innerHighlightBrushCache.find(key);
        if (brushCached != innerHighlightBrushCache.end()) {
            gradBrush = brushCached->second;
        } else if (SUCCEEDED(target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(startPt, endPt), stopCollection.Get(), &gradBrush))) {
            if (innerHighlightBrushCache.size() >= 32) innerHighlightBrushCache.erase(innerHighlightBrushCache.begin());
            innerHighlightBrushCache.emplace(key, gradBrush);
        }
        if (gradBrush) {
            gradBrush->SetStartPoint(startPt);
            gradBrush->SetEndPoint(endPt);
            target->FillRectangle(lineRect, gradBrush.Get());
            static_cast<Backend*>(this)->OnPrimitiveDrawn();
        }
    }
    void PushClip(const D2D1_RECT_F& rect) {
        auto* self = static_cast<Backend*>(this);
        if (auto* target = self->DrawingTarget()) target->PushAxisAlignedClip(self->SnapTargetRect(rect), D2D1_ANTIALIAS_MODE_ALIASED);
    }
    void PopClip() {
        if (auto* target = static_cast<Backend*>(this)->DrawingTarget()) target->PopAxisAlignedClip();
    }
    void DrawSvgPath(const char* svgPath, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float viewBox = 24.0f,
                     bool fill = true, float strokeWidth = 1.4f, bool cacheGeometry = true) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        auto* factory = self->GeometryFactory();
        if (!target || !solidBrush || !factory || !svgPath) return;
        ComPtr<ID2D1PathGeometry> transientGeom;
        ID2D1PathGeometry* geom = nullptr;
        if (cacheGeometry) {
            auto it = svgGeometryCache.find(svgPath);
            if (it == svgGeometryCache.end()) {
                if (svgGeometryCache.size() >= 512) svgGeometryCache.clear();
                auto built = SvgGeometry::Build(factory, svgPath);
                if (!built) return;
                it = svgGeometryCache.emplace(svgPath, std::move(built)).first;
            }
            geom = it->second.Get();
        } else {
            transientGeom = SvgGeometry::Build(factory, svgPath);
            geom = transientGeom.Get();
        }
        if (!geom) return;
        solidBrush->SetColor(color);
        const float destW = RectWidth(rect);
        const float destH = RectHeight(rect);
        const float scale = std::min(destW, destH) / viewBox;
        const float offsetX = rect.left + (destW - viewBox * scale) * 0.5f;
        const float offsetY = rect.top + (destH - viewBox * scale) * 0.5f;
        TransformScope scope = ApplyTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(offsetX, offsetY));
        if (fill) {
            target->FillGeometry(geom, solidBrush.Get());
        } else {
            target->DrawGeometry(geom, solidBrush.Get(), strokeWidth / scale);
        }
        self->OnPrimitiveDrawn();
    }
    void TouchShadow(typename std::map<ShadowKey, ShadowEntry>::iterator it) {
        if (it == shadowCache.end()) return;
        shadowLru.splice(shadowLru.begin(), shadowLru, it->second.lruIt);
        it->second.lruIt = shadowLru.begin();
    }
    void ClearShadowCache() {
        shadowCache.clear();
        shadowLru.clear();
        prevFrameShadowKeys.clear();
        currFrameShadowKeys.clear();
        shadowCacheBytes = 0;
    }
    void BeginShadowFrame() {
        prevFrameShadowKeys = std::move(currFrameShadowKeys);
        currFrameShadowKeys.clear();
    }
    ComPtr<ID2D1Bitmap> GetOrCreateShadowBitmap(const ShadowKey& key, UINT& padXOut, UINT& topPadOut, UINT& bottomPadOut) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || key.widthPx <= 0 || key.heightPx <= 0 || key.blurQ <= 0) return nullptr;
        currFrameShadowKeys.insert(key);
        auto cached = shadowCache.find(key);
        if (cached != shadowCache.end()) {
            TouchShadow(cached);
            padXOut = cached->second.padX;
            topPadOut = cached->second.topPad;
            bottomPadOut = cached->second.bottomPad;
            return cached->second.bitmap;
        }
        // Only cache when exact dimensions/opacity hold for 2 consecutive frames
        // so morphing or fading surfaces never thrash GPU texture allocations.
        if (prevFrameShadowKeys.find(key) == prevFrameShadowKeys.end()) return nullptr;
        const ShadowConfig cfg = Backend::GetShadowConfig();
        const float radius = std::max(0.0f, key.radiusQ / 4.0f);
        const float blur = std::max(0.0f, key.blurQ / 4.0f);
        const float yOffset = key.offsetYQ / 4.0f;
        const UINT padX = static_cast<UINT>(std::max(2.0f, std::ceil(blur) + 2.0f));
        const UINT topPad = static_cast<UINT>(std::max(2.0f, std::ceil(blur * cfg.topGrowScale + std::max(0.0f, -yOffset)) + 2.0f));
        const UINT bottomPad = static_cast<UINT>(std::max(2.0f, std::ceil(blur * cfg.bottomGrowScale + std::max(0.0f, yOffset)) + 2.0f));
        const UINT pixelWidth = static_cast<UINT>(key.widthPx) + padX * 2u;
        const UINT pixelHeight = static_cast<UINT>(key.heightPx) + topPad + bottomPad;
        if (pixelWidth == 0 || pixelHeight == 0 || pixelWidth > 4096 || pixelHeight > 4096) return nullptr;
        const uint64_t approximateBytes = static_cast<uint64_t>(pixelWidth) * pixelHeight * 4ull;
        if (!cfg.allowUncachedLargeBitmap && approximateBytes > cfg.byteLimit) return nullptr;
        D2D1_SIZE_U pixelSize = D2D1::SizeU(pixelWidth, pixelHeight);
        const D2D1_PIXEL_FORMAT shadowPixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1BitmapRenderTarget> shadowTarget;
        if (FAILED(target->CreateCompatibleRenderTarget(nullptr, &pixelSize, &shadowPixelFormat,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &shadowTarget)) || !shadowTarget) return nullptr;
        shadowTarget->SetDpi(96.0f, 96.0f);
        shadowTarget->BeginDraw();
        shadowTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        D2D1_COLOR_F shadowColor = D2D1::ColorF(
            key.colorR / 65535.0f, key.colorG / 65535.0f, key.colorB / 65535.0f, key.colorA / 65535.0f);
        ComPtr<ID2D1SolidColorBrush> shadowBrush;
        if (FAILED(shadowTarget->CreateSolidColorBrush(shadowColor, &shadowBrush)) || !shadowBrush) {
            shadowTarget->EndDraw();
            return nullptr;
        }
        const D2D1_RECT_F baseRect = D2D1::RectF(
            static_cast<float>(padX), static_cast<float>(topPad),
            static_cast<float>(padX + static_cast<UINT>(key.widthPx)),
            static_cast<float>(topPad + static_cast<UINT>(key.heightPx)));
        constexpr int kSteps = 6;
        for (int i = kSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float grow = blur * t;
            D2D1_ROUNDED_RECT s = D2D1::RoundedRect(baseRect, radius + grow, radius + grow);
            s.rect.left -= grow;   s.rect.right += grow;
            s.rect.top  -= grow * cfg.topGrowScale;
            s.rect.bottom += grow * cfg.bottomGrowScale;
            s.rect.top += yOffset; s.rect.bottom += yOffset;
            const float falloff = std::pow(1.0f - t, cfg.falloffExponent);
            shadowBrush->SetColor(D2D1::ColorF(shadowColor.r, shadowColor.g, shadowColor.b, shadowColor.a * falloff * cfg.alphaMultiplier));
            shadowTarget->FillRoundedRectangle(s, shadowBrush.Get());
        }
        if (FAILED(shadowTarget->EndDraw())) return nullptr;
        ComPtr<ID2D1Bitmap> bitmap;
        if (FAILED(shadowTarget->GetBitmap(&bitmap)) || !bitmap) return nullptr;
        padXOut = padX;
        topPadOut = topPad;
        bottomPadOut = bottomPad;
        if (approximateBytes > cfg.byteLimit) return bitmap;
        while (!shadowLru.empty() && (shadowCache.size() >= cfg.entryLimit ||
                shadowCacheBytes + approximateBytes > cfg.byteLimit)) {
            const ShadowKey& oldKey = shadowLru.back();
            auto old = shadowCache.find(oldKey);
            if (old != shadowCache.end()) {
                shadowCacheBytes -= std::min(shadowCacheBytes, old->second.approximateBytes);
                shadowCache.erase(old);
            }
            shadowLru.pop_back();
        }
        shadowLru.push_front(key);
        ShadowEntry entry{ bitmap, approximateBytes, padX, topPad, bottomPad, shadowLru.begin() };
        auto inserted = shadowCache.emplace(key, std::move(entry));
        if (inserted.second) {
            shadowCacheBytes += approximateBytes;
            return bitmap;
        }
        shadowLru.pop_front();
        TouchShadow(inserted.first);
        return inserted.first->second.bitmap;
    }
    void DrawDropShadow(const D2D1_RECT_F& rect, float radius, float blur, float yOffset, const D2D1_COLOR_F& color) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target || !solidBrush || color.a <= 0.001f || blur <= 0.0f) return;
        const ShadowConfig cfg = Backend::GetShadowConfig();
        ShadowKey key;
        key.widthPx = std::max(1, static_cast<int>(std::lround(RectWidth(rect))));
        key.heightPx = std::max(1, static_cast<int>(std::lround(RectHeight(rect))));
        key.radiusQ = std::max(0, static_cast<int>(std::lround(radius * 4.0f)));
        key.blurQ = std::max(1, static_cast<int>(std::lround(blur * 4.0f)));
        key.offsetYQ = static_cast<int>(std::lround(yOffset * 4.0f));
        key.colorR = QuantizeDrawingColor(color.r);
        key.colorG = QuantizeDrawingColor(color.g);
        key.colorB = QuantizeDrawingColor(color.b);
        key.colorA = QuantizeDrawingColor(color.a);
        UINT padX = 0, topPad = 0, bottomPad = 0;
        if (ComPtr<ID2D1Bitmap> shadow = GetOrCreateShadowBitmap(key, padX, topPad, bottomPad)) {
            const D2D1_SIZE_F shadowSize = shadow->GetSize();
            target->DrawBitmap(
                shadow.Get(),
                D2D1::RectF(rect.left - static_cast<float>(padX), rect.top - static_cast<float>(topPad),
                            rect.left - static_cast<float>(padX) + shadowSize.width,
                            rect.top - static_cast<float>(topPad) + shadowSize.height),
                1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            self->OnPrimitiveDrawn();
            return;
        }
        constexpr int kSteps = 6;
        for (int i = kSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float grow = blur * t;
            D2D1_ROUNDED_RECT s = D2D1::RoundedRect(rect, radius + grow, radius + grow);
            s.rect.left -= grow;   s.rect.right += grow;
            s.rect.top  -= grow * cfg.topGrowScale;
            s.rect.bottom += grow * cfg.bottomGrowScale;
            s.rect.top += yOffset; s.rect.bottom += yOffset;
            const float falloff = std::pow(1.0f - t, cfg.falloffExponent);
            solidBrush->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a * falloff * cfg.alphaMultiplier));
            target->FillRoundedRectangle(s, solidBrush.Get());
        }
        self->OnPrimitiveDrawn();
    }
    void DrawGlow(const D2D1_RECT_F& rect, float radius, float blur, const D2D1_COLOR_F& glowColor) {
        if (glowColor.a <= 0.001f || blur <= 0.0f) return;
        DrawDropShadow(rect, radius, blur, 0.0f, glowColor);
    }
    void DrawString(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                    FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target || !solidBrush || text.empty()) return;
        D2D1_RECT_F snappedRect = self->SnapTargetRect(rect);
        ComPtr<IDWriteTextLayout> layout = self->GetOrCreateTextLayout(
            text, font, std::max(1.0f, RectWidth(snappedRect)), std::max(1.0f, RectHeight(snappedRect)),
            align, valign, self->SingleLineTextMode());
        if (!layout) return;
        self->BeforeDrawText(font);
        solidBrush->SetColor(color);
        target->DrawTextLayout(D2D1::Point2F(snappedRect.left, snappedRect.top), layout.Get(), solidBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        self->AfterDrawText(font);
        self->OnPrimitiveDrawn();
    }
    void DrawStringWrapped(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                           FontType font = FontType::Regular, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target || !solidBrush || text.empty()) return;
        D2D1_RECT_F snappedRect = self->SnapTargetRect(rect);
        ComPtr<IDWriteTextLayout> layout = self->GetOrCreateTextLayout(
            text, font, std::max(1.0f, RectWidth(snappedRect)), std::max(1.0f, RectHeight(snappedRect)),
            align, valign, TextLayoutMode::WrappedEllipsis);
        if (!layout) return;
        solidBrush->SetColor(color);
        target->DrawTextLayout(D2D1::Point2F(snappedRect.left, snappedRect.top), layout.Get(), solidBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
        self->OnPrimitiveDrawn();
    }
    void DrawStringSized(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color, float sizeDp,
                         DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER,
                         DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        auto* self = static_cast<Backend*>(this);
        auto* target = self->DrawingTarget();
        if (!target || !solidBrush || text.empty()) return;
        D2D1_RECT_F snappedRect = self->SnapTargetRect(rect);
        ComPtr<IDWriteTextLayout> layout = self->GetOrCreateSizedLayout(
            text, sizeDp, weight, std::max(1.0f, RectWidth(snappedRect)), std::max(1.0f, RectHeight(snappedRect)), align, valign);
        if (!layout) return;
        solidBrush->SetColor(color);
        target->DrawTextLayout(D2D1::Point2F(snappedRect.left, snappedRect.top), layout.Get(), solidBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        self->OnPrimitiveDrawn();
    }
    void DrawStringWithShadow(const std::wstring& text, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color,
                              const D2D1_COLOR_F& shadowColor, FontType font = FontType::Regular,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, float shadowOffsetY = 1.0f) {
        auto* self = static_cast<Backend*>(this);
        if (shadowColor.a > 0.001f) {
            D2D1_RECT_F sRect = D2D1::RectF(rect.left, rect.top + shadowOffsetY * self->dpiScale,
                                            rect.right, rect.bottom + shadowOffsetY * self->dpiScale);
            DrawString(text, sRect, shadowColor, font, align, valign);
        }
        DrawString(text, rect, color, font, align, valign);
    }
};

} // namespace Aperture
