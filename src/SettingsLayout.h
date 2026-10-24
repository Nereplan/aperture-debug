#pragma once

#include "Presentation.h"

namespace Aperture::SettingsLayout {

// Shared DIP geometry for the native settings view. Keeping the visual, pointer, accessibility, and keyboard-scroll origins together
// prevents a styling-only change from moving controls away from their hit targets.
// Action ids: 11 check updates, 9 hover previews, 10 reduce motion, 13 smart series, 14 jump list,
// 15 register file types, 16 remove file types, 0 open data, 2 clear cache, 7 clear history, 8 reset defaults.
inline constexpr int kSystemActions[] = {11, 9, 10, 13, 14, 15, 16, 0, 2, 7, 8};
inline constexpr int kSystemRowCount = 11;
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

// Experience uses the playback card rhythm for its mode picker and the
// Custom-mode feature card. The feature card height derives from the catalog
// feature count (37 features, 4 group headers) so the page scrolls correctly.
inline constexpr float kExperienceCardTitleHeight = kPlaybackCardTitleHeight;
inline constexpr float kExperienceFeatureRowHeight = kPlaybackBehaviorRowHeight;
inline constexpr float kExperienceGroupHeaderHeight = 28.0f;
inline constexpr float kExperienceFeatureCount = 37.0f;
inline constexpr float kExperienceFeaturesCardHeight = kExperienceCardTitleHeight +
    kExperienceFeatureCount * kExperienceFeatureRowHeight + 4.0f * kExperienceGroupHeaderHeight + 8.0f;
inline constexpr float kExperienceModeCardHeight = kExperienceCardTitleHeight + kControlHeight +
    8.0f + 18.0f + 8.0f;

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
