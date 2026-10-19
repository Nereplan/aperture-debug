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
