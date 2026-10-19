#pragma once

#include "Common.h"

namespace Aperture::SplitIrisMenuLayout {
inline constexpr float Width = 238.0f;
inline constexpr float Center = Width * 0.5f;
inline constexpr float ChamberRadius = 38.0f;
inline constexpr float RowsTop = 169.0f;
inline constexpr float RowHeight = 29.0f;
inline constexpr float RowPitch = 30.0f;
inline constexpr float SeparatorHeight = 9.0f;

struct LabelRowBounds {
    float left, right;
    float Width() const { return std::max(0.0f, right - left); }
};

// Horizontal text rows must fit between both radial dividers and the curved
// rims. Intersect the limits at both ends of the row, with a visible gutter.
inline LabelRowBounds DialLabelRow(float centerDeg, float top, float bottom) {
    constexpr float pi = 3.14159265358979323846f, gutter = 2.5f;
    constexpr float outer = Center - 0.5f - gutter, inner = ChamberRadius + gutter;
    const float start = (centerDeg - 18.0f) * pi / 180.0f;
    const float end = (centerDeg + 18.0f) * pi / 180.0f;
    LabelRowBounds bounds{0.0f, Width};
    for (float y : {top, bottom}) {
        const float height = Center - y;
        if (height < 0.0f || height >= outer) return {Center, Center};
        const float extent = std::sqrt(outer * outer - height * height);
        bounds.left = std::max(bounds.left, Center - extent);
        bounds.right = std::min(bounds.right, Center + extent);
        if (centerDeg > -72.0f) bounds.left = std::max(bounds.left,
            Center + height * std::tan(start) + gutter / std::cos(start));
        if (centerDeg < 72.0f) bounds.right = std::min(bounds.right,
            Center + height * std::tan(end) - gutter / std::cos(end));
        if (height < inner) {
            const float chamber = std::sqrt(inner * inner - height * height);
            if (centerDeg < 0.0f) bounds.right = std::min(bounds.right, Center - chamber);
            else if (centerDeg > 0.0f) bounds.left = std::max(bounds.left, Center + chamber);
            else return {Center, Center};
        }
    }
    return bounds;
}

inline float Scale(float progress) {
    return 0.95f + 0.05f * std::clamp(progress, 0.0f, 1.0f);
}

template <class Items> float Bottom(const Items& items) {
    float bottom = RowsTop + 5.0f;
    for (const auto& item : items) bottom += item.isSeparator ? SeparatorHeight : RowPitch;
    if (!items.empty() && !items.back().isSeparator) bottom -= 1.0f;
    return bottom;
}

inline D2D1_POINT_2F Unscale(float x, float y, D2D1_POINT_2F origin, float dp, float progress) {
    const float cx = origin.x + Center * dp, cy = origin.y + Center * dp;
    const float scale = Scale(progress);
    return D2D1::Point2F(cx + (x - cx) / scale, cy + (y - cy) / scale);
}
} // namespace Aperture::SplitIrisMenuLayout
