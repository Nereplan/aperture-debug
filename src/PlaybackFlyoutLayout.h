#pragma once

#include "GeometryHelpers.h"
#include <string_view>
#include <string>
#include <cmath>
#include <cwchar>
#include <cwctype>

namespace Aperture::PlaybackFlyoutLayout {

inline constexpr int None = -1, Back = -2;
inline constexpr double Speeds[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };

inline D2D1_RECT_F Header(const D2D1_RECT_F& bounds, float dp) {
    return D2D1::RectF(bounds.left + 4 * dp, bounds.top + 8 * dp,
        bounds.right - 4 * dp, bounds.top + 44 * dp);
}

inline D2D1_RECT_F SpeedTrack(const D2D1_RECT_F& bounds, float dp) {
    return D2D1::RectF(bounds.left + 24 * dp, bounds.top + 83 * dp,
        bounds.right - 24 * dp, bounds.top + 107 * dp);
}

inline D2D1_RECT_F SpeedInput(const D2D1_RECT_F& bounds, float dp) {
    const float center = (bounds.left + bounds.right) * 0.5f;
    return D2D1::RectF(center - 56 * dp, bounds.top + 50 * dp, center + 56 * dp, bounds.top + 82 * dp);
}

inline bool ParseSpeed(std::wstring text, double& result) {
    if (text.empty() || text.size() > 32) return false;
    for (auto& ch : text) if (ch == L',') ch = L'.';
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value) || value < 0.25 || value > 4.0) return false;
    while (std::iswspace(*end)) ++end;
    if (*end == L'x' || *end == L'X') ++end;
    while (std::iswspace(*end)) ++end;
    if (end != text.c_str() + text.size()) return false;
    result = value;
    return true;
}

inline D2D1_RECT_F Option(std::string_view menu, int index, const D2D1_RECT_F& bounds, float dp) {
    float left = bounds.left + 4 * dp, right = bounds.right - 4 * dp;
    float top = bounds.top + (menu == "main" ? 8 : menu == "queue" ? 78 : 50) * dp;
    float height = (menu == "main" ? 40 : 34) * dp;
    float pitch = (menu == "main" ? 42 : 36) * dp;
    if (menu == "speed" || menu == "audiodelay" || menu == "subdelay") {
        const float gap = (menu == "speed" ? 5 : 6) * dp;
        const float width = (RectWidth(bounds) - 28 * dp - 2 * gap) / 3;
        left = bounds.left + 14 * dp + (index % 3) * (width + gap);
        right = left + width;
        top = bounds.top + (menu == "speed" ? 108 : 96) * dp;
        height = (menu == "speed" ? 30 : 34) * dp;
        pitch = height + gap;
        index /= 3;
    }
    top += index * pitch;
    return D2D1::RectF(left, top, right, top + height);
}

inline int HitTest(std::string_view menu, int count, const D2D1_RECT_F& bounds,
    float x, float y, float dp) {
    if (!PtInRectF(bounds, x, y)) return None;
    if (menu != "main" && PtInRectF(Header(bounds, dp), x, y)) return Back;
    for (int i = 0; i < count; ++i) {
        const auto row = Option(menu, i, bounds, dp);
        if (row.top >= bounds.bottom) break;
        if (PtInRectF(row, x, y)) return i;
    }
    return None;
}

} // namespace Aperture::PlaybackFlyoutLayout
