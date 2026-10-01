#include "App.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "CollectionAlgorithms.h"
#include "IpcProtocol.h"
#include "LibraryQuery.h"
#include "MediaFoundation.h"
#include "OnlineSubtitles.h"
#include "Reorder.h"
#include "Updater.h"
#include "Version.h"
#include "BuildInfo.h"
#include "ChannelPolicy.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "WindowGeometry.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <imm.h>
#include <numeric>

namespace Aperture {


bool App::UpdateHoverStates(float mouseX, float mouseY) {
    if (playbackSession.IsOpen() || IsAutoscrolling()) return false;
    bool changed = false;
    const auto setIfChanged = [&changed](auto& current, const auto& next) {
        if (current != next) { current = next; changed = true; }
    };
    float dp = renderer.GetDpiScale();
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    if (globalSearchModel.IsOpen()) {
        const float cardW = std::min(440.0f * dp, winW - 36.0f * dp);
        const float cardH = std::min(390.0f * dp, winH - 106.0f * dp);
        const float cardLeft = 18.0f * dp, cardTop = 76.0f * dp;
        const D2D1_RECT_F closeRect = D2D1::RectF(cardLeft + cardW - 42.0f * dp, cardTop + 10.0f * dp,
            cardLeft + cardW - 10.0f * dp, cardTop + 42.0f * dp);
        // Hit-testing only needs the borrowed result list; rebuilding the
        // owning projection copies every result on every mouse move.
        const std::vector<GlobalSearchResultItem>& searchResults = globalSearchModel.GetResults();
        int searchHover = -1;
        float itemY = cardTop + 52.0f * dp;
        for (size_t i = 0; i < searchResults.size(); ++i) {
            const D2D1_RECT_F itemRect = D2D1::RectF(cardLeft + 8.0f * dp, itemY, cardLeft + cardW - 8.0f * dp, itemY + 44.0f * dp);
            if (itemRect.bottom <= cardTop + cardH - 26.0f * dp && PtInRectF(itemRect, mouseX, mouseY)) {
                searchHover = static_cast<int>(i);
                break;
            }
            itemY += 46.0f * dp;
        }
        setIfChanged(hoveredGlobalSearchIndex, searchHover);
        setIfChanged(isGlobalSearchCloseHovered, PtInRectF(closeRect, mouseX, mouseY));
        setIfChanged(hoveredMediaIndex, -1);
        setIfChanged(isPinnedCloseHovered, false);
        setIfChanged(hoveredGroupBtn, -1);
        setIfChanged(hoveredMediaToolbarAction, -1);
        setIfChanged(hoveredMediaGroupHeader, -1);
        setIfChanged(hoveredFolderDeleteIndex, -1);
        setIfChanged(hoveredTab, (TabView)-1);
        setIfChanged(isNavOpenHovered, false);
        setIfChanged(isNavSettingsHovered, false);
        SetNavHoverTargets(false, false, false, false, false);
        return changed;
    }
    float pad = 18.0f * dp, padY = 14.0f * dp, navH = 48.0f * dp;
    // Check Top Nav Bar tabs with exact pixel-matching bounds (NO LOGO)
    float tabH = 32.0f * dp, tabY = padY + 4.0f * dp, iconSz = 14.0f * dp, gap = 7.0f * dp;
    float padPillX = 14.0f * dp, tabGap = 8.0f * dp, actionGap = 10.0f * dp;
    bool isRtl = Localization::Instance().IsRtl();
    float mediaW = iconSz + gap + renderer.MeasureString(Loc::Tr("nav.media"), FontType::Medium).width + padPillX * 2.0f;
    float playlistsW = iconSz + gap + renderer.MeasureString(Loc::Tr("nav.playlists"), FontType::Medium).width + padPillX * 2.0f;
    float historyW = iconSz + gap + renderer.MeasureString(Loc::Tr("nav.history"), FontType::Medium).width + padPillX * 2.0f;
    float settingsW = iconSz + gap + renderer.MeasureString(Loc::Tr("nav.settings"), FontType::Medium).width + padPillX * 2.0f;
    float openW = renderer.MeasureString(Loc::Tr("nav.open_file"), FontType::Medium).width + 32.0f * dp;
    float openX = isRtl ? pad : (winW - pad - openW);
    float settingsX = isRtl ? (openX + openW + actionGap) : (openX - actionGap - settingsW);
    TabView newHoveredTab = (TabView)-1;
    bool newNavOpenHovered = false, newNavSettingsHovered = false;
    if (mouseY >= tabY && mouseY <= tabY + tabH) {
        if (!isRtl) {
            float curX = pad;
            if (mouseX >= curX && mouseX <= curX + mediaW) {
                newHoveredTab = TabView::Media;
            } else if (mouseX >= curX + mediaW + tabGap && mouseX <= curX + mediaW + tabGap + playlistsW) {
                newHoveredTab = TabView::Playlists;
            } else if (mouseX >= curX + mediaW + tabGap + playlistsW + tabGap && mouseX <= curX + mediaW + tabGap + playlistsW + tabGap + historyW) {
                newHoveredTab = TabView::History;
            }
        } else {
            float curX = winW - pad;
            if (mouseX >= curX - mediaW && mouseX <= curX) {
                newHoveredTab = TabView::Media;
            } else if (mouseX >= curX - mediaW - tabGap - playlistsW && mouseX <= curX - mediaW - tabGap) {
                newHoveredTab = TabView::Playlists;
            } else if (mouseX >= curX - mediaW - tabGap - playlistsW - tabGap - historyW && mouseX <= curX - mediaW - tabGap - playlistsW - tabGap) {
                newHoveredTab = TabView::History;
            }
        }
        newNavOpenHovered = (mouseX >= openX && mouseX <= openX + openW);
        newNavSettingsHovered = (mouseX >= settingsX && mouseX <= settingsX + settingsW);
    }
    setIfChanged(hoveredTab, newHoveredTab);
    setIfChanged(isNavOpenHovered, newNavOpenHovered);
    setIfChanged(isNavSettingsHovered, newNavSettingsHovered);
    // Animated hover feedback for the top navigation. The AnimatedFloats are
    // ticked in Update(); without this the nav snaps between two states.
    SetNavHoverTargets(hoveredTab == TabView::Media, hoveredTab == TabView::Playlists,
                      hoveredTab == TabView::History, isNavSettingsHovered, isNavOpenHovered);
    if (isNavSettingsHovered) {
        AnimateVisual(navSettingsRotation, 45.0f, MotionTokens::kSettle, MotionTokens::Spring());
    } else { AnimateVisual(navSettingsRotation, 0.0f, MotionTokens::kSettle, MotionTokens::Spring()); }
    D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    if (currentTab == TabView::Media) {
        if (IsAutoscrolling()) {
            setIfChanged(hoveredMediaIndex, -1);
            setIfChanged(isPinnedCloseHovered, false);
            setIfChanged(hoveredGroupBtn, -1);
            setIfChanged(hoveredMediaToolbarAction, -1);
            setIfChanged(isMediaSearchClearHovered, false);
            return changed;
        }
        int newHoveredToolbarAction = -1;
        if (PtInRectF(mediaSearchBtnBounds, mouseX, mouseY)) {
            newHoveredToolbarAction = 0;
        } else if (PtInRectF(mediaSortBtnBounds, mouseX, mouseY)) { newHoveredToolbarAction = 1; }
        setIfChanged(hoveredMediaToolbarAction, newHoveredToolbarAction);
        bool newSearchClearHovered = (!mediaSearchInput.GetText().empty() && PtInRectF(mediaSearchClearBtnBounds, mouseX, mouseY));
        setIfChanged(isMediaSearchClearHovered, newSearchClearHovered);
        int newHoveredGroupBtn = -1;
        if (PtInRectF(mediaFlatBtnBounds, mouseX, mouseY)) {
            newHoveredGroupBtn = 0;
        } else if (PtInRectF(mediaSeriesBtnBounds, mouseX, mouseY)) { newHoveredGroupBtn = 1; }
        setIfChanged(hoveredGroupBtn, newHoveredGroupBtn);
        const auto& visibleIndices = GetVisibleMediaIndices();
        float tbH = 32.0f * dp, listTop = viewBounds.top + tbH + 12.0f * dp, cardLeft = viewBounds.left + 1.0f * dp;
        float cardRight = viewBounds.right - 2.0f * dp;
        int newHoveredIndex = -1;
        bool newPinnedCloseHovered = false;
        int newHoveredGroupHeader = -1, newHoveredFolderDelete = -1;
        if (libraryGroupMode != LibraryGroupMode::Flat) {
            const auto& proj = GetCurrentMediaProjection();
            if (mouseX >= cardLeft && mouseX <= cardRight && mouseY >= listTop && mouseY <= viewBounds.bottom) {
                float yDIP = (mouseY - listTop + mediaScrollY) / dp;
                newHoveredGroupHeader = HitTestMediaGroupHeader(yDIP, proj.groups.size());
                if (newHoveredGroupHeader >= 0) {
                    float delRight = cardRight - 8.0f * dp, delLeft = cardRight - 28.0f * dp;
                    if (mouseX >= delLeft && mouseX <= delRight) { newHoveredFolderDelete = newHoveredGroupHeader; }
                }
            }
        }
        setIfChanged(hoveredMediaGroupHeader, newHoveredGroupHeader);
        setIfChanged(hoveredFolderDeleteIndex, newHoveredFolderDelete);
        if (visibleIndices.empty()) {
            setIfChanged(hoveredMediaIndex, -1);
            int newHoveredEmptyBtn = -1;
            for (size_t i = 0; i < mediaEmptyBtnBounds.size(); ++i) {
                if (PtInRectF(mediaEmptyBtnBounds[i], mouseX, mouseY)) { newHoveredEmptyBtn = (int)i; break; }
            }
            if (newHoveredEmptyBtn != hoveredMediaEmptyBtn) { setIfChanged(hoveredMediaEmptyBtn, newHoveredEmptyBtn); }
        } else if (newHoveredGroupHeader != -1) { setIfChanged(hoveredMediaEmptyBtn, -1); newHoveredIndex = -1; } else {
            setIfChanged(hoveredMediaEmptyBtn, -1);
            if (mouseX >= cardLeft && mouseX <= cardRight && mouseY >= listTop && mouseY <= viewBounds.bottom) {
                float yDIP = (mouseY - listTop + mediaScrollY) / dp;
                newHoveredIndex = HitTestMediaRow(yDIP, visibleIndices.size());
            }
        }
        bool hasPinned = !isLastWatchedCleared && !visibleIndices.empty() && libraryGroupMode == LibraryGroupMode::Flat &&
            (visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0);
        if (newHoveredIndex == 0 && hasPinned) {
            float curY = listTop + (GetMediaRowTopDIP(0) * dp) - mediaScrollY;
            float itemH = GetMediaRowHeightDIP(0) * dp;
            float closeSize = 28.0f * dp, closeX = cardRight - 14.0f * dp - closeSize;
            float closeY = curY + (itemH - closeSize) * 0.5f;
            D2D1_RECT_F closeRect = D2D1::RectF(closeX, closeY, closeX + closeSize, closeY + closeSize);
            newPinnedCloseHovered = PtInRectF(closeRect, mouseX, mouseY);
        }
        setIfChanged(isPinnedCloseHovered, newPinnedCloseHovered);
        if (newHoveredIndex != hoveredMediaIndex) {
            int oldHovered = hoveredMediaIndex;
            setIfChanged(hoveredMediaIndex, newHoveredIndex);
            if (oldHovered >= 0 && oldHovered < (int)rowAnimStates.size()) {
                auto& row = rowAnimStates[oldHovered];
                bool isPinned = (hasPinned && oldHovered == 0);
                if (row.isHovering) {
                    row.isHovering = false;
                    row.hoverEnterTime = -1.0f;
                    float restH = isPinned ? kMediaPinnedRestHeight : kMediaRestHeight;
                    AnimateVisual(row.height, restH, MotionTokens::kResponsive, MotionTokens::Standard());
                    AnimateVisual(row.liftY, 0.0f, MotionTokens::kResponsive, MotionTokens::Spring());
                    AnimateVisual(row.previewOpacity, 0.0f, MotionTokens::kModerate, MotionTokens::Spring());
                    InvalidateMediaLayoutCache(oldHovered);
                    EnqueueActiveRowAnimation((size_t)oldHovered);
                }
            }
            if (newHoveredIndex >= 0 && newHoveredIndex < (int)rowAnimStates.size()) {
                auto& row = rowAnimStates[newHoveredIndex];
                if (!row.isHovering) {
                    row.isHovering = true;
                    row.hoverEnterTime = NowSeconds();
                    row.liftY.SnapTo(2.0f);
                    EnqueueActiveRowAnimation((size_t)newHoveredIndex);
                }
            }
        }
    }
    return changed;
}


int App::HitTestSystemSettingsRow(const D2D1_RECT_F& contentBounds, float contentY, float x, float y) {
    // Mirrors the System page renderer: action rows start kSystemActionsTop below the
    // page-header origin and repeat at 58 + 10 DIP. Pointer, mouse-down, and wheel
    // scrolling all share this so the drawn hover can never drift from the hit target.
    const float dp = renderer.GetDpiScale();
    const float padX = SettingsLayout::kContentPadX * dp;
    if (x < contentBounds.left + padX || x > contentBounds.right - padX) return -1;
    const float rowH = 58.0f * dp, rowGap = 10.0f * dp;
    const float relY = y - (contentY + SettingsLayout::kSystemActionsTop * dp);
    if (relY < 0.0f) return -1;
    const float pitch = rowH + rowGap;
    const int idx = static_cast<int>(relY / pitch);
    if (idx < 0 || idx >= SettingsLayout::kSystemRowCount) return -1;
    if ((relY - idx * pitch) > rowH) return -1;
    return idx;
}


void App::RefreshHubHitTargets(float mouseX, float mouseY) {
    if (!hWnd || playbackSession.IsOpen()) return;
    // WM_MOUSEMOVE owns the complete hub hit map (nav, cards, rails, settings, menus). Re-run it synchronously for the input event itself
    // so a click received without a preceding move cannot use stale hover state.
    int ix = std::clamp((int)std::lround(mouseX), -32768, 32767);
    int iy = std::clamp((int)std::lround(mouseY), -32768, 32767);
    SendMessage(hWnd, WM_MOUSEMOVE, 0, MAKELPARAM((short)ix, (short)iy));
}


void App::RefreshOverlayHitTargets(float mouseX, float mouseY) {
    if (!hWnd || !playbackSession.IsOpen()) return;
    float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    RECT rc{};
    GetClientRect(hWnd, &rc);
    float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp, padX = 20.0f * dp, btnSize = 36.0f * dp;
    hoveredPlayerButtonId = 0;
    isBackHovered = false;
    isVolumeHovered = false;
    hoveredFlyoutOption = -1;
    hoveredContextMenuItem = -1;
    // Menu item hover is presentation-independent: it must run before the PiP early-return
    // below or context-menu clicks in PiP would always see no hovered item and just close.
    if (isContextMenuOpen) {
        float menuW = 220.0f * dp, itemH = 32.0f * dp, curY = contextMenuOrigin.y + 4.0f * dp;
        for (size_t i = 0; i < contextMenuItems.size(); ++i) {
            if (contextMenuItems[i].isSeparator) { curY += 6.0f * dp; continue; }
            if (mouseX >= contextMenuOrigin.x && mouseX <= contextMenuOrigin.x + menuW && mouseY >= curY && mouseY <= curY + itemH) {
                hoveredContextMenuItem = (int)i;
                break;
            }
            curY += itemH;
        }
    }
    if (playbackSession.IsPiP()) {
        const auto pip = Presentation::PiP(winW, winH, dp);
        if (PtInRectF(pip.close, mouseX, mouseY)) hoveredPlayerButtonId = 3;
        else if (PtInRectF(pip.restore, mouseX, mouseY)) hoveredPlayerButtonId = 2;
        else if (PtInRectF(pip.pin, mouseX, mouseY)) hoveredPlayerButtonId = 7;
        else if (PtInRectF(pip.audio, mouseX, mouseY)) hoveredPlayerButtonId = 6;
        else if (PtInRectF(pip.next, mouseX, mouseY)) hoveredPlayerButtonId = 5;
        else if (PtInRectF(pip.previous, mouseX, mouseY)) hoveredPlayerButtonId = 4;
        else if (PtInRectF(pip.playPause, mouseX, mouseY)) hoveredPlayerButtonId = 1;
        isTimelineHovered = PtInRectF(pip.timelineHit, mouseX, mouseY);
        UpdateFlyoutHitTarget(mouseX, mouseY, winW, winH, dp, bottomBarH);
        return;
    }
    D2D1_RECT_F backBtn = D2D1::RectF(24.0f * dp, 16.0f * dp, 24.0f * dp + btnSize, 16.0f * dp + btnSize);
    isBackHovered = PtInRectF(backBtn, mouseX, mouseY);
    float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, trackCenterY = winH - bottomBarH + 16.0f * dp;
    bool overTrack = (mouseX >= barLeft && mouseX <= barRight &&
                      mouseY >= (winH - bottomBarH - 4.0f * dp) && mouseY <= (winH - bottomBarH + 26.0f * dp));
    bool prevTimelineHovered = isTimelineHovered;
    isTimelineHovered = overTrack;
    if (isTimelineHovered != prevTimelineHovered && !IsReducedMotionEnabled())
        AnimateVisual(timelineHoverAnim, isTimelineHovered ? 1.0f : 0.0f, MotionTokens::kFast, MotionTokens::Standard());
    if (!overTrack) { if (previewPlayer.IsSeekbarActive()) previewPlayer.ResetSeekbarPreview(); }
    const bool queuePlayback = playbackSession.IsQueuePlayback();
    const auto layout = Presentation::Player(winW, winH, dp, queuePlayback);
    const std::pair<int, D2D1_RECT_F> buttons[] = {
        {1, layout.play}, {2, layout.next}, {3, layout.mute}, {5, layout.settings}, {7, layout.fullscreen}
    };
    for (const auto& button : buttons) { if (PtInRectF(button.second, mouseX, mouseY)) hoveredPlayerButtonId = button.first; }
    if (queuePlayback && PtInRectF(layout.prev, mouseX, mouseY)) hoveredPlayerButtonId = 8;
    if (layout.showPip && PtInRectF(layout.pip, mouseX, mouseY)) hoveredPlayerButtonId = 6;
    if (layout.showSubtitles && PtInRectF(layout.subtitles, mouseX, mouseY)) hoveredPlayerButtonId = 4;
    isVolumeHovered = layout.showVolume && PtInRectF(InsetRectF(layout.volume, -10.0f * dp, -8.0f * dp), mouseX, mouseY);
    UpdateFlyoutHitTarget(mouseX, mouseY, winW, winH, dp, bottomBarH);
    // Jump ahead button hit test
    if (playbackSession.IsOpen() && !playbackSession.IsPiP() && jumpAheadAnim.current > 0.1f) {
        float btnW = 148.0f * dp;
        float btnH = 42.0f * dp;
        float rightMargin = 24.0f * dp;
        float slideOffset = (1.0f - jumpAheadAnim.current) * 8.0f * dp;
        float bottomY = winH - bottomBarH - 16.0f * dp + slideOffset;
        float topY = bottomY - btnH;
        float rightX = winW - rightMargin;
        float leftX = rightX - btnW;
        jumpAheadBounds = D2D1::RectF(leftX, topY, rightX, bottomY);
        isJumpAheadHovered = PtInRectF(jumpAheadBounds, mouseX, mouseY);
    } else {
        isJumpAheadHovered = false;
        jumpAheadBounds = { 0, 0, 0, 0 };
    }
}


void App::OnMouseLeave() {
    isMouseTracking = false;
    isOverlayMouseTracking = false;
    isVideoMouseTracking = false;
    lastPlayerMouseX = -1.0f;
    lastPlayerMouseY = -1.0f;
    hubMouseX = -1.0f;
    hubMouseY = -1.0f;
    hoveredMediaIndex = -1;
    hoveredPlaylistDetailItemIndex = -1;
    ClearPlaylistCardHover();
    hoveredPlaylistDetailDeleteIndex = -1;
    hoveredHistoryIndex = -1;
    hoveredHistoryDeleteIndex = -1;
    hoveredTab = (TabView)-1;
    isNavOpenHovered = false;
    isNavSettingsHovered = false;
    SetNavHoverTargets(false, false, false, false, false);
    isTimelineHovered = false;
    isPinnedCloseHovered = false;
    isClearHistoryHovered = false;
    isModalOkHovered = false;
    isModalCancelHovered = false;
    hoveredSettingsCat = (SettingsCategory)-1;
    hoveredSettingsRow = -1;
    hoveredShortcutIndex = -1;
    isResetShortcutsHovered = false;
    isJumpAheadHovered = false;
    if (GetCapture() != hWnd) {
        settingsPressState = {};
        isClearHistoryPressed = false;
        isModalOkPressed = false;
        isModalCancelPressed = false;
        isJumpAheadPressed = false;
        pressedNavIndex = -1;
    }
    bool hasPinned = !isLastWatchedCleared && !library.mediaItems.empty() && libraryGroupMode == LibraryGroupMode::Flat &&
        (library.mediaItems[0].lastWatchedTimestamp > 0);
    for (size_t idx : m_activeAnimRowIndices) {
        if (idx < rowAnimStates.size()) {
            auto& row = rowAnimStates[idx];
            bool isPinned = (hasPinned && idx == 0);
            if (row.isHovering) {
                row.isHovering = false;
                row.hoverEnterTime = -1.0f;
                float restH = isPinned ? kMediaPinnedRestHeight : kMediaRestHeight;
                AnimateVisual(row.height, restH, MotionTokens::kResponsive, MotionTokens::Standard());
                AnimateVisual(row.liftY, 0.0f, MotionTokens::kResponsive, MotionTokens::Spring());
                AnimateVisual(row.previewOpacity, 0.0f, MotionTokens::kModerate, MotionTokens::Spring());
                InvalidateMediaLayoutCache(idx);
                EnqueueActiveRowAnimation(idx);
            }
        }
    }
    previewPlayer.StopPreview();
}

} // namespace Aperture
