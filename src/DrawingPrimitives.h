#pragma once

#include "Common.h"
#include <map>

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
    static D2D1_RECT_F SnapDrawingRect(const D2D1_RECT_F& r) {
        return D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
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
    void FillPolygon(const D2D1_POINT_2F* points, UINT32 count, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush || count < 3) return;
        ComPtr<ID2D1Factory> factory;
        target->GetFactory(&factory);
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(factory->CreatePathGeometry(&path))) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(&sink))) return;
        sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(points + 1, count - 1);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return;
        solidBrush->SetColor(color);
        target->FillGeometry(path.Get(), solidBrush.Get());
    }
    void FillRect(const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        target->FillRectangle(SnapDrawingRect(rect), solidBrush.Get());
    }
    void DrawLine(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), solidBrush.Get(), strokeWidth);
    }
    void FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        target->FillEllipse(e, solidBrush.Get());
    }
    void DrawCircle(float cx, float cy, float radius, const D2D1_COLOR_F& color, float strokeWidth = 1.0f) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        target->DrawEllipse(e, solidBrush.Get(), strokeWidth);
    }
    void FillRoundedRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& color) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target || !solidBrush) return;
        solidBrush->SetColor(color);
        D2D1_ROUNDED_RECT r = D2D1::RoundedRect(SnapDrawingRect(rect), radius, radius);
        target->FillRoundedRectangle(r, solidBrush.Get());
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
    }
    void FillRoundedGradientRect(const D2D1_RECT_F& rect, float radius, const D2D1_COLOR_F& startColor, const D2D1_COLOR_F& endColor,
                                 bool vertical) {
        auto* target = static_cast<Backend*>(this)->DrawingTarget();
        if (!target) return;
        D2D1_RECT_F snappedRect = SnapDrawingRect(rect);
        GradientKey key;
        key.startR = QuantizeDrawingColor(startColor.r); key.startG = QuantizeDrawingColor(startColor.g);
        key.startB = QuantizeDrawingColor(startColor.b); key.startA = QuantizeDrawingColor(startColor.a);
        key.endR = QuantizeDrawingColor(endColor.r); key.endG = QuantizeDrawingColor(endColor.g);
        key.endB = QuantizeDrawingColor(endColor.b); key.endA = QuantizeDrawingColor(endColor.a);
        ComPtr<ID2D1GradientStopCollection> stopCollection;
        auto cachedStops = gradientStopCache.find(key);
        if (cachedStops != gradientStopCache.end()) { stopCollection = cachedStops->second; } else {
            D2D1_GRADIENT_STOP stops[2] = {
                { 0.0f, startColor }, { 1.0f, endColor }
            };
            if (FAILED(target->CreateGradientStopCollection( stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection))) return;
            if (gradientStopCache.size() >= 64) gradientStopCache.clear();
            gradientStopCache.emplace(key, stopCollection);
        }
        D2D1_POINT_2F startPt = D2D1::Point2F(snappedRect.left, snappedRect.top);
        D2D1_POINT_2F endPt = vertical ? D2D1::Point2F(snappedRect.left, snappedRect.bottom)
            : D2D1::Point2F(snappedRect.right, snappedRect.top);
        ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto cachedBrush = gradientBrushCache.find(key);
        if (cachedBrush != gradientBrushCache.end()) {
            gradBrush = cachedBrush->second;
        } else if (SUCCEEDED(target->CreateLinearGradientBrush( D2D1::LinearGradientBrushProperties(startPt, endPt),
            stopCollection.Get(), &gradBrush))) {
            if (gradientBrushCache.size() >= 64) gradientBrushCache.clear();
            gradientBrushCache.emplace(key, gradBrush);
        }
        if (!gradBrush) return;
        gradBrush->SetStartPoint(startPt);
        gradBrush->SetEndPoint(endPt);
        target->FillRoundedRectangle( D2D1::RoundedRect(snappedRect, radius, radius), gradBrush.Get());
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
            if (innerHighlightCache.size() >= 32) innerHighlightCache.clear();
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
            if (innerHighlightBrushCache.size() >= 32) innerHighlightBrushCache.clear();
            innerHighlightBrushCache.emplace(key, gradBrush);
        }
        if (gradBrush) {
            gradBrush->SetStartPoint(startPt);
            gradBrush->SetEndPoint(endPt);
            target->FillRectangle(lineRect, gradBrush.Get());
        }
    }
};

} // namespace Aperture
