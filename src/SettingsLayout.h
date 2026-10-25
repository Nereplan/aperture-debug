#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "FeatureGate.h"
#include "Presentation.h"

namespace Aperture::SettingsLayout {

// Shared DIP geometry for the native settings view. Keeping the visual, pointer, accessibility, and keyboard-scroll origins together
// prevents a styling-only change from moving controls away from their hit targets.
// Action ids: 11 check updates, 9 hover previews, 10 reduce motion, 13 smart series, 14 jump list,
// 0 open data, 2 clear cache, 7 clear history, 8 reset defaults,
// 15 register file types, 16 remove file types. The register/remove actions sit at the
// end so Simple mode can tail-truncate them along with the other advanced rows.
inline constexpr int kSystemActions[] = {11, 9, 10, 13, 14, 0, 2, 7, 8, 15, 16};
inline constexpr int kSystemRowCount = 11;
// Settings rows visible per page; Simple mode tail-truncates the advanced rows.
inline constexpr int kSystemSimpleRowCount = 9;
inline int PlaybackRowCount(bool simpleMode) { return simpleMode ? 7 : 14; }
inline int SystemRowCount(bool simpleMode) { return simpleMode ? kSystemSimpleRowCount : kSystemRowCount; }
// Settings content below this width renders the shortcuts list in a single
// full-width column and drops the Experience mode-tile radios.
inline bool ShortcutsSingleColumn(float contentW, float dp) { return contentW < 560.0f * dp; }
inline bool ExperienceNarrowTiles(float contentW, float dp) { return contentW < 540.0f * dp; }
inline constexpr float kSystemAboutHeight = 72.0f;
inline constexpr float kSystemActionsTop = 28.0f + kSystemAboutHeight + 18.0f + 28.0f;
inline constexpr float kSidebarWidth = 210.0f;
inline constexpr float kSidebarTopInset = 14.0f;
inline constexpr float kSidebarItemHeight = 42.0f;
inline constexpr float kSidebarItemGap = 6.0f;
inline constexpr float kContentPadX = 26.0f;
inline constexpr float kContentStartOffset = 22.0f;
inline constexpr float kPageHeaderReserve = 72.0f;
inline constexpr float kSubtitlePageHeaderReserve = 46.0f;

// Card metrics shared by rendering and hit-testing.
inline constexpr float kSectionCardGap = 16.0f;
inline constexpr float kSubtitleCardGap = 12.0f;
inline constexpr float kSectionCardRadius = 14.0f;
inline constexpr float kControlRadius = 8.0f;
inline constexpr float kControlHeight = 36.0f;
inline constexpr float kSubtitlePreviewCardHeight = 128.0f;
inline constexpr float kSubtitleBottomPadding = 8.0f;
inline constexpr float kSubtitleTrackCardHeight = 92.0f;
inline constexpr float kSubtitleFontCardHeight = 132.0f;
inline constexpr float kSubtitleColorCardHeight = 82.0f;
inline constexpr float kSubtitleStyleCardHeight = 148.0f;
inline constexpr float kSubtitlePositionCardHeight = 148.0f;

// Playback uses the same card-first rhythm as Audio & Subtitles. These metrics are shared by rendering, pointer hit-testing, accessibility,
// and keyboard autoscroll so controls cannot drift apart again.
inline constexpr float kPlaybackCardTitleHeight = 36.0f;
inline constexpr float kPlaybackBehaviorRowHeight = 46.0f;
inline constexpr float kPlaybackBehaviorCardHeight = kPlaybackCardTitleHeight + 4.0f * kPlaybackBehaviorRowHeight + 8.0f;
inline constexpr float kPlaybackSimpleCardHeight = 68.0f;
inline constexpr float kPlaybackSpeedCardHeight = 84.0f;
inline constexpr float kPlaybackMpvSwitchHeight = 50.0f;
inline constexpr float kPlaybackMpvActionHeight = 46.0f;
inline constexpr float kPlaybackMpvActionGap = 8.0f;
inline constexpr float kPlaybackMpvActionTop = kPlaybackCardTitleHeight + kPlaybackMpvSwitchHeight + 10.0f;
inline constexpr float kPlaybackMpvCardHeight = kPlaybackMpvActionTop + 3.0f * kPlaybackMpvActionHeight +
    2.0f * kPlaybackMpvActionGap + 8.0f;

// Experience: three mode tiles, a features heading, then one card per catalog
// group whose rows flow top-to-bottom through one or two columns. Rendering,
// pointer hit-testing, accessibility, keyboard autoscroll, and the scroll
// extent all read MakeExperienceLayout so they cannot drift apart.
inline constexpr float kExperienceTileHeight = 76.0f;
inline constexpr float kExperienceTileGap = 12.0f;
inline constexpr float kExperienceSectionHeight = 34.0f;
inline constexpr float kExperienceGroupTitleHeight = 52.0f;
inline constexpr float kExperienceFeatureRowHeight = 42.0f;
inline constexpr float kExperienceColumnGap = 16.0f;
inline constexpr float kExperienceGroupBottomPad = 10.0f;
inline constexpr float kExperienceTwoColumnMinWidth = 520.0f;

struct ExperienceGroupLayout {
    const char* id;
    size_t first;
    size_t count;
    D2D1_RECT_F card;
};

struct ExperienceLayout {
    std::array<D2D1_RECT_F, 3> modeTiles{};
    float sectionTop = 0.0f;
    std::vector<ExperienceGroupLayout> groups;
    std::array<D2D1_RECT_F, static_cast<size_t>(FeatureGate::FeatureId::Count)> rows{}; // catalog order
    float bottom = 0.0f;
};

inline ExperienceLayout MakeExperienceLayout(float contentLeft, float contentRight, float top, float dp, bool withFeatures = true) {
    ExperienceLayout layout;
    const float cardLeft = contentLeft - 10.0f * dp, cardRight = contentRight + 10.0f * dp;
    const float tileGap = kExperienceTileGap * dp;
    const float tileW = (cardRight - cardLeft - 2.0f * tileGap) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float left = cardLeft + i * (tileW + tileGap);
        layout.modeTiles[i] = D2D1::RectF(left, top, left + tileW, top + kExperienceTileHeight * dp);
    }
    if (!withFeatures) {
        layout.bottom = top + kExperienceTileHeight * dp;
        return layout;
    }
    layout.sectionTop = top + (kExperienceTileHeight + kSectionCardGap) * dp;
    const size_t columns = (contentRight - contentLeft) >= kExperienceTwoColumnMinWidth * dp ? 2 : 1;
    const float colGap = kExperienceColumnGap * dp;
    const float colW = (contentRight - contentLeft - static_cast<float>(columns - 1) * colGap) / static_cast<float>(columns);
    const float rowH = kExperienceFeatureRowHeight * dp;
    const auto& catalog = FeatureGate::Catalog();
    float y = layout.sectionTop + kExperienceSectionHeight * dp;
    for (size_t first = 0; first < catalog.size();) {
        size_t count = 1;
        while (first + count < catalog.size() && std::string_view(catalog[first + count].group) == catalog[first].group) ++count;
        const size_t perColumn = (count + columns - 1) / columns;
        const float rowsTop = y + kExperienceGroupTitleHeight * dp;
        for (size_t k = 0; k < count; ++k) {
            const float left = contentLeft + static_cast<float>(k / perColumn) * (colW + colGap);
            const float rowTop = rowsTop + static_cast<float>(k % perColumn) * rowH;
            layout.rows[first + k] = D2D1::RectF(left, rowTop, left + colW, rowTop + rowH);
        }
        const float cardBottom = rowsTop + static_cast<float>(perColumn) * rowH + kExperienceGroupBottomPad * dp;
        layout.groups.push_back({ catalog[first].group, first, count, D2D1::RectF(cardLeft, y, cardRight, cardBottom) });
        y = cardBottom + kSectionCardGap * dp;
        first += count;
    }
    layout.bottom = y - kSectionCardGap * dp;
    return layout;
}

// Content pane inside the hub view and the scrolling origin of its rows.
// Rendering, pointer hit-testing, and wheel hover re-resolution all share
// these so a drawn hover can never drift from its hit target.
inline D2D1_RECT_F ContentBounds(const D2D1_RECT_F& viewBounds, float dp) {
    return D2D1::RectF(viewBounds.left + kSidebarWidth * dp + 16.0f * dp, viewBounds.top,
                       viewBounds.right, viewBounds.bottom);
}
inline float ContentTop(const D2D1_RECT_F& contentBounds, float dp, float scrollY, float headerReserve = kPageHeaderReserve) {
    return contentBounds.top + (kContentStartOffset + headerReserve) * dp - scrollY;
}
}
