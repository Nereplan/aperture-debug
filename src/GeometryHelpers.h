#pragma once

// D2D rectangle predicates and metrics.

#include "AppIncludes.h"

namespace Aperture {

// --- Geometry & Rect Utilities ---
inline bool PtInRectF(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

inline D2D1_RECT_F InsetRectF(const D2D1_RECT_F& r, float dx, float dy) {
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}

inline float RectWidth(const D2D1_RECT_F& r) { return r.right - r.left; }

inline float RectHeight(const D2D1_RECT_F& r) { return r.bottom - r.top; }

} // namespace Aperture
