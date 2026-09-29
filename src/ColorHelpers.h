#pragma once

// D2D color parsing and blending helpers.

#include "AppIncludes.h"
#include <string>

namespace Aperture {

// --- Color Helpers ---
inline D2D1_COLOR_F HexToD2DColor(const std::string& hex, float alpha = 1.0f) {
    std::string clean = hex;
    if (!clean.empty() && clean[0] == '#') clean = clean.substr(1);
    if (clean.length() == 6) {
        char* end = nullptr;
        unsigned long val = std::strtoul(clean.c_str(), &end, 16);
        float r = ((val >> 16) & 0xFF) / 255.0f;
        float g = ((val >> 8) & 0xFF) / 255.0f;
        float b = (val & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, alpha);
    }
    if (clean.length() == 8) {
        char* end = nullptr;
        unsigned long val = std::strtoul(clean.c_str(), &end, 16);
        float r = ((val >> 24) & 0xFF) / 255.0f;
        float g = ((val >> 16) & 0xFF) / 255.0f;
        float b = ((val >> 8) & 0xFF) / 255.0f;
        float a = (val & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, a * alpha);
    }
    return D2D1::ColorF(0.14f, 0.15f, 0.23f, alpha);
}

inline COLORREF D2DColorToColorRef(const D2D1_COLOR_F& col) {
    return RGB((BYTE)(col.r * 255.0f), (BYTE)(col.g * 255.0f), (BYTE)(col.b * 255.0f));
}

inline D2D1_COLOR_F ColorWithAlpha(const D2D1_COLOR_F& col, float alpha) { return D2D1::ColorF(col.r, col.g, col.b, alpha); }

inline D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF( a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t );
}

} // namespace Aperture
