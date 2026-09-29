#pragma once

#include "Common.h"

namespace Aperture {

// Shared by drawing, hit-testing, scrolling, and accessibility.
inline constexpr float kMediaPinnedRestHeight = 92.0f;
inline constexpr float kMediaRestHeight = 76.0f;

// Reuse the unchanged prefix during card expansion. Recalculate from the
// first changed row instead of repeatedly adding deltas (which accumulates
// floating-point drift across hover reversals).
template <typename HeightAt>
float UpdateFlatMediaRowOffsets(std::vector<float>& offsets, size_t count, size_t firstChanged, HeightAt heightAt) {
    if (offsets.size() != count + 1) firstChanged = 0;
    offsets.resize(count + 1);
    firstChanged = std::min(firstChanged, count);
    float y = firstChanged == 0 ? 0.0f : offsets[firstChanged];
    for (size_t i = firstChanged; i < count; ++i) {
        offsets[i] = y;
        y += heightAt(i) + 14.0f;
    }
    offsets[count] = y;
    return y;
}

struct MediaToolbarGeometry {
    D2D1_RECT_F search = { 0, 0, 0, 0 };
    D2D1_RECT_F searchClear = { 0, 0, 0, 0 };
    D2D1_RECT_F sort = { 0, 0, 0, 0 };
    D2D1_RECT_F group = { 0, 0, 0, 0 };
    D2D1_RECT_F flat = { 0, 0, 0, 0 };
    D2D1_RECT_F folder = { 0, 0, 0, 0 };
    D2D1_RECT_F series = { 0, 0, 0, 0 };
    D2D1_RECT_F duplicates = { 0, 0, 0, 0 };
    D2D1_RECT_F summary = { 0, 0, 0, 0 };
    bool compact = false, showSummary = false;
};

inline MediaToolbarGeometry ComputeMediaToolbarGeometry(const D2D1_RECT_F& toolbar, float dp) {
    MediaToolbarGeometry g;
    const float width = std::max(0.0f, toolbar.right - toolbar.left);
    g.compact = width < 700.0f * dp;
    const float actionGap = 10.0f * dp, groupGap = 10.0f * dp;
    const float sortW = (g.compact ? 72.0f : 132.0f) * dp;
    const float minGroupW = (g.compact ? 136.0f : 172.0f) * dp;
    const float groupW = std::min(200.0f * dp, std::max(minGroupW, width * 0.26f));
    g.group = D2D1::RectF(toolbar.right - groupW, toolbar.top, toolbar.right, toolbar.bottom);
    g.sort = D2D1::RectF(g.group.left - groupGap - sortW, toolbar.top, g.group.left - groupGap, toolbar.bottom);
    // Compute search width flexibly: standard width is 230dp, scaling down if space is tight
    const float maxSearchW = std::max(40.0f * dp, g.sort.left - groupGap - toolbar.left);
    const float searchTargetW = (g.compact ? 170.0f : 230.0f) * dp;
    const float searchW = std::min(maxSearchW, searchTargetW);
    g.search = D2D1::RectF(toolbar.left, toolbar.top, toolbar.left + searchW, toolbar.bottom);
    // Clear button within search box
    const float clearSz = 18.0f * dp, clearRight = g.search.right - 6.0f * dp;
    const float clearTop = g.search.top + (RectHeight(g.search) - clearSz) * 0.5f;
    g.searchClear = D2D1::RectF(clearRight - clearSz, clearTop, clearRight, clearTop + clearSz);
    const float segW = std::max(1.0f * dp, (groupW - 4.0f * dp) / 2.0f);
    g.flat = D2D1::RectF(g.group.left + 2.0f * dp, g.group.top + 2.0f * dp, g.group.left + 2.0f * dp + segW, g.group.bottom - 2.0f * dp);
    g.series = D2D1::RectF(g.flat.right, g.group.top + 2.0f * dp, g.group.right - 2.0f * dp, g.group.bottom - 2.0f * dp);
    g.folder = D2D1::RectF(0, 0, 0, 0);
    g.duplicates = D2D1::RectF(0, 0, 0, 0);
    g.summary = D2D1::RectF(g.search.right + actionGap, toolbar.top,
        std::max(g.search.right + actionGap, g.sort.left - 10.0f * dp), toolbar.bottom);
    g.showSummary = (g.summary.right - g.summary.left) >= 80.0f * dp;
    return g;
}

inline LONG AccessibilityStableId(LONG prefix, const std::wstring& key) {
    uint32_t hash = 2166136261u;
    for (wchar_t ch : key) { hash ^= static_cast<uint32_t>(ch); hash *= 16777619u; }
    return prefix + static_cast<LONG>(hash % 900000u);
}

} // namespace Aperture
