#include "App.h"
#include "Reorder.h"
#include "AppLayout.h"
#include "Presentation.h"
#include <ctime>
#include <unordered_map>

namespace Aperture {

namespace {
D2D1_RECT_F IslandRectUnion(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF(std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom));
}
} // namespace

bool App::IsMediaFileMissing(const std::wstring& path) {
    if (path.empty()) return true;
    const auto observation = pathAvailability.Lookup(path);
    return observation.known && (observation.value != PathAvailability::Present || observation.isDirectory);
}

const std::vector<MpvChapterInfo>& App::GetCachedChapters() const {
    // Chapters only change on load or a chapter-list event; the version check
    // keeps every-frame consumers from re-copying the vector under stateMutex.
    const uint64_t version = player.GetChaptersVersion();
    if (version != m_chaptersCacheVersion) {
        m_cachedChapters = player.GetChapters();
        m_chaptersCacheVersion = version;
    }
    return m_cachedChapters;
}

const std::vector<MpvTrackInfo>& App::GetCachedSubtitleTracks() const {
    const uint64_t version = player.GetTracksVersion();
    if (version != m_subTracksCacheVersion) {
        m_cachedSubTracks = player.GetSubtitleTracks();
        m_subTracksCacheVersion = version;
    }
    return m_cachedSubTracks;
}

const std::vector<MpvTrackInfo>& App::GetCachedAudioTracks() const {
    const uint64_t version = player.GetTracksVersion();
    if (version != m_audioTracksCacheVersion) {
        m_cachedAudioTracks = player.GetAudioTracks();
        m_audioTracksCacheVersion = version;
    }
    return m_cachedAudioTracks;
}

std::shared_ptr<const std::vector<TimelineChapterMarker>> App::GetChapterMarkersSnapshot() const {
    const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
    // Use the version of this copied chapter list. The player can publish a
    // newer version between calls; it must not label an older list as current.
    return m_chapterMarkersCache.Get(chapters, m_chaptersCacheVersion, player.GetDuration(), player.GetCurrentTime());
}

const HistoryTimelineProjection& App::GetHistoryProjection() {
    // Group buckets depend on the wall-clock day; ordering and membership only
    // change when history changes, so the projection is stable between
    // those events instead of being rebuilt (and re-sorted) every frame.
    const int64_t nowSec = GetCurrentTimestampSec();
    const std::time_t ttNow = (std::time_t)nowSec;
    std::tm tmNow{};
    localtime_s(&tmNow, &ttNow);
    const int64_t day = tmNow.tm_year * 366 + tmNow.tm_yday;
    if (m_historyProjectionVersion == library.GetRevisions().history && m_historyProjectionDay == day &&
        m_historyProjectionSize == library.history.size()) {
        return m_historyProjection;
    }
    m_historyProjection = ProjectHistoryTimeline(library.history);
    m_historyProjectionVersion = library.GetRevisions().history;
    m_historyProjectionDay = day;
    m_historyProjectionSize = library.history.size();
    return m_historyProjection;
}

PlayerRenderViewModel App::BuildPlayerRenderViewModel() const {
    PlayerRenderViewModel vm;
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    vm.winBounds = D2D1::RectF(0, 0, winW, winH);
    vm.dpiScale = overlayRenderer.GetDpiScale();
    float dp = vm.dpiScale;
    vm.isPiP = playbackSession.IsPiP();
    // 1. Subtitles
    if (subtitles.GetActiveTrack()) {
        vm.subtitle.text = subtitles.GetCurrentSubtitleText(player.GetCurrentTime(), settings.subDelayMs);
        if (!vm.subtitle.text.empty()) {
            vm.subtitle.visible = true;
            vm.subtitle.bounds = vm.winBounds;
            vm.subtitle.bgStyle = settings.subBgStyle;
            vm.subtitle.textColor = settings.GetSubTextColor();
            vm.subtitle.fontSizePoints = settings.GetSubFontSizePoints();
            vm.subtitle.position = settings.subPosition;
        }
    }
    // 2. Double-tap Seek Feedback Animation
    const bool reducedMotion = IsReducedMotionEnabled();
    vm.doubleTap.bounds = vm.winBounds;
    vm.doubleTap.isPiP = vm.isPiP;
    if (doubleTapTimer > 0.0f) {
        vm.doubleTap.visible = true;
        vm.doubleTap.isForward = doubleTapIsForward;
        vm.doubleTap.totalSeconds = doubleTapAccumSec;
        if (!reducedMotion) vm.doubleTap.chevronSlides = doubleTapChevronSlides;
        vm.doubleTap.signPop = reducedMotion ? 1.0f : doubleTapSignPop;
        vm.doubleTap.numberPop = reducedMotion ? 1.0f : doubleTapNumberPop;
        vm.doubleTap.appear = reducedMotion ? 1.0f : doubleTapAppear;
        // The final 0.22s of the window is the ease-out exit: fade + slight scale-down + 8dp drift toward the seek direction.
        vm.doubleTap.fadeOut = std::clamp(1.0f - doubleTapTimer / kSeekOsdFadeSec, 0.0f, 1.0f);
    }
    if (doubleTapFadeValid && !reducedMotion) {
        vm.doubleTap.visible = true;
        vm.doubleTap.fadeSideVisible = true;
        vm.doubleTap.fadeSideForward = doubleTapFadeForward;
        vm.doubleTap.fadeSideSeconds = doubleTapFadeSeconds;
        vm.doubleTap.fadeSideChevronSlides = doubleTapFadeChevronSlides;
        vm.doubleTap.fadeSideSignPop = doubleTapFadeSignPop;
        vm.doubleTap.fadeSideNumberPop = doubleTapFadeNumberPop;
        vm.doubleTap.fadeSideAppear = doubleTapFadeAppear;
        vm.doubleTap.fadeSideFadeOut = doubleTapFadeExitStart + (1.0f - doubleTapFadeExitStart) * doubleTapFadeProgress;
    }
    // 3. Center Play/Pause Ripple Badge
    if (playPauseAnimTimer > 0.0f) {
        vm.playPauseBadge.visible = true;
        vm.playPauseBadge.isPlay = playPauseAnimIsPlay;
        vm.playPauseBadge.animProgress = IsReducedMotionEnabled() ? 1.0f : 1.0f - (playPauseAnimTimer / MotionTokens::kPlayPauseBadge);
        vm.playPauseBadge.bounds = vm.winBounds;
    }
    // 4. Controls
    float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp;
    std::wstring mediaTitle = GetFileTitleFromPath(player.GetCurrentFilePath());
    // Keep app-rendered subtitles clear of the transport/timeline while the controls are visible. Fade the reserved space with the controls
    // so the subtitle position settles naturally once the overlay disappears.
    if (vm.subtitle.visible && !vm.isPiP) {
        const float controlsInset = bottomBarH * std::clamp(controlsOpacity, 0.0f, 1.0f);
        vm.subtitle.bounds.bottom = std::max(vm.subtitle.bounds.top, vm.subtitle.bounds.bottom - controlsInset);
    }
    if (vm.isPiP) {
        vm.pipOverlay.visible = true;
        vm.pipOverlay.bounds = vm.winBounds;
        vm.pipOverlay.title = mediaTitle;
        vm.pipOverlay.isPlaying = player.IsPlaying();
        vm.pipOverlay.currentTime = player.GetCurrentTime();
        vm.pipOverlay.duration = player.GetDuration();
        vm.pipOverlay.isScrubbing = isScrubbing;
        vm.pipOverlay.canPlayPrevious = playbackSession.IsQueuePlayback() && (playbackSession.GetQueueIndex() > 0 || settings.loop);
        vm.pipOverlay.canPlayNext = playbackSession.IsQueuePlayback() && playbackSession.GetQueueSize() > 1;
        vm.pipOverlay.isPinned = playbackSession.IsPiPPinned();
        vm.pipOverlay.hoveredButtonId = hoveredPlayerButtonId;
        vm.pipOverlay.focusedControlId = playerKeyboardFocusId;
        vm.pipOverlay.showKeyboardFocus = isKeyboardNavActive;
        vm.pipOverlay.opacity = controlsOpacity;
        vm.pipOverlay.isVolumeSliderOpen = isPiPVolumeSliderOpen;
        vm.pipOverlay.volume = (float)player.GetVolume();
        vm.pipOverlay.isMuted = player.IsMuted() || player.GetVolume() <= 0.001;
        vm.pipOverlay.volumeSliderAnim = pipVolumeAnim.current;
    } else {
        vm.topBar.visible = true;
        vm.topBar.bounds = D2D1::RectF(0, 0, vm.winBounds.right, topBarH);
        vm.topBar.title = mediaTitle;
        // The title alone identifies what is playing; codec/resolution chips
        // read as clutter on the overlay.
        vm.topBar.isBackHovered = isBackHovered;
        vm.topBar.focusedControlId = playerKeyboardFocusId;
        vm.topBar.showKeyboardFocus = isKeyboardNavActive;
        vm.topBar.opacity = controlsOpacity;
        vm.bottomBar.visible = true;
        vm.bottomBar.bounds = D2D1::RectF(0, vm.winBounds.bottom - bottomBarH, vm.winBounds.right, vm.winBounds.bottom);
        vm.bottomBar.hoveredButtonId = hoveredPlayerButtonId;
        vm.bottomBar.focusedControlId = playerKeyboardFocusId;
        vm.bottomBar.showKeyboardFocus = isKeyboardNavActive;
        vm.bottomBar.opacity = controlsOpacity;
        if (controlsOpacity > 0.001f) {
            const auto& subTracks = GetCachedSubtitleTracks();
            bool isSubtitlesActive = subtitles.GetActiveTrack() != nullptr;
            for (const auto& track : subTracks) {
                if (track.isSelected) { isSubtitlesActive = true; break; }
            }
            vm.bottomBar.isPlaying = player.IsPlaying();
            vm.bottomBar.currentTime = (isScrubbing && scrubTargetTime >= 0.0) ? scrubTargetTime : player.GetCurrentTime();
            vm.bottomBar.duration = player.GetDuration();
            vm.bottomBar.cacheDuration = player.GetCacheDuration();
            vm.bottomBar.isScrubbing = isScrubbing;
            vm.bottomBar.volume = player.GetVolume();
            vm.bottomBar.isMuted = player.IsMuted();
            vm.bottomBar.isVolumeSliderHovered = (isVolumeHovered || isVolumeScrubbing);
            vm.bottomBar.isTimelineHovered = isTimelineHovered;
            vm.bottomBar.timelineHoverX = (isTimelineHovered && !isScrubbing) ? lastPlayerMouseX : -1.0f;
            vm.bottomBar.timelineHoverProgress = (timelineHoverAnim.animating || timelineHoverAnim.current > 0.0f) ? timelineHoverAnim.current
                : ((isTimelineHovered || isScrubbing) ? 1.0f : 0.0f);
            vm.bottomBar.isSubtitlesActive = isSubtitlesActive;
            vm.bottomBar.isFullscreen = playbackSession.IsFullscreen();
            vm.bottomBar.isPiP = playbackSession.IsPiP();
            vm.bottomBar.isQueuePlayback = playbackSession.IsQueuePlayback();
            vm.bottomBar.isFirstQueueButtonDisabled = playbackSession.IsQueuePlayback() && playbackSession.GetQueueIndex() == 0 && !settings.loop;
            const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
            double duration = vm.bottomBar.duration;
            double currentTime = vm.bottomBar.currentTime;
            int curChap = player.GetCurrentChapter();
            std::wstring currentChapterTitle;
            if (curChap >= 0 && curChap < (int)chapters.size()) {
                currentChapterTitle = chapters[curChap].title;
            } else if (!chapters.empty() && duration > 0.0) {
                for (int i = (int)chapters.size() - 1; i >= 0; --i) {
                    if (chapters[i].time <= currentTime + 0.25) { currentChapterTitle = chapters[i].title; break; }
                }
            }
            vm.bottomBar.currentChapterTitle = currentChapterTitle;
            vm.topBar.chapterTitle = currentChapterTitle;
            vm.bottomBar.chapterMarkers = GetChapterMarkersSnapshot();
        }
    }
    // 6. Settings Flyout
    if (isSettingsFlyoutOpen || flyoutClosing) {
        vm.settingsFlyout.isOpen = true;
        float flyoutW = 280.0f * dp;
        float flyoutH = GetFlyoutHeight();
        vm.settingsFlyout.bounds = D2D1::RectF(vm.winBounds.right - flyoutW - 20.0f * dp, vm.winBounds.bottom - bottomBarH - flyoutH - 10.0f * dp, vm.winBounds.right - 20.0f * dp, vm.winBounds.bottom - bottomBarH - 10.0f * dp);
        vm.settingsFlyout.currentMenu = currentFlyoutMenu;
        vm.settingsFlyout.hoveredOptionIndex = hoveredFlyoutOption;
        vm.settingsFlyout.subTracks = GetCachedSubtitleTracks();
        vm.settingsFlyout.activeSubIndex = -1;
        for (size_t i = 0; i < vm.settingsFlyout.subTracks.size(); ++i) {
            if (vm.settingsFlyout.subTracks[i].isSelected && settings.subtitlesEnabled && player.GetSubtitleVisibility()) { vm.settingsFlyout.activeSubIndex = (int)i; break; }
        }
        vm.settingsFlyout.audioTracks = GetCachedAudioTracks();
        vm.settingsFlyout.activeAudioIndex = -1;
        for (size_t i = 0; i < vm.settingsFlyout.audioTracks.size(); ++i) {
            if (vm.settingsFlyout.audioTracks[i].isSelected) { vm.settingsFlyout.activeAudioIndex = (int)i; break; }
        }
        vm.settingsFlyout.currentSpeed = isPlaybackSpeedDragging ? playbackSpeedDragValue : player.GetPlaybackRate();
        vm.settingsFlyout.speedInput.displayText = Text16ToWideString(playbackSpeedInput.GetDisplayText());
        vm.settingsFlyout.speedInput.selectionStart = playbackSpeedInput.GetSelection().start;
        vm.settingsFlyout.speedInput.selectionEnd = playbackSpeedInput.GetSelection().end;
        vm.settingsFlyout.speedInput.caretIndex = playbackSpeedInput.GetCaret();
        vm.settingsFlyout.speedInput.focused = activeTextInputField == TextInputField::PlaybackSpeed;
        vm.settingsFlyout.speedInput.caretVisible = vm.settingsFlyout.speedInput.focused && textInputCaretVisible;
        vm.settingsFlyout.currentAspect = settings.lastAspectRatio;
        vm.settingsFlyout.isLoop = settings.loop;
        vm.settingsFlyout.isAutoplay = settings.autoplay;
        vm.settingsFlyout.isPrivateMode = playbackSession.IsPrivateMode();
        vm.settingsFlyout.pQueueItems = &playbackSession.GetQueue();
        vm.settingsFlyout.queueIndex = playbackSession.GetQueueIndex();
        vm.settingsFlyout.queueFirstVisible = queueFlyoutFirstVisible;
        vm.settingsFlyout.animProgress = flyoutAnim.current;
    }
    // 7. OSD Badge
    if (osdTimer > 0.0f) {
        vm.osdBadge.visible = true;
        vm.osdBadge.isPiP = vm.isPiP;
        float osdW = vm.isPiP ? 160.0f * dp : 240.0f * dp;
        float osdH = vm.isPiP ? 26.0f * dp : 44.0f * dp;
        float osdTop = vm.isPiP ? 10.0f * dp : 75.0f * dp;
        vm.osdBadge.bounds = D2D1::RectF((vm.winBounds.right - osdW) * 0.5f, osdTop, (vm.winBounds.right + osdW) * 0.5f, osdTop + osdH);
        vm.osdBadge.opacity = IsReducedMotionEnabled() ? 1.0f : std::min(1.0f, osdTimer / MotionTokens::kModerate);
        vm.osdBadge.icon = osdIcon;
        vm.osdBadge.text = osdText;
    }
    // 7b. Diagnostics
    if (showPlaybackDiagnostics) {
        vm.diagnosticsOverlay.visible = true;
        vm.diagnosticsOverlay.closeHovered = IsPlaybackDiagnosticsCloseHit(lastPlayerMouseX, lastPlayerMouseY);
        vm.diagnosticsOverlay.bounds = vm.winBounds;
        vm.diagnosticsOverlay.diagnostics = player.GetDiagnostics();
        vm.diagnosticsOverlay.audioDelayMs = settings.audioDelayMs;
        vm.diagnosticsOverlay.subDelayMs = settings.subDelayMs;
    }
    // 7b-2. Diagnostics Log (mirrored each frame like the diagnostics overlay).
    if (showLogViewer) {
        vm.logOverlay.visible = true;
        vm.logOverlay.closeHovered = IsLogViewerCloseHit(lastPlayerMouseX, lastPlayerMouseY);
        vm.logOverlay.bounds = vm.winBounds;
        vm.logOverlay.lines = logViewerLines;
    }
    // 7c. Jump Ahead Button
    if (playbackSession.IsOpen() && !vm.isPiP && jumpAheadAnim.current > 0.001f) {
        vm.jumpAhead.visible = true;
        vm.jumpAhead.opacity = jumpAheadAnim.current;
        vm.jumpAhead.isHovered = isJumpAheadHovered;
        vm.jumpAhead.isPressed = isJumpAheadPressed;
        vm.jumpAhead.scale = isJumpAheadPressed ? 0.98f : 1.0f;
        vm.jumpAhead.text = L"Jump ahead";

        float btnW = 148.0f * dp;
        float btnH = 42.0f * dp;
        float rightMargin = 24.0f * dp;
        float slideOffset = (1.0f - jumpAheadAnim.current) * 8.0f * dp;
        float bottomY = winH - bottomBarH - 16.0f * dp + slideOffset;
        float topY = bottomY - btnH;
        float rightX = winW - rightMargin;
        float leftX = rightX - btnW;

        vm.jumpAhead.bounds = D2D1::RectF(leftX, topY, rightX, bottomY);
    }
    // 8. Context Menu
    if (isContextMenuOpen || contextMenuClosing) {
        PopulateContextMenuViewModel(vm.contextMenu);
    }
    vm.modal = BuildModalViewModel();
    if (inPresentationTransition) {
        vm.pipOverlay.visible = false;
        vm.topBar.visible = false;
        vm.bottomBar.visible = false;
        vm.jumpAhead.visible = false;
        vm.contextMenu.isOpen = false;
        vm.settingsFlyout.isOpen = false;
    }
    return vm;
}

ModalViewModel App::BuildModalViewModel() const {
    ModalViewModel vm;
    if (activeModal != ModalType::None) {
        vm.type = activeModal;
        vm.title = modalTitle;
        vm.prompt = modalPrompt;
        vm.message = modalMessage;
        vm.confirmText = modalConfirmText;
        vm.isDestructive = modalIsDestructive;
        const bool confirmation = activeModal == ModalType::Confirmation || activeModal == ModalType::DeletePlaylist;
        vm.isOkHovered = isModalOkHovered || modalKeyboardFocus == (confirmation ? 0 : 1);
        vm.isCancelHovered = isModalCancelHovered || modalKeyboardFocus == (confirmation ? 1 : 2);
        vm.isOkPressed = isModalOkPressed;
        vm.isCancelPressed = isModalCancelPressed;
        if (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist || activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput || activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput || activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides || activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile || activeModal == ModalType::JumpToTime || activeModal == ModalType::CustomAspectInput || activeModal == ModalType::CustomRotationInput) {
            vm.visualState.displayText = Text16ToWideString(modalInput.GetDisplayText());
            vm.visualState.selectionStart = modalInput.GetSelection().start;
            vm.visualState.selectionEnd = modalInput.GetSelection().end;
            vm.visualState.caretIndex = modalInput.GetCaret();
            vm.visualState.focused = (activeTextInputField == TextInputField::Modal);
            vm.visualState.caretVisible = vm.visualState.focused && textInputCaretVisible;
            if (modalInput.IsComposing()) {
                const auto compositionRange = modalInput.GetCompositionRange();
                vm.visualState.hasCompositionRange = true;
                vm.visualState.compositionStart = compositionRange.start;
                vm.visualState.compositionEnd = compositionRange.end;
                vm.visualState.compositionCaretIndex = modalInput.GetDisplayCaret();
            }
        }
    }
    return vm;
}

HubRenderViewModel App::BuildHubRenderViewModel() {
    HubRenderViewModel vm;
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    vm.winBounds = D2D1::RectF(0, 0, winW, winH);
    vm.dpiScale = renderer.GetDpiScale();
    float dp = vm.dpiScale;
    vm.currentTab = currentTab;
    float padX = 18.0f * dp, padY = 14.0f * dp, navH = 48.0f * dp;
    // 1. Top Nav Bar
    vm.navBar.bounds = D2D1::RectF(padX, padY, vm.winBounds.right - padX, padY + navH);
    vm.navBar.activeTab = currentTab;
    vm.navBar.hoveredTab = hoveredTab;
    vm.navBar.isSettingsHovered = isNavSettingsHovered;
    vm.navBar.isOpenHovered = isNavOpenHovered;
    vm.navBar.settingsRotationDegrees = navSettingsRotation.current;
    vm.navBar.isPlaylistsNavDropHovered = isPlaylistsNavDropHovered;
    vm.navBar.focusedNavIndex = (activeFocusPane == FocusPane::TopNavBar) ? topNavFocusedIndex : -1;
    vm.navBar.mediaHoverMix = navMediaTabHover.current;
    vm.navBar.playlistHoverMix = navPlaylistTabHover.current;
    vm.navBar.historyHoverMix = navHistoryTabHover.current;
    vm.navBar.settingsHoverMix = navSettingsTabHover.current;
    vm.navBar.openHoverMix = navOpenFileLift.current;
    vm.navBar.pressedNavIndex = pressedNavIndex;
    vm.navBar.indicatorPos = navIndicator.current;
    // 2. Main Content View Area
    vm.viewBounds = Presentation::HubView(winW, winH, dp);
    switch (currentTab) {
        case TabView::Media: {
            float tbH = 32.0f * dp, listTop = vm.viewBounds.top + tbH + 12.0f * dp;
            vm.mediaTab.clipBounds = D2D1::RectF(vm.viewBounds.left - 10.0f * dp, listTop, vm.viewBounds.right + 10.0f * dp, vm.winBounds.bottom);
            float cardLeft = vm.viewBounds.left + 1.0f * dp, cardRight = vm.viewBounds.right - 2.0f * dp;
            // Toolbar layout
            vm.mediaTab.toolbarBounds = D2D1::RectF(vm.viewBounds.left, vm.viewBounds.top, vm.viewBounds.right, vm.viewBounds.top + tbH);
            const auto toolbarGeometry = ComputeMediaToolbarGeometry(vm.mediaTab.toolbarBounds, dp);
            vm.mediaTab.searchBtnBounds = toolbarGeometry.search;
            mediaSearchBtnBounds = vm.mediaTab.searchBtnBounds;
            vm.mediaTab.searchClearBounds = toolbarGeometry.searchClear;
            mediaSearchClearBtnBounds = vm.mediaTab.searchClearBounds;
            vm.mediaTab.sortBtnBounds = toolbarGeometry.sort;
            mediaSortBtnBounds = vm.mediaTab.sortBtnBounds;
            vm.mediaTab.groupControlBounds = toolbarGeometry.group;
            vm.mediaTab.flatBtnBounds = toolbarGeometry.flat;
            mediaFlatBtnBounds = vm.mediaTab.flatBtnBounds;
            vm.mediaTab.seriesBtnBounds = toolbarGeometry.series;
            mediaSeriesBtnBounds = vm.mediaTab.seriesBtnBounds;
            vm.mediaTab.listenBtnBounds = toolbarGeometry.listen;
            mediaListenBtnBounds = vm.mediaTab.listenBtnBounds;
            vm.mediaTab.activeGroupMode = static_cast<int>(libraryGroupMode);
            vm.mediaTab.groupModeIndicatorPos = groupModeIndicator.current;
            vm.mediaTab.hoveredGroupBtn = hoveredGroupBtn;
            vm.mediaTab.searchHovered = hoveredMediaToolbarAction == 0;
            vm.mediaTab.sortHovered = hoveredMediaToolbarAction == 1;
            vm.mediaTab.searchFocused = isMediaSearchFocused && (activeTextInputField == TextInputField::MediaSearch);
            vm.mediaTab.searchClearHovered = isMediaSearchClearHovered;
            vm.mediaTab.hasSearchQuery = !mediaSearchInput.GetText().empty();
            vm.mediaTab.filterQuery = Text16ToWideString(mediaSearchInput.GetText());
            vm.mediaTab.searchVisual.displayText = Text16ToWideString(mediaSearchInput.GetDisplayText());
            vm.mediaTab.searchVisual.selectionStart = mediaSearchInput.GetSelection().start;
            vm.mediaTab.searchVisual.selectionEnd = mediaSearchInput.GetSelection().end;
            vm.mediaTab.searchVisual.caretIndex = mediaSearchInput.GetCaret();
            vm.mediaTab.searchVisual.focused = vm.mediaTab.searchFocused;
            vm.mediaTab.searchVisual.caretVisible = vm.mediaTab.searchFocused && textInputCaretVisible;
            if (mediaSearchInput.IsComposing()) {
                const auto compositionRange = mediaSearchInput.GetCompositionRange();
                vm.mediaTab.searchVisual.hasCompositionRange = true;
                vm.mediaTab.searchVisual.compositionStart = compositionRange.start;
                vm.mediaTab.searchVisual.compositionEnd = compositionRange.end;
                vm.mediaTab.searchVisual.compositionCaretIndex = mediaSearchInput.GetDisplayCaret();
            }
            vm.mediaTab.showSummary = toolbarGeometry.showSummary;
            vm.mediaTab.compactToolbar = toolbarGeometry.compact;
            vm.mediaTab.sortLabel = GetLibrarySortLabel(toolbarGeometry.compact);
            // ---- Listen panel (podcasts + radio), drawn instead of media cards ----
            if (libraryGroupMode == LibraryGroupMode::Listen) {
                vm.listenPanel.isActive = true;
                // Kick the directory/feed jobs once per entry (both are
                // idempotent; private mode skips podcast refresh).
                if (!listenJobsKicked) {
                    listenJobsKicked = true;
                    std::vector<std::wstring> feedUrls;
                    for (const auto& f : podcastSubscriptions.GetFeeds()) feedUrls.push_back(f.url);
                    if (!feedUrls.empty() && !playbackSession.IsPrivateMode()) podcastJobs.EnsureRefresh(feedUrls);
                }
                if (!radioFavoritesLoaded) {
                    radioFavoritesLoaded = true;
                    radioFavorites.Load(GetDataDir());
                }
                const D2D1_RECT_F view = Presentation::HubView(winW, winH, dp);
                auto& panel = vm.listenPanel;
                panel.bounds = D2D1::RectF(view.left, view.top + 44.0f * dp, view.right, view.bottom);
                panel.scrollY = listenScrollY;
                panel.viewportHeight = RectHeight(panel.bounds);
                panel.subTab = (listenSubTab == 1) ? ListenSubTab::Radio : ListenSubTab::Podcasts;
                panel.spinnerAngle = (float)listenSpinAngle;
                if (!listenSubTabIndicatorReady) {
                    listenSubTabIndicatorReady = true;
                    listenSubTabIndicator.SnapTo((float)listenSubTab);
                }
                panel.subTabIndicatorPos = listenSubTabIndicator.current;
                const float pad = 14.0f * dp;
                // Fixed header: segmented control + per-tab actions; only the
                // row area below scrolls.
                const float bandH = 30.0f * dp, segW = 118.0f * dp;
                panel.subTabBounds = D2D1::RectF(panel.bounds.left + pad, panel.bounds.top + 8.0f * dp,
                                                 panel.bounds.left + pad + segW * 2.0f, panel.bounds.top + 8.0f * dp + bandH);
                panel.subTabPodcastsRect = D2D1::RectF(panel.subTabBounds.left + 2.0f * dp, panel.subTabBounds.top + 2.0f * dp,
                                                       panel.subTabBounds.left + segW, panel.subTabBounds.bottom - 2.0f * dp);
                panel.subTabRadioRect = D2D1::RectF(panel.subTabBounds.left + segW, panel.subTabPodcastsRect.top,
                                                    panel.subTabBounds.right - 2.0f * dp, panel.subTabPodcastsRect.bottom);
                const float rowH = 46.0f * dp, epH = 38.0f * dp, stH = 44.0f * dp, gap = 4.0f * dp;
                const float contentTop = panel.subTabBounds.bottom + 8.0f * dp;
                float y = contentTop - listenScrollY;
                // Stations render in row order (podcasts tab leaves it empty);
                // clicks resolve through the mirrored listenVisibleStations list.
                std::vector<RadioStation> visibleStations;
                const auto resolveFreshFeed = [&](size_t subIdx) -> const PodcastFeed* {
                    const auto& subs = podcastSubscriptions.GetFeeds();
                    if (subIdx >= subs.size()) return nullptr;
                    const std::wstring normSubUrl = NormalizePodcastFeedUrl(subs[subIdx].url);
                    for (const auto& f : listenFeeds) {
                        if (f.url == subs[subIdx].url || (!normSubUrl.empty() && NormalizePodcastFeedUrl(f.url) == normSubUrl)) {
                            return &f;
                        }
                    }
                    if (listenFeeds.size() == subs.size()) return &listenFeeds[subIdx];
                    return nullptr;
                };
                if (panel.subTab == ListenSubTab::Podcasts) {
                    const float addW = 112.0f * dp, addH = 26.0f * dp;
                    panel.addPodcastRect = D2D1::RectF(panel.bounds.right - pad - addW,
                        panel.subTabBounds.top + (bandH - addH) * 0.5f, panel.bounds.right - pad,
                        panel.subTabBounds.top + (bandH - addH) * 0.5f + addH);
                    const bool refreshFailed = podcastJobs.GetFetchState() == PodcastJobs::FetchState::Ready
                        && !podcastJobs.GetResult().success && !podcastSubscriptions.GetFeeds().empty();
                    if (refreshFailed) {
                        panel.podcastRefreshFailed = true;
                        const auto& failures = podcastJobs.GetResult().failures;
                        panel.podcastErrorText = (!failures.empty() && !failures.front().reason.empty())
                            ? (L"Couldn't refresh podcasts \u2014 " + failures.front().reason)
                            : std::wstring(L"Couldn't refresh podcasts \u2014 check your connection");
                        const float bannerH = 34.0f * dp;
                        panel.podcastBannerRect = D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + bannerH);
                        const float retryW = 78.0f * dp, retryH = 22.0f * dp;
                        panel.podcastBannerRetryRect = D2D1::RectF(panel.podcastBannerRect.right - 8.0f * dp - retryW,
                            panel.podcastBannerRect.top + (bannerH - retryH) * 0.5f,
                            panel.podcastBannerRect.right - 8.0f * dp,
                            panel.podcastBannerRect.top + (bannerH - retryH) * 0.5f + retryH);
                        y += bannerH + 10.0f * dp;
                    }
                    const auto& subs = podcastSubscriptions.GetFeeds();
                    const auto& fresh = listenFeeds;
                    for (size_t i = 0; i < subs.size(); ++i) {
                        ListenFeedViewModel feedVm;
                        feedVm.title = subs[i].title.empty() ? subs[i].url : subs[i].title;
                        const PodcastFeed* freshFeed = resolveFreshFeed(i);
                        if (freshFeed && !freshFeed->title.empty() && (subs[i].title.empty() || subs[i].title == subs[i].url)) {
                            feedVm.title = freshFeed->title;
                        }
                        feedVm.metaText = freshFeed && !freshFeed->episodes.empty()
                            ? (std::to_wstring(freshFeed->episodes.size()) + L" episodes")
                            : (podcastJobs.IsRunning() ? std::wstring(L"Loading episodes\u2026") : std::wstring(L"No episodes found"));
                        if (freshFeed && !freshFeed->episodes.empty() && freshFeed->episodes.front().publishedSec > 0) {
                            std::time_t tt = (std::time_t)freshFeed->episodes.front().publishedSec;
                            std::tm tmLocal{};
                            if (localtime_s(&tmLocal, &tt) == 0) {
                                wchar_t buf[40]{};
                                swprintf_s(buf, L" \x00B7 latest %d/%d/%d", tmLocal.tm_mon + 1, tmLocal.tm_mday, tmLocal.tm_year + 1900);
                                feedVm.metaText += buf;
                            }
                        }
                        feedVm.expanded = ((int)i == listenExpandedFeed);
                        if (feedVm.expanded && freshFeed) {
                            for (const auto& ep : freshFeed->episodes) {
                                ListenEpisodeViewModel epVm;
                                epVm.title = ep.title.empty() ? L"Untitled episode" : ep.title;
                                epVm.url = ep.url;
                                if (ep.publishedSec > 0) {
                                    std::time_t tt = (std::time_t)ep.publishedSec;
                                    std::tm tmLocal{};
                                    if (localtime_s(&tmLocal, &tt) == 0) {
                                        wchar_t buf[32]{};
                                        swprintf_s(buf, L"%d/%d/%d", tmLocal.tm_mon + 1, tmLocal.tm_mday, tmLocal.tm_year + 1900);
                                        epVm.dateText = buf;
                                    }
                                }
                                if (ep.durationSec > 0.0) epVm.durationText = FormatDuration(ep.durationSec);
                                feedVm.episodes.push_back(std::move(epVm));
                            }
                        }
                        feedVm.episodes.resize(std::min<size_t>(feedVm.episodes.size(), 60));
                        panel.feeds.push_back(std::move(feedVm));
                        panel.feedBounds.push_back(D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + rowH));
                        // Hover actions at the row's right edge: [trash][play latest][chevron].
                        const float chevSz = 16.0f * dp, playSz = 22.0f * dp, trashBox = 20.0f * dp;
                        const float chevL = panel.bounds.right - pad - 12.0f * dp - chevSz;
                        const float playR = chevL - 10.0f * dp, playL = playR - playSz;
                        const float trashR = playL - 8.0f * dp, trashL = trashR - trashBox;
                        const float midTop = y + (rowH - playSz) * 0.5f, midBottom = midTop + playSz;
                        panel.feedPlayBounds.push_back(D2D1::RectF(playL, midTop, playR, midBottom));
                        panel.feedRemoveBounds.push_back(D2D1::RectF(trashL, y, trashR, y + rowH));
                        y += rowH + gap;
                        if (((int)i == listenExpandedFeed) && !panel.feeds.back().episodes.empty()) {
                            for (size_t e = 0; e < panel.feeds.back().episodes.size(); ++e) {
                                panel.episodeBounds.push_back(D2D1::RectF(panel.bounds.left + pad + 40.0f * dp, y, panel.bounds.right - pad, y + epH));
                                const float epPlaySz = 22.0f * dp;
                                panel.episodePlayBounds.push_back(D2D1::RectF(panel.bounds.right - pad - 12.0f * dp - epPlaySz,
                                    y + (epH - epPlaySz) * 0.5f, panel.bounds.right - pad - 12.0f * dp,
                                    y + (epH - epPlaySz) * 0.5f + epPlaySz));
                                panel.episodeFeedIndex.push_back((int)i);
                                y += epH + gap;
                            }
                        }
                    }
                    if (subs.empty()) {
                        panel.state.visible = true;
                        panel.state.icon = IconType::MicPodcast;
                        panel.state.title = L"No podcasts yet";
                        panel.state.body = L"Follow a show by pasting its RSS feed URL \u2014 new episodes appear here automatically.";
                        panel.state.actionText = L"Add Podcast";
                        panel.state.bounds = D2D1::RectF(panel.bounds.left, contentTop, panel.bounds.right, panel.bounds.bottom);
                        const float btnW = 150.0f * dp, btnH = 36.0f * dp;
                        const float bcx = (panel.state.bounds.left + panel.state.bounds.right) * 0.5f;
                        const float bcy = panel.state.bounds.top + 164.0f * dp + btnH * 0.5f; // below the draw-side title+body stack
                        panel.state.buttonBounds = D2D1::RectF(bcx - btnW * 0.5f, bcy - btnH * 0.5f, bcx + btnW * 0.5f, bcy + btnH * 0.5f);
                    }
                } else {
                    // Radio header actions: search field + add station.
                    const float actH = 26.0f * dp, iconBtnW = 30.0f * dp;
                    const float actTop = panel.subTabBounds.top + (bandH - actH) * 0.5f;
                    const float actBottom = actTop + actH;
                    panel.radioAddStationRect = D2D1::RectF(panel.bounds.right - pad - iconBtnW, actTop, panel.bounds.right - pad, actBottom);
                    const float maxSearchW = panel.radioAddStationRect.left - 10.0f * dp - (panel.subTabBounds.right + 12.0f * dp);
                    const float searchW = std::clamp(240.0f * dp, std::min(120.0f * dp, maxSearchW), maxSearchW);
                    panel.radioSearchRect = D2D1::RectF(panel.radioAddStationRect.left - 10.0f * dp - searchW, actTop,
                                                        panel.radioAddStationRect.left - 10.0f * dp, actBottom);
                    const float clearSz = 18.0f * dp;
                    panel.radioSearchClearRect = D2D1::RectF(panel.radioSearchRect.right - 6.0f * dp - clearSz,
                        panel.radioSearchRect.top + (actH - clearSz) * 0.5f,
                        panel.radioSearchRect.right - 6.0f * dp,
                        panel.radioSearchRect.top + (actH - clearSz) * 0.5f + clearSz);
                    panel.radioSearchFocused = isRadioSearchFocused;
                    panel.radioSearchVisual.displayText = Text16ToWideString(radioSearchInput.GetText());
                    panel.radioSearchVisual.selectionStart = radioSearchInput.GetSelection().start;
                    panel.radioSearchVisual.selectionEnd = radioSearchInput.GetSelection().end;
                    panel.radioSearchVisual.caretIndex = radioSearchInput.GetCaret();
                    panel.radioSearchVisual.focused = panel.radioSearchFocused;
                    panel.radioSearchVisual.caretVisible = panel.radioSearchFocused && textInputCaretVisible;
                    if (radioSearchInput.IsComposing()) {
                        const auto compositionRange = radioSearchInput.GetCompositionRange();
                        panel.radioSearchVisual.hasCompositionRange = true;
                        panel.radioSearchVisual.compositionStart = compositionRange.start;
                        panel.radioSearchVisual.compositionEnd = compositionRange.end;
                        panel.radioSearchVisual.compositionCaretIndex = radioSearchInput.GetDisplayCaret();
                    }
                    const std::wstring query = Text16ToWideString(radioSearchInput.GetText());
                    const auto trim = [](const std::wstring& s) {
                        size_t b = s.find_first_not_of(L" \t");
                        if (b == std::wstring::npos) return std::wstring();
                        size_t e = s.find_last_not_of(L" \t");
                        return s.substr(b, e - b + 1);
                    };
                    const std::wstring trimmed = trim(query);
                    const bool searching = radioJobs.IsSearchRunning();
                    panel.radioSearching = searching;
                    const auto addStationRow = [&](const RadioStation& st) {
                        visibleStations.push_back(st);
                    };
                    if (!trimmed.empty()) {
                        // Search results view (the directory list is replaced).
                        const bool resultsCurrent = (listenSearchDoneQuery == trimmed);
                        if (searching) {
                            panel.sectionCaptions.push_back(L"SEARCHING\u2026");
                            panel.sectionCaptionBounds.push_back(D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + 18.0f * dp));
                            y += 24.0f * dp;
                        }
                        if (!resultsCurrent && !searching) {
                            // The pending debounce will fire; keep the empty frame minimal.
                        } else if (listenSearchFailed && !searching && resultsCurrent) {
                            panel.state.visible = true;
                            panel.state.icon = IconType::Search;
                            panel.state.title = L"Station search unavailable";
                            panel.state.body = listenSearchError.empty()
                                ? std::wstring(L"Check your connection and try again.")
                                : listenSearchError;
                            panel.state.actionText = L"Retry";
                            panel.state.bounds = D2D1::RectF(panel.bounds.left, contentTop, panel.bounds.right, panel.bounds.bottom);
                            const float btnW = 120.0f * dp, btnH = 36.0f * dp;
                            const float bcx = (panel.state.bounds.left + panel.state.bounds.right) * 0.5f;
                            const float bcy = panel.state.bounds.top + 164.0f * dp + btnH * 0.5f; // below the draw-side title+body stack
                            panel.state.buttonBounds = D2D1::RectF(bcx - btnW * 0.5f, bcy - btnH * 0.5f, bcx + btnW * 0.5f, bcy + btnH * 0.5f);
                        } else if (resultsCurrent && listenSearchResults.empty() && !searching) {
                            panel.state.visible = true;
                            panel.state.icon = IconType::Search;
                            panel.state.title = L"No stations found";
                            panel.state.body = L"Nothing matches \u201C" + trimmed + L"\u201D. Try a different station name.";
                            panel.state.bounds = D2D1::RectF(panel.bounds.left, contentTop, panel.bounds.right, panel.bounds.bottom);
                        } else if (resultsCurrent || searching) {
                            if (!searching) {
                                panel.sectionCaptions.push_back(L"RESULTS \x2014 \u201C" + trimmed + L"\u201D");
                                panel.sectionCaptionBounds.push_back(D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + 18.0f * dp));
                                y += 24.0f * dp;
                            }
                            for (const auto& st : listenSearchResults) addStationRow(st);
                        }
                    } else {
                        // No query: saved favorites, nothing else — the
                        // popular-stations directory is gone.
                        if (!radioFavorites.GetStations().empty()) {
                            panel.sectionCaptions.push_back(L"FAVORITES");
                            panel.sectionCaptionBounds.push_back(D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + 18.0f * dp));
                            y += 24.0f * dp;
                            for (const auto& st : radioFavorites.GetStations()) addStationRow(st);
                        } else {
                            panel.state.visible = true;
                            panel.state.icon = IconType::RadioWaves;
                            panel.state.title = L"No favorite stations yet";
                            panel.state.body = L"Search a station by name, or add one by its stream URL. Favorites are saved here.";
                            panel.state.actionText = L"Add Station";
                            panel.state.bounds = D2D1::RectF(panel.bounds.left, contentTop, panel.bounds.right, panel.bounds.bottom);
                            const float btnW = 140.0f * dp, btnH = 36.0f * dp;
                            const float bcx = (panel.state.bounds.left + panel.state.bounds.right) * 0.5f;
                            const float bcy = panel.state.bounds.top + 164.0f * dp + btnH * 0.5f; // below the draw-side title+body stack
                            panel.state.buttonBounds = D2D1::RectF(bcx - btnW * 0.5f, bcy - btnH * 0.5f, bcx + btnW * 0.5f, bcy + btnH * 0.5f);
                        }
                    }
                    for (const auto& st : visibleStations) {
                        ListenStationViewModel stVm;
                        stVm.name = st.name;
                        stVm.url = st.url;
                        stVm.isFavorite = radioFavorites.Contains(st.url);
                        stVm.metaText = st.bitrateKbps > 0 ? (std::to_wstring(st.bitrateKbps) + L" kbps") : L"";
                        if (!st.codec.empty()) { if (!stVm.metaText.empty()) stVm.metaText += L" \x00B7 "; stVm.metaText += st.codec; }
                        if (!st.country.empty()) { if (!stVm.metaText.empty()) stVm.metaText += L" \x00B7 "; stVm.metaText += st.country; }
                        panel.stations.push_back(std::move(stVm));
                        panel.stationBounds.push_back(D2D1::RectF(panel.bounds.left + pad, y, panel.bounds.right - pad, y + stH));
                        const float playSz = 22.0f * dp;
                        panel.stationPlayBounds.push_back(D2D1::RectF(panel.bounds.right - pad - 12.0f * dp - playSz,
                            y + (stH - playSz) * 0.5f, panel.bounds.right - pad - 12.0f * dp,
                            y + (stH - playSz) * 0.5f + playSz));
                        y += stH + gap;
                    }
                }
                panel.contentHeight = (y + listenScrollY) - contentTop + 10.0f * dp;
                listenContentHeight = panel.contentHeight;
                listenViewportHeight = panel.viewportHeight;
                // Mirror hit-test state for the input handlers. Row rects are
                // clipped to the row viewport: a row scrolled beneath the fixed
                // header must not keep a clickable rect inside the header band.
                const D2D1_RECT_F rowHitViewport = D2D1::RectF(panel.bounds.left, panel.subTabBounds.bottom + 4.0f * dp,
                                                               panel.bounds.right, panel.bounds.bottom);
                const auto clipToViewport = [rowHitViewport](const D2D1_RECT_F& r) {
                    return D2D1::RectF(std::max(r.left, rowHitViewport.left), std::max(r.top, rowHitViewport.top),
                                       std::min(r.right, rowHitViewport.right), std::min(r.bottom, rowHitViewport.bottom));
                };
                listenFeedBounds.clear();
                for (const auto& r : panel.feedBounds) listenFeedBounds.push_back(clipToViewport(r));
                listenEpisodeBounds.clear();
                for (const auto& r : panel.episodeBounds) listenEpisodeBounds.push_back(clipToViewport(r));
                listenEpisodeFeedIndex = panel.episodeFeedIndex;
                listenStationBounds.clear();
                for (const auto& r : panel.stationBounds) listenStationBounds.push_back(clipToViewport(r));
                listenVisibleStations = std::move(visibleStations);
                listenFeedPlayBounds.clear();
                for (const auto& r : panel.feedPlayBounds) listenFeedPlayBounds.push_back(clipToViewport(r));
                listenFeedRemoveBounds.clear();
                for (const auto& r : panel.feedRemoveBounds) listenFeedRemoveBounds.push_back(clipToViewport(r));
                listenEpisodePlayBounds.clear();
                for (const auto& r : panel.episodePlayBounds) listenEpisodePlayBounds.push_back(clipToViewport(r));
                listenStationPlayBounds.clear();
                for (const auto& r : panel.stationPlayBounds) listenStationPlayBounds.push_back(clipToViewport(r));
                listenAddPodcastBounds = panel.addPodcastRect;
                listenSubTabBounds = panel.subTabBounds;
                listenSubTabPodcastsBounds = panel.subTabPodcastsRect;
                listenSubTabRadioBounds = panel.subTabRadioRect;
                listenPodcastBannerRetryBounds = panel.podcastBannerRetryRect;
                listenRadioSearchBounds = panel.radioSearchRect;
                listenRadioSearchClearBounds = panel.radioSearchClearRect;
                listenRadioAddStationBounds = panel.radioAddStationRect;
                listenStateButtonBounds = panel.state.buttonBounds;
                panel.hoveredFeed = listenHoveredFeed;
                panel.hoveredEpisode = listenHoveredEpisode;
                panel.hoveredEpisodeFeed = listenHoveredEpisodeFeed;
                panel.hoveredStation = listenHoveredStation;
                panel.hoveredFeedPlay = listenHoveredFeedPlay;
                panel.hoveredFeedRemove = listenHoveredFeedRemove;
                panel.hoveredStationPlay = listenHoveredStationPlay;
                panel.addPodcastHoverProg = listenHeaderHoverAnim[0].current;
                panel.bannerRetryHoverProg = listenHeaderHoverAnim[1].current;
                panel.addStationHoverProg = listenHeaderHoverAnim[2].current;
                panel.stateButtonHoverProg = listenHeaderHoverAnim[3].current;
                panel.radioSearchClearHovered = listenRadioSearchClearHovered;
                panel.activeUrl = listenPlaybackActive ? listenUrl : std::wstring();
                panel.activePaused = listenPlaybackPaused;
            } else {
                listenJobsKicked = false;
            }
            static const LibraryViewProjection kPendingProjection;
            static const std::vector<size_t> kPendingVisibleIndices;
            const auto& proj = libraryReady ? GetCurrentMediaProjection() : kPendingProjection;
            const auto& visibleIndices = libraryReady ? proj.indices : kPendingVisibleIndices;
            size_t totalCount = library.mediaItems.size();
            const bool hasSearch = !mediaSearchInput.GetText().empty();
            if (!libraryReady) {
                vm.mediaTab.summaryText = libraryLoadFailed ? L"Library unavailable" : L"Loading library\x2026";
            } else if (libraryGroupMode == LibraryGroupMode::Listen) {
                vm.mediaTab.summaryText = L"Podcasts & radio";
            } else if (hasSearch) {
                if (libraryGroupMode == LibraryGroupMode::Folder) {
                    vm.mediaTab.summaryText = std::to_wstring(proj.groups.size()) + L" folders \x2022 " + std::to_wstring(proj.filteredCount) + L" of " + std::to_wstring(totalCount) + L" videos";
                } else if (libraryGroupMode == LibraryGroupMode::Series) {
                    size_t actualSeriesCount = 0;
                    for (const auto& g : proj.groups) { if (!g.isSpecial) actualSeriesCount++; }
                    vm.mediaTab.summaryText = std::to_wstring(actualSeriesCount) + L" series \x2022 " + std::to_wstring(proj.filteredCount) + L" of " + std::to_wstring(totalCount) + L" videos";
                } else {
                    vm.mediaTab.summaryText = std::to_wstring(proj.filteredCount) + L" of " + std::to_wstring(totalCount) + L" videos";
                }
            } else if (libraryGroupMode == LibraryGroupMode::Folder) {
                vm.mediaTab.summaryText = std::to_wstring(proj.groups.size()) + L" folders \x2022 " + std::to_wstring(totalCount) + L" videos";
            } else if (libraryGroupMode == LibraryGroupMode::Series) {
                size_t actualSeriesCount = 0;
                for (const auto& g : proj.groups) { if (!g.isSpecial) actualSeriesCount++; }
                vm.mediaTab.summaryText = std::to_wstring(actualSeriesCount) + L" series \x2022 " + std::to_wstring(totalCount) + L" videos";
            } else if (libraryGroupMode == LibraryGroupMode::Duplicates) {
                if (proj.groups.empty()) {
                    vm.mediaTab.summaryText = L"No duplicate media detected \x2022 " + std::to_wstring(totalCount) + L" videos";
                } else {
                    size_t dupEntries = (proj.indices.size() > proj.groups.size()) ? (proj.indices.size() - proj.groups.size()) : 0;
                    vm.mediaTab.summaryText = std::to_wstring(proj.groups.size()) + L" duplicate groups \x2022 " + std::to_wstring(dupEntries) + L" duplicates";
                }
            } else { vm.mediaTab.summaryText = std::to_wstring(totalCount) + L" videos"; }
            vm.mediaTab.summaryTextBounds = toolbarGeometry.summary;
            bool showEmptyState = (libraryGroupMode == LibraryGroupMode::Listen) ? false
                : (libraryGroupMode == LibraryGroupMode::Flat) ? visibleIndices.empty()
                : (proj.groups.empty() && visibleIndices.empty());
            if (showEmptyState) {
                vm.mediaTab.isEmpty = true;
                vm.mediaTab.emptyState.visible = true;
                vm.mediaTab.emptyState.bounds = vm.mediaTab.clipBounds;
                // Integrated empty state: match the content center used by
                // DrawEmptyState (no card; text and actions sit on the canvas).
                float boundsW = RectWidth(vm.mediaTab.clipBounds);
                float boxW = std::min(540.0f * dp, std::max(280.0f * dp, boundsW - 48.0f * dp));
                float boxH = 150.0f * dp;
                float cx = (vm.mediaTab.clipBounds.left + vm.mediaTab.clipBounds.right) * 0.5f;
                float cy = vm.mediaTab.clipBounds.top + (RectHeight(vm.mediaTab.clipBounds) - boxH) * 0.5f + 46.0f * dp;
                vm.mediaTab.emptyState.cardBounds = D2D1::RectF(cx - boxW * 0.5f, cy - 44.0f * dp, cx + boxW * 0.5f, cy + 80.0f * dp);
                mediaEmptyBtnBounds.clear();
                if (!libraryReady) {
                    vm.mediaTab.emptyState.type = EmptyStateType::MediaEmptyLibrary;
                    vm.mediaTab.emptyState.icon = libraryLoadFailed ? IconType::VolumeMuted : IconType::Refresh;
                    vm.mediaTab.emptyState.title = libraryLoadFailed ? L"The media library could not be loaded" : L"Loading your media library";
                    vm.mediaTab.emptyState.body = libraryLoadFailed
                        ? L"The existing files were left unchanged so they can be repaired or recovered safely."
                        : L"Aperture is preparing media, playlists, and history in the background.";
                } else if (totalCount == 0) {
                    vm.mediaTab.emptyState.type = EmptyStateType::MediaEmptyLibrary;
                    vm.mediaTab.emptyState.icon = IconType::MediaFilm;
                    vm.mediaTab.emptyState.title = L"Media library is empty";
                    vm.mediaTab.emptyState.body = L"Open video files or add a folder to watch. You can also drag and drop media files anywhere into the window.";
                    EmptyStateButtonViewModel openFileBtn;
                    openFileBtn.text = L"Open File";
                    openFileBtn.icon = IconType::Plus;
                    openFileBtn.isPrimary = true;
                    openFileBtn.isHovered = (hoveredMediaEmptyBtn == 0);
                    openFileBtn.isPressed = (pressedMediaEmptyBtn == 0);
                    openFileBtn.isFocused = (activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == 0);
                    EmptyStateButtonViewModel addFolderBtn;
                    addFolderBtn.text = L"Add Folder";
                    addFolderBtn.icon = IconType::FolderPlaylist;
                    addFolderBtn.isPrimary = false;
                    addFolderBtn.isHovered = (hoveredMediaEmptyBtn == 1);
                    addFolderBtn.isPressed = (pressedMediaEmptyBtn == 1);
                    addFolderBtn.isFocused = (activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == 1);
                    float btnH = 36.0f * dp, btnY = cy + 44.0f * dp, btnGap = 12.0f * dp, btn1W = 124.0f * dp;
                    float btn2W = 132.0f * dp;
                    if (boxW >= (btn1W + btn2W + btnGap + 32.0f * dp)) {
                        float totalBtnW = btn1W + btn2W + btnGap, startX = cx - totalBtnW * 0.5f;
                        openFileBtn.bounds = D2D1::RectF(startX, btnY, startX + btn1W, btnY + btnH);
                        addFolderBtn.bounds = D2D1::RectF(startX + btn1W + btnGap, btnY, startX + totalBtnW, btnY + btnH);
                    } else {
                        float singleW = std::min(btn1W, (boxW - 40.0f * dp - btnGap) * 0.5f);
                        float startX = cx - (singleW * 2.0f + btnGap) * 0.5f;
                        openFileBtn.bounds = D2D1::RectF(startX, btnY, startX + singleW, btnY + btnH);
                        addFolderBtn.bounds = D2D1::RectF(startX + singleW + btnGap, btnY, startX + singleW * 2.0f + btnGap, btnY + btnH);
                    }
                    mediaEmptyBtnBounds.push_back(openFileBtn.bounds);
                    mediaEmptyBtnBounds.push_back(addFolderBtn.bounds);
                    vm.mediaTab.emptyState.buttons.push_back(std::move(openFileBtn));
                    vm.mediaTab.emptyState.buttons.push_back(std::move(addFolderBtn));
                } else {
                    vm.mediaTab.emptyState.type = EmptyStateType::MediaNoResults;
                    vm.mediaTab.emptyState.icon = IconType::Search;
                    if (!mediaSearchInput.GetText().empty()) {
                        vm.mediaTab.emptyState.title = L"No media matching \"" + Text16ToWideString(mediaSearchInput.GetText()) + L"\"";
                        vm.mediaTab.emptyState.body = L"Try refining your search terms or clear the search to view all videos.";
                    } else if (libraryGroupMode == LibraryGroupMode::Duplicates) {
                        vm.mediaTab.emptyState.title = L"No duplicate media found";
                        vm.mediaTab.emptyState.body = L"No duplicate videos were detected. Switch to Standalone to see every file.";
                    } else {
                        vm.mediaTab.emptyState.title = L"No matching media found";
                        vm.mediaTab.emptyState.body = L"No media matches the current grouping. Switch to Standalone to see every file.";
                    }
                    EmptyStateButtonViewModel resetFilterBtn;
                    resetFilterBtn.text = !mediaSearchInput.GetText().empty() ? L"Clear Search" : L"Clear Filters";
                    resetFilterBtn.icon = IconType::Refresh;
                    resetFilterBtn.isPrimary = true;
                    resetFilterBtn.isHovered = (hoveredMediaEmptyBtn == 0);
                    resetFilterBtn.isPressed = (pressedMediaEmptyBtn == 0);
                    resetFilterBtn.isFocused = (activeFocusPane == FocusPane::MediaList && keyboardFocusedIndex == 0);
                    float btnH = 36.0f * dp, btnY = cy + 44.0f * dp, btnW = 140.0f * dp;
                    resetFilterBtn.bounds = D2D1::RectF(cx - btnW * 0.5f, btnY, cx + btnW * 0.5f, btnY + btnH);
                    mediaEmptyBtnBounds.push_back(resetFilterBtn.bounds);
                    vm.mediaTab.emptyState.buttons.push_back(std::move(resetFilterBtn));
                }
            } else {
                mediaEmptyBtnBounds.clear();
                EnsureMediaLayoutCache();
                bool hasPinned = !isLastWatchedCleared && !visibleIndices.empty() && libraryGroupMode == LibraryGroupMode::Flat &&
                    (visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0);
                bool isMediaDrag = (activeDragType == DragType::MediaList);
                vm.mediaTab.isMediaDrag = isMediaDrag;
                vm.mediaTab.dragFromIndex = dragFromIndex;
                float scrollYDIP = mediaScrollY / dp;
                float viewportHDIP = RectHeight(vm.mediaTab.clipBounds) / dp;
                auto [firstVisible, lastVisible] = GetVisibleMediaRowRange(scrollYDIP, viewportHDIP, visibleIndices.size(), 2);
                const float kGroupHeaderHDIP = 38.0f;
                // Project group headers
                if (libraryGroupMode != LibraryGroupMode::Flat) {
                    const auto& headerOffsets = m_mediaLayoutCache.groupHeaderOffsetsDIP;
                    const size_t groupCount = std::min(proj.groups.size(), headerOffsets.size());
                    // Offsets are sorted even when groups are collapsed. Keep
                    // the pixel-space comparison identical at clip boundaries.
                    const auto firstHeader = std::lower_bound(headerOffsets.begin(), headerOffsets.begin() + groupCount,
                        vm.mediaTab.clipBounds.top, [&](float offset, float clipTop) {
                            return listTop + (offset * dp) - mediaScrollY + (kGroupHeaderHDIP * dp) < clipTop;
                        });
                    for (size_t g = static_cast<size_t>(firstHeader - headerOffsets.begin()); g < groupCount; ++g) {
                        const auto& grp = proj.groups[g];
                        float headerTopDIP = headerOffsets[g];
                        float headerTopPx = listTop + (headerTopDIP * dp) - mediaScrollY;
                        if (headerTopPx > vm.mediaTab.clipBounds.bottom) break;
                        float headerBottomPx = headerTopPx + (kGroupHeaderHDIP * dp);
                        if (headerBottomPx >= vm.mediaTab.clipBounds.top && headerTopPx <= vm.mediaTab.clipBounds.bottom) {
                            VisibleMediaGroupHeaderViewModel hvm;
                            hvm.key = grp.key;
                            hvm.title = grp.title;
                            hvm.subtitle = grp.subtitle;
                            hvm.isSpecial = grp.isSpecial;
                            hvm.isSeries = (libraryGroupMode == LibraryGroupMode::Series);
                            hvm.isCollapsed = grp.isCollapsed;
                            hvm.isHovered = (hoveredMediaGroupHeader == (int)g);
                            hvm.isSelected = (selectedFolderKeys.count(grp.key) > 0);
                            hvm.isDeleteHovered = (hoveredFolderDeleteIndex == (int)g);
                            hvm.headerRect = D2D1::RectF(cardLeft, headerTopPx, cardRight, headerBottomPx - 4.0f * dp);
                            if (libraryGroupMode == LibraryGroupMode::Series && !grp.isSpecial) {
                                hvm.countText = std::to_wstring(grp.count) + L" episodes";
                            } else if (libraryGroupMode == LibraryGroupMode::Duplicates) {
                                hvm.countText = std::to_wstring(grp.count) + L" copies";
                            } else { hvm.countText = std::to_wstring(grp.count) + L" videos"; }
                            vm.mediaTab.visibleGroupHeaders.push_back(std::move(hvm));
                        }
                    }
                }
                vm.mediaTab.visibleCards.reserve(lastVisible > firstVisible ? (lastVisible - firstVisible) : 0);
                for (size_t i = firstVisible; i < lastVisible; ++i) {
                    size_t origIdx = visibleIndices[i];
                    if (origIdx >= library.mediaItems.size()) continue;
                    float rowTopDIP = GetMediaRowTopDIP(i);
                    float curY = listTop + (rowTopDIP * dp) - mediaScrollY;
                    if (isMediaDrag && (int)i == dragFromIndex) {
                        float baseH = (hasPinned && (int)i == 0) ? kMediaPinnedRestHeight : kMediaRestHeight;
                        float itemH = (i < rowAnimStates.size() && rowAnimStates[i].height.current > 10.0f)
                            ? rowAnimStates[i].height.current * dp : baseH * dp;
                        VisibleMediaCardViewModel card;
                        card.originalIndex = origIdx;
                        card.isPlaceholder = true;
                        float slotY = listTop + GetMediaRowTopDIP(static_cast<size_t>(std::max(0, dragToIndex))) * dp - mediaScrollY;
                        card.itemRect = D2D1::RectF(cardLeft, slotY, cardRight, slotY + itemH);
                        vm.mediaTab.visibleCards.push_back(std::move(card));
                        continue;
                    }
                    bool isPinned = (hasPinned && i == 0);
                    float baseH = isPinned ? kMediaPinnedRestHeight : kMediaRestHeight;
                    float itemH = (i < rowAnimStates.size() && rowAnimStates[i].height.current > 10.0f)
                                  ? (rowAnimStates[i].height.current * dp) : (baseH * dp);
                    float lift = (i < rowAnimStates.size() && !isMediaDrag) ? (rowAnimStates[i].liftY.current * dp) : 0.0f;
                    // Explorer-style live gap: while dragging, rows between the origin and the current drop slot slide one row pitch out of
                    // the way so the destination is always visible.
                    if (isMediaDrag && dragFromIndex >= 0 && dragToIndex >= 0 && (int)i != dragFromIndex) {
                        const int slot = Reorder::Slot(static_cast<int>(i), dragFromIndex, dragToIndex);
                        const float desired = (GetMediaRowTopDIP(static_cast<size_t>(slot)) - rowTopDIP) * dp;
                        curY += CardDragOffset(static_cast<int>(i), 0, desired).y;
                    }
                    D2D1_RECT_F itemRect = D2D1::RectF(cardLeft, curY, cardRight, curY + itemH);
                    bool isHover = (hoveredMediaIndex == (int)i && !isMediaDrag);
                    bool isSel = (selectedMediaIndices.count(origIdx) > 0) || (selectedMediaIndex == (int)origIdx);
                    float previewOp = (i < rowAnimStates.size()) ? rowAnimStates[i].previewOpacity.current : 0.0f;
                    ID2D1Bitmap* pLiveBm = (previewPlayer.GetCurrentPreviewPath() == library.mediaItems[origIdx].filePath) ? previewPlayer.GetPreviewBitmap() : nullptr;
                    if (curY + itemH >= vm.mediaTab.clipBounds.top && curY <= vm.mediaTab.clipBounds.bottom) {
                        VisibleMediaCardViewModel card;
                        card.originalIndex = origIdx;
                        card.pItem = &library.mediaItems[origIdx];
                        card.effectiveStatus = IsMediaFileMissing(card.pItem->filePath) ? "missing" : card.pItem->status;
                        card.itemRect = itemRect;
                        card.isHovered = isHover;
                        card.isSelected = isSel;
                        card.isPinned = isPinned;
                        card.previewOpacity = previewOp;
                        card.pLivePreviewBitmap = pLiveBm;
                        card.isCloseHovered = (isPinned && isPinnedCloseHovered);
                        card.liftY = lift;
                        card.isPlaceholder = false;
                        vm.mediaTab.visibleCards.push_back(std::move(card));
                    }
                }
                if (isMediaDrag && dragFromIndex >= 0 && dragFromIndex < (int)visibleIndices.size()) {
                    size_t dragOrigIdx = visibleIndices[dragFromIndex];
                    if (dragOrigIdx < library.mediaItems.size()) {
                        const float offsetY = dragCurrentY - dragStartY;
                        vm.mediaTab.dragCloneRect = D2D1::RectF( dragSourceRect.left, dragSourceRect.top + offsetY,
                            dragSourceRect.right, dragSourceRect.bottom + offsetY);
                        vm.mediaTab.pDragCloneItem = &library.mediaItems[dragOrigIdx];
                        vm.mediaTab.hasDragClone = true;
                    }
                } else if (activeDragType == DragType::MediaFolder && dragFromIndex >= 0 && dragFromIndex < (int)proj.groups.size()) {
                    vm.mediaTab.isFolderDrag = true;
                    vm.mediaTab.dragFromFolderIndex = dragFromIndex;
                    const auto& grp = proj.groups[dragFromIndex];
                    VisibleMediaGroupHeaderViewModel dragHvm;
                    dragHvm.key = grp.key;
                    dragHvm.title = grp.title;
                    dragHvm.subtitle = grp.subtitle;
                    dragHvm.isSpecial = grp.isSpecial;
                    dragHvm.isCollapsed = true;
                    dragHvm.isHovered = true;
                    dragHvm.isSelected = true;
                    dragHvm.countText = std::to_wstring(grp.count) + L" videos";
                    float h = (kGroupHeaderHDIP - 4.0f) * dp;
                    float yCenter = dragCurrentY;
                    dragHvm.headerRect = D2D1::RectF(cardLeft + 12.0f * dp, yCenter - h * 0.5f, cardRight - 12.0f * dp, yCenter + h * 0.5f);
                    vm.mediaTab.dragCloneFolder = std::move(dragHvm);
                }
            }
            break;
        }
        case TabView::Playlists: {
            float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(vm.viewBounds) * 0.42f));
            vm.playlistsTab.rail.bounds = D2D1::RectF(vm.viewBounds.left, vm.viewBounds.top, vm.viewBounds.left + railW, vm.viewBounds.bottom);
            vm.playlistsTab.detail.listClipBounds = D2D1::RectF(vm.viewBounds.left + railW + 16.0f * dp, vm.viewBounds.top, vm.viewBounds.right, vm.viewBounds.bottom);
            // Rail view model
            vm.playlistsTab.rail.activePlaylistId = activePlaylistId;
            vm.playlistsTab.rail.railIndicatorPos = playlistRailIndicator.current;
            vm.playlistsTab.rail.hoveredIndex = hoveredPlaylistRailIndex;
            vm.playlistsTab.rail.isFolderDropActive = isRailFolderDropActive;
            vm.playlistsTab.rail.isAddHovered = isPlaylistAddHovered;
            vm.playlistsTab.rail.scrollY = playlistRailScrollY;
            vm.playlistsTab.rail.dragFromIndex = (activeDragType == DragType::PlaylistRail) ? dragFromIndex : -1;
            vm.playlistsTab.rail.dragToIndex = (activeDragType == DragType::PlaylistRail) ? dragToIndex : -1;
            vm.playlistsTab.rail.dragOffsetY = (activeDragType == DragType::PlaylistRail) ? (dragCurrentY - dragStartY) : 0.0f;
            vm.playlistsTab.rail.isRailFocused = isKeyboardNavActive && (activeFocusPane == FocusPane::PlaylistRail);
            vm.playlistsTab.rail.isInlineRenaming = isInlineRenaming;
            vm.playlistsTab.rail.inlineRenamePlaylistId = inlineRenamePlaylistId;
            if (isInlineRenaming) {
                vm.playlistsTab.rail.inlineRenameVisual.displayText = Text16ToWideString(inlineRenameInput.GetDisplayText());
                vm.playlistsTab.rail.inlineRenameVisual.selectionStart = inlineRenameInput.GetSelection().start;
                vm.playlistsTab.rail.inlineRenameVisual.selectionEnd = inlineRenameInput.GetSelection().end;
                vm.playlistsTab.rail.inlineRenameVisual.caretIndex = inlineRenameInput.GetCaret();
                vm.playlistsTab.rail.inlineRenameVisual.focused = (activeTextInputField == TextInputField::InlineRename);
                vm.playlistsTab.rail.inlineRenameVisual.caretVisible = vm.playlistsTab.rail.inlineRenameVisual.focused && textInputCaretVisible;
                if (inlineRenameInput.IsComposing()) {
                    const auto compositionRange = inlineRenameInput.GetCompositionRange();
                    vm.playlistsTab.rail.inlineRenameVisual.hasCompositionRange = true;
                    vm.playlistsTab.rail.inlineRenameVisual.compositionStart = compositionRange.start;
                    vm.playlistsTab.rail.inlineRenameVisual.compositionEnd = compositionRange.end;
                    vm.playlistsTab.rail.inlineRenameVisual.compositionCaretIndex = inlineRenameInput.GetDisplayCaret();
                }
            }
            float headerH = 48.0f * dp, bottomBtnH = 34.0f * dp, itemH = 52.0f * dp;
            float curRailY = vm.playlistsTab.rail.bounds.top + headerH - 2.0f * dp - playlistRailScrollY;
            int railDragFrom = vm.playlistsTab.rail.dragFromIndex, railDragTo = vm.playlistsTab.rail.dragToIndex;
            const float railClipTop = vm.playlistsTab.rail.bounds.top + headerH - 4.0f * dp;
            const float railClipBottom = vm.playlistsTab.rail.bounds.bottom - bottomBtnH - 18.0f * dp;
            const size_t visibleRailCapacity = static_cast<size_t>(std::ceil(
                std::max(0.0f, railClipBottom - railClipTop) / (itemH + 6.0f * dp))) + 2;
            vm.playlistsTab.rail.items.reserve(std::min(library.playlists.size(), visibleRailCapacity));
            for (size_t i = 0; i < library.playlists.size(); ++i) {
                if (railDragFrom >= 0 && (int)i == railDragFrom) { curRailY += itemH + 6.0f * dp; continue; }
                float renderY = curRailY;
                if (railDragFrom >= 0 && railDragTo >= 0) {
                    const int slot = Reorder::Slot(static_cast<int>(i), railDragFrom, railDragTo);
                    renderY += CardDragOffset(static_cast<int>(i), 0, (slot - static_cast<int>(i)) * (itemH + 6.0f * dp)).y;
                }
                curRailY += itemH + 6.0f * dp;
                // Preserve drag animation targets for every row, but resolve
                // statistics and copy labels only after the visibility check.
                if (renderY > railClipBottom && railDragFrom < 0) break;
                if (renderY + itemH < railClipTop || renderY > railClipBottom) continue;
                const auto& pl = library.playlists[i];
                // Reuse cached playlist statistics for the visible rail items.
                PlaylistStats plStats = library.GetPlaylistStats(pl.id);
                PlaylistRailItemViewModel rItem;
                rItem.id = pl.id;
                rItem.name = pl.name;
                rItem.isSmart = pl.IsSmart();
                rItem.isActive = (pl.id == activePlaylistId);
                rItem.isFocused = vm.playlistsTab.rail.isRailFocused && (keyboardFocusedIndex == (int)i);
                rItem.isHovered = (hoveredPlaylistRailIndex == (int)i);
                rItem.totalVids = plStats.totalCount;
                rItem.watchedCount = plStats.watchedCount;
                rItem.progressRatio = (rItem.totalVids > 0) ? ((float)rItem.watchedCount / (float)rItem.totalVids) : 0.0f;
                rItem.itemRect = D2D1::RectF(vm.playlistsTab.rail.bounds.left + 8.0f * dp, renderY, vm.playlistsTab.rail.bounds.right - 8.0f * dp, renderY + itemH);
                float ringRadius = 8.0f * dp;
                rItem.ringCX = rItem.itemRect.left + 30.0f * dp;
                rItem.ringCY = rItem.itemRect.top + itemH * 0.5f;
                rItem.ringRadius = ringRadius;
                rItem.titleRect = D2D1::RectF(rItem.ringCX + ringRadius + 10.0f * dp, rItem.itemRect.top + 7.0f * dp, rItem.itemRect.right - 10.0f * dp, rItem.itemRect.top + 27.0f * dp);
                rItem.subRect = D2D1::RectF(rItem.ringCX + ringRadius + 10.0f * dp, rItem.itemRect.top + 27.0f * dp, rItem.itemRect.right - 10.0f * dp, rItem.itemRect.top + 45.0f * dp);
                rItem.isInlineRenaming = (isInlineRenaming && inlineRenamePlaylistId == pl.id);
                vm.playlistsTab.rail.items.push_back(std::move(rItem));
            }
            if (railDragFrom >= 0 && railDragFrom < (int)library.playlists.size()) {
                const auto& pl = library.playlists[railDragFrom];
                vm.playlistsTab.rail.hasDragClone = true;
                float cloneY = dragSourceRect.top + vm.playlistsTab.rail.dragOffsetY;
                vm.playlistsTab.rail.dragCloneRect = D2D1::RectF(vm.playlistsTab.rail.bounds.left + 8.0f * dp, cloneY, vm.playlistsTab.rail.bounds.right - 8.0f * dp, cloneY + itemH);
                vm.playlistsTab.rail.dragCloneName = pl.name;
                vm.playlistsTab.rail.dragCloneItemCount = library.GetPlaylistStats(pl.id).totalCount;
            }
            // Detail pane view model
            Playlist* currentPl = library.FindPlaylist(activePlaylistId);
            if (currentPl) {
                vm.playlistsTab.detail.hasPlaylist = true;
                vm.playlistsTab.detail.playlistId = currentPl->id;
                vm.playlistsTab.detail.playlistName = currentPl->name;
                vm.playlistsTab.detail.isSmart = currentPl->IsSmart();
                const std::vector<std::wstring>& plItemPaths = library.GetPlaylistPathsRef(*currentPl);
                PlaylistStats plStats = library.GetPlaylistStats(currentPl->id);
                std::wstring sizeStr = FormatFileSizeStr(plStats.totalSizeBytes);
                std::wstring durStr = FormatDurationHumanStr(plStats.totalDurationSec);
                vm.playlistsTab.detail.metaStr = std::to_wstring(plItemPaths.size()) + L" videos \x00B7 " + sizeStr + L" \x00B7 " + durStr + L" total";
                if (plStats.watchedCount > 0)
                    vm.playlistsTab.detail.metaStr += L" \x00B7 " + std::to_wstring(plStats.watchedCount) + L" watched";
                if (currentPl->IsSmart()) { vm.playlistsTab.detail.metaStr += L" \x00B7 Smart"; }
                vm.playlistsTab.detail.isAddFilesHovered = isPlaylistAddFilesHovered;
                vm.playlistsTab.detail.isPlayAllHovered = isPlaylistPlayAllHovered;
                vm.playlistsTab.detail.isDeletePlHovered = isPlaylistDeleteHovered;
                vm.playlistsTab.detail.isResumeHovered = isPlaylistResumeHovered;
                vm.playlistsTab.detail.isDropTargetActive = isPlaylistDropTargetActive;
                vm.playlistsTab.detail.isFolderDrop = isPlaylistDropTargetActive && oleDragPayloadHasFolder;
                vm.playlistsTab.detail.isEmpty = plItemPaths.empty();
                if (!plItemPaths.empty()) {
                    size_t resumeIndex = plStats.resumeIndex;
                    const std::wstring* pResumePath = (resumeIndex < plItemPaths.size()) ? &plItemPaths[resumeIndex] : &plItemPaths[0];
                    MediaItem* pResumeMedia = pResumePath ? library.FindByPath(*pResumePath) : nullptr;
                    float detailHeadH = 50.0f * dp, resumeH = 112.0f * dp;
                    float curDetY = vm.playlistsTab.detail.listClipBounds.top + 14.0f * dp + detailHeadH + 16.0f * dp;
                    float paneRight = std::max(vm.playlistsTab.detail.listClipBounds.left + 120.0f * dp, vm.playlistsTab.detail.listClipBounds.right - 26.0f * dp);
                    vm.playlistsTab.detail.resumeCard.hasResumeCard = true;
                    vm.playlistsTab.detail.resumeCard.cardRect = D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, curDetY, paneRight, curDetY + resumeH);
                    vm.playlistsTab.detail.resumeCard.isResumeFocused = isKeyboardNavActive && (activeFocusPane == FocusPane::PlaylistDetail && keyboardFocusedIndex == -1);
                    vm.playlistsTab.detail.resumeCard.isResumeHovered = isPlaylistResumeHovered;
                    vm.playlistsTab.detail.resumeCard.filePath = pResumePath ? *pResumePath : L"";
                    if (previewPlayer.IsActive() && previewPlayer.GetCurrentPreviewPath() == vm.playlistsTab.detail.resumeCard.filePath) {
                        vm.playlistsTab.detail.resumeCard.pLivePreviewBitmap = previewPlayer.GetPreviewBitmap();
                        vm.playlistsTab.detail.resumeCard.previewOpacity = resumePreviewAnim.current;
                    }
                    float thumbW = std::min(220.0f * dp, std::max(80.0f * dp, RectWidth(vm.playlistsTab.detail.resumeCard.cardRect) * 0.30f));
                    float thumbH = std::min(84.0f * dp, std::max(45.0f * dp, resumeH - 28.0f * dp));
                    float thumbX = vm.playlistsTab.detail.resumeCard.cardRect.left + 16.0f * dp;
                    float thumbY = vm.playlistsTab.detail.resumeCard.cardRect.top + (resumeH - thumbH) * 0.5f;
                    vm.playlistsTab.detail.resumeCard.thumbRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
                    vm.playlistsTab.detail.resumeCard.progressRatio = pResumeMedia ? pResumeMedia->GetProgressRatio() : 0.0;
                    // Cache for the Update()-side hover-preview driver.
                    resumePreviewPath = vm.playlistsTab.detail.resumeCard.filePath;
                    resumeResumePos = (pResumeMedia && pResumeMedia->lastPosSec > 5.0) ? pResumeMedia->lastPosSec : 0.0;
                    vm.playlistsTab.detail.resumeCard.resumePosSec = resumeResumePos;
                    vm.playlistsTab.detail.resumeCard.tagText = (pResumeMedia && pResumeMedia->lastPosSec > 0.0) ? L"CONTINUE WATCHING" : L"START WATCHING";
                    vm.playlistsTab.detail.resumeCard.tagRect = D2D1::RectF(thumbX + thumbW + 18.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 18.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.right - 20.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 34.0f * dp);
                    vm.playlistsTab.detail.resumeCard.title = pResumeMedia ? pResumeMedia->title : (pResumePath ? *pResumePath : L"");
                    vm.playlistsTab.detail.resumeCard.titleRect = D2D1::RectF(thumbX + thumbW + 18.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 38.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.right - 20.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 68.0f * dp);
                    double curSec = pResumeMedia ? pResumeMedia->lastPosSec : 0.0, durSec = pResumeMedia ? pResumeMedia->durationSec : 0.0;
                    vm.playlistsTab.detail.resumeCard.timeText = FormatDuration(curSec) + L" / " + FormatDuration(durSec);
                    vm.playlistsTab.detail.resumeCard.timeRect = D2D1::RectF(thumbX + thumbW + 18.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 72.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.right - 20.0f * dp, vm.playlistsTab.detail.resumeCard.cardRect.top + 94.0f * dp);
                    // Search Visual
                    vm.playlistsTab.detail.filterQuery = Text16ToWideString(playlistSearchInput.GetText());
                    vm.playlistsTab.detail.gridView = playlistGridView;
                    vm.playlistsTab.detail.viewModeIndicatorPos = playlistViewModeIndicator.current;
                    vm.playlistsTab.detail.searchFocused = isPlaylistSearchFocused;
                    vm.playlistsTab.detail.searchVisual.displayText = Text16ToWideString(playlistSearchInput.GetDisplayText());
                    vm.playlistsTab.detail.searchVisual.selectionStart = playlistSearchInput.GetSelection().start;
                    vm.playlistsTab.detail.searchVisual.selectionEnd = playlistSearchInput.GetSelection().end;
                    vm.playlistsTab.detail.searchVisual.caretIndex = playlistSearchInput.GetCaret();
                    vm.playlistsTab.detail.searchVisual.focused = isPlaylistSearchFocused && (activeTextInputField == TextInputField::PlaylistSearch);
                    vm.playlistsTab.detail.searchVisual.caretVisible = vm.playlistsTab.detail.searchVisual.focused && textInputCaretVisible;
                    if (playlistSearchInput.IsComposing()) {
                        const auto compositionRange = playlistSearchInput.GetCompositionRange();
                        vm.playlistsTab.detail.searchVisual.hasCompositionRange = true;
                        vm.playlistsTab.detail.searchVisual.compositionStart = compositionRange.start;
                        vm.playlistsTab.detail.searchVisual.compositionEnd = compositionRange.end;
                        vm.playlistsTab.detail.searchVisual.compositionCaretIndex = playlistSearchInput.GetDisplayCaret();
                    }
                    const auto& visibleIndices = GetVisiblePlaylistIndices();
                    vm.playlistsTab.detail.visibleCount = visibleIndices.size();
                    float listContentTop = curDetY + resumeH + 16.0f * dp + 32.0f * dp + 12.0f * dp, epItemH = 62.0f * dp;
                    float viewportH = std::max(1.0f * dp, vm.playlistsTab.detail.listClipBounds.bottom - listContentTop);
                    const size_t gridColumns = playlistGridView ? (size_t)std::max(1.0f, std::floor((RectWidth(D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, listContentTop, paneRight, vm.playlistsTab.detail.listClipBounds.bottom)) + 10.0f * dp) / (190.0f * dp))) : 1;
                    const float gridGap = 10.0f * dp;
                    const float gridW = playlistGridView ? std::max(120.0f * dp, (RectWidth(D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, listContentTop, paneRight, vm.playlistsTab.detail.listClipBounds.bottom)) - (gridColumns - 1) * gridGap) / (float)gridColumns) : RectWidth(D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, listContentTop, paneRight, vm.playlistsTab.detail.listClipBounds.bottom));
                    const float gridH = 166.0f * dp, gridRowPitch = gridH + gridGap, listRowPitch = epItemH + 6.0f * dp;
                    size_t firstDisplayIndex = 0;
                    size_t lastDisplayIndex = visibleIndices.size();
                    if (playlistGridView) {
                        int totalRows = static_cast<int>((visibleIndices.size() + gridColumns - 1) / gridColumns);
                        int firstRow = std::max<int>(0, static_cast<int>(std::floor((playlistDetailScrollY - gridH) / gridRowPitch)) - 1);
                        int lastRow = std::min<int>(totalRows - 1, static_cast<int>(std::ceil((playlistDetailScrollY + viewportH) / gridRowPitch)) + 1);
                        firstDisplayIndex = static_cast<size_t>(firstRow * (int)gridColumns);
                        lastDisplayIndex = std::min<size_t>(visibleIndices.size(), static_cast<size_t>((lastRow + 1) * (int)gridColumns));
                    } else {
                        int firstRow = std::max<int>(0, static_cast<int>(std::floor((playlistDetailScrollY - epItemH) / listRowPitch)) - 2);
                        int lastRow = std::min<int>(static_cast<int>(visibleIndices.size()), static_cast<int>(std::ceil((playlistDetailScrollY + viewportH) / listRowPitch)) + 3);
                        firstDisplayIndex = static_cast<size_t>(firstRow);
                        lastDisplayIndex = std::min<size_t>(visibleIndices.size(), static_cast<size_t>(lastRow));
                    }
                    int itemDragFrom = (activeDragType == DragType::PlaylistItem) ? dragFromIndex : -1;
                    int itemDragTo = (activeDragType == DragType::PlaylistItem) ? dragToIndex : -1;
                    float itemDragOffset = (activeDragType == DragType::PlaylistItem) ? (dragCurrentY - dragStartY) : 0.0f;
                    vm.playlistsTab.detail.visibleEpisodes.reserve(lastDisplayIndex > firstDisplayIndex ? (lastDisplayIndex - firstDisplayIndex) : 0);
                    for (size_t displayIndex = firstDisplayIndex; displayIndex < lastDisplayIndex; ++displayIndex) {
                        size_t i = visibleIndices[displayIndex];
                        if (itemDragFrom >= 0 && (int)i == itemDragFrom) continue;
                        const int destination = itemDragFrom >= 0 && itemDragTo >= 0
                            ? Reorder::Slot(static_cast<int>(displayIndex), itemDragFrom, itemDragTo) : static_cast<int>(displayIndex);
                        const size_t gridColumn = playlistGridView ? displayIndex % gridColumns : 0;
                        const size_t gridRow = playlistGridView ? displayIndex / gridColumns : 0;
                        float renderY = listContentTop - playlistDetailScrollY + (playlistGridView ? gridRow * gridRowPitch : displayIndex * listRowPitch);
                        D2D1_POINT_2F offset{};
                        if (itemDragFrom >= 0) {
                            const float dx = playlistGridView ? (static_cast<int>(destination % gridColumns) - static_cast<int>(gridColumn)) * (gridW + gridGap) : 0;
                            const float dy = playlistGridView ? (static_cast<int>(destination / gridColumns) - static_cast<int>(gridRow)) * gridRowPitch
                                : (destination - static_cast<int>(displayIndex)) * listRowPitch;
                            offset = CardDragOffset(static_cast<int>(i), dx, dy);
                            renderY += offset.y;
                        }
                        const auto& path = plItemPaths[i];
                        MediaItem* pItem = library.FindByPath(path);
                        VisiblePlaylistEpisodeViewModel ep;
                        ep.originalIndex = i;
                        ep.filePath = path;
                        ep.title = pItem ? pItem->title : GetFileNameFromPath(path);
                        ep.durationSec = pItem ? pItem->durationSec : 0.0;
                        ep.lastPosSec = pItem ? pItem->lastPosSec : 0.0;
                        ep.progressRatio = pItem ? pItem->GetProgressRatio() : 0.0;
                        ep.isHovered = itemDragFrom < 0 && (hoveredPlaylistDetailItemIndex == (int)i);
                        ep.isDeleteHovered = (!currentPl->IsSmart() && hoveredPlaylistDetailDeleteIndex == (int)i);
                        ep.isSelected = (selectedEpisodeIndices.count(i) > 0);
                        if (playlistGridView) {
                            float cellLeft = vm.playlistsTab.detail.listClipBounds.left + gridColumn * (gridW + gridGap) + offset.x;
                            ep.itemRect = D2D1::RectF(cellLeft, renderY, cellLeft + gridW, renderY + gridH);
                            auto itExp = playlistCardExpands.find(i);
                            if (itExp != playlistCardExpands.end()) {
                                ep.titleExpandProgress = itExp->second.current;
                            }
                        } else {
                            ep.itemRect = D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, renderY, paneRight, renderY + epItemH);
                        }
                        std::wstring subInfo;
                        if (pItem && pItem->fileSizeBytes > 0) { subInfo += FormatFileSizeStr(pItem->fileSizeBytes); }
                        std::wstring ext = GetFileExtension(path);
                        if (!ext.empty()) {
                            if (ext[0] == L'.') ext = ext.substr(1);
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::towupper);
                            if (!subInfo.empty()) subInfo += L" \x00B7 ";
                            subInfo += ext;
                        }
                        if (pItem && pItem->videoHeight > 0) {
                            if (!subInfo.empty()) subInfo += L" \x00B7 ";
                            subInfo += std::to_wstring(pItem->videoHeight) + L"p";
                        }
                        if (subInfo.empty()) { subInfo = L"Episode " + std::to_wstring(i + 1); }
                        ep.subtitle = subInfo;
                        vm.playlistsTab.detail.visibleEpisodes.push_back(std::move(ep));
                    }
                    if (itemDragFrom >= 0 && itemDragFrom < (int)currentPl->itemPaths.size()) {
                        const auto& path = currentPl->itemPaths[itemDragFrom];
                        MediaItem* pItem = library.FindByPath(path);
                        float cloneY = dragSourceRect.top + itemDragOffset;
                        float cloneX = dragSourceRect.left + (playlistGridView ? dragCurrentX - dragStartX : 0);
                        vm.playlistsTab.detail.hasDragClone = true;
                        vm.playlistsTab.detail.dragCloneRect = D2D1::RectF(cloneX, cloneY, cloneX + RectWidth(dragSourceRect), cloneY + RectHeight(dragSourceRect));
                        const int slot = std::max(0, itemDragTo);
                        float slotX = vm.playlistsTab.detail.listClipBounds.left + (playlistGridView ? (slot % gridColumns) * (gridW + gridGap) : 0);
                        float slotY = listContentTop - playlistDetailScrollY + (playlistGridView ? (slot / gridColumns) * gridRowPitch : slot * listRowPitch);
                        vm.playlistsTab.detail.dropSlotRect = D2D1::RectF(slotX, slotY, slotX + (playlistGridView ? gridW : paneRight - slotX), slotY + (playlistGridView ? gridH : epItemH));
                        vm.playlistsTab.detail.dragCloneEpisode.filePath = path;
                        vm.playlistsTab.detail.dragCloneEpisode.title = pItem ? pItem->title : GetFileNameFromPath(path);
                        std::wstring subInfo;
                        if (pItem && pItem->fileSizeBytes > 0) subInfo += FormatFileSizeStr(pItem->fileSizeBytes);
                        vm.playlistsTab.detail.dragCloneEpisode.subtitle = subInfo;
                    }
                }
            }
            break;
        }
        case TabView::History: {
            vm.historyTab.bounds = vm.viewBounds;
            vm.historyTab.pHistory = &library.history;
            vm.historyTab.hoveredIndex = hoveredHistoryIndex;
            vm.historyTab.hoveredDeleteIndex = hoveredHistoryDeleteIndex;
            vm.historyTab.isClearHovered = isClearHistoryHovered;
            vm.historyTab.isClearPressed = isClearHistoryPressed;
            vm.historyTab.scrollY = historyScrollY;
            vm.historyTab.hasProjectedLayout = true;
            if (library.history.empty()) {
                vm.historyTab.isEmpty = true;
                vm.historyTab.emptyState.visible = true;
                D2D1_RECT_F contentBounds = D2D1::RectF(vm.viewBounds.left + 12.0f * dp, vm.viewBounds.top + 80.0f * dp, vm.viewBounds.right - 12.0f * dp, vm.viewBounds.bottom - 20.0f * dp);
                vm.historyTab.emptyState.bounds = contentBounds;
                float boundsW = RectWidth(contentBounds);
                float boxW = std::min(540.0f * dp, std::max(280.0f * dp, boundsW - 48.0f * dp));
                float boxH = 150.0f * dp;
                float cx = (contentBounds.left + contentBounds.right) * 0.5f;
                float cy = contentBounds.top + (RectHeight(contentBounds) - boxH) * 0.5f + 46.0f * dp;
                vm.historyTab.emptyState.cardBounds = D2D1::RectF(cx - boxW * 0.5f, cy - 44.0f * dp, cx + boxW * 0.5f, cy + 80.0f * dp);
                vm.historyTab.emptyState.type = EmptyStateType::HistoryEmpty;
                vm.historyTab.emptyState.icon = IconType::HistoryClock;
                vm.historyTab.emptyState.title = L"No watch history yet";
                vm.historyTab.emptyState.body = L"Media you play will show up here automatically with your saved playback positions and timestamps.";
                EmptyStateButtonViewModel openFileBtn;
                openFileBtn.text = L"Open File";
                openFileBtn.icon = IconType::Plus;
                openFileBtn.isPrimary = true;
                openFileBtn.isHovered = (hoveredHistoryEmptyBtn == 0);
                openFileBtn.isPressed = (pressedHistoryEmptyBtn == 0);
                openFileBtn.isFocused = (activeFocusPane == FocusPane::HistoryList && keyboardFocusedIndex == 0);
                float btnH = 36.0f * dp, btnY = cy + 44.0f * dp, btnW = 130.0f * dp;
                openFileBtn.bounds = D2D1::RectF(cx - btnW * 0.5f, btnY, cx + btnW * 0.5f, btnY + btnH);
                historyEmptyBtnBounds.clear();
                historyEmptyBtnBounds.push_back(openFileBtn.bounds);
                vm.historyTab.emptyState.buttons.push_back(std::move(openFileBtn));
            } else {
                historyEmptyBtnBounds.clear();
                const HistoryTimelineProjection& proj = GetHistoryProjection();
                float headerY = vm.viewBounds.top + kHistoryContentInset * dp - historyScrollY;
                // Search each group's implicit row positions; hidden rows
                // need neither view models nor a per-row layout walk.
                const float clipTop = vm.viewBounds.top - 80.0f * dp;
                const float clipBottom = vm.viewBounds.bottom + 80.0f * dp;
                for (size_t g = 0; g < proj.groups.size(); ++g) {
                    const HistoryGroupLayout layout(headerY, proj.groups[g].count, dp);
                    headerY = layout.NextHeaderTop();
                    HistoryGroupSummary grp = proj.groups[g];
                    grp.headerRect = D2D1::RectF(vm.viewBounds.left + 12.0f * dp, layout.headerTop,
                        vm.viewBounds.right - 24.0f * dp, layout.headerTop + layout.headerHeight);
                    if (grp.headerRect.bottom >= clipTop && grp.headerRect.top <= clipBottom) { vm.historyTab.groups.push_back(grp); }
                    const auto [firstRow, lastRow] = layout.VisibleRows(clipTop, clipBottom);
                    for (size_t row = firstRow; row < lastRow; ++row) {
                        const size_t idx = grp.startIndex + row;
                        size_t origIdx = proj.itemIndices[idx];
                        if (origIdx >= library.history.size()) continue;
                        const HistoryRecord& rec = library.history[origIdx];
                        const float rowY = layout.RowTop(row);
                        const D2D1_RECT_F itemRect = D2D1::RectF(vm.viewBounds.left + 12.0f * dp, rowY,
                            vm.viewBounds.right - 24.0f * dp, rowY + layout.rowHeight);
                        VisibleHistoryItemViewModel item;
                        item.originalIndex = origIdx;
                        item.pRecord = &rec;
                        item.itemRect = itemRect;
                        item.isHovered = (hoveredHistoryIndex == (int)origIdx);
                        item.isDeleteHovered = (hoveredHistoryDeleteIndex == (int)origIdx);
                        item.isSelected = (selectedHistoryIndices.count(origIdx) > 0);
                        item.isKeyboardFocused = isKeyboardNavActive &&
                            activeFocusPane == FocusPane::HistoryList && keyboardFocusedIndex == static_cast<int>(origIdx);
                        item.displayTitle = rec.title.empty() ? GetFileNameFromPath(rec.filePath) : rec.title;
                        item.formattedDuration = FormatHistoryProgress(rec);
                        vm.historyTab.visibleItems.push_back(std::move(item));
                    }
                }
            }
            break;
        }
        case TabView::Settings: {
            vm.settingsTab.bounds = vm.viewBounds;
            vm.settingsTab.activeCat = currentSettingsCat;
            vm.settingsTab.categoryIndicatorPos = settingsCatIndicator.current;
            vm.settingsTab.hoveredCat = hoveredSettingsCat;
            vm.settingsTab.hoveredRowIndex = hoveredSettingsRow;
            vm.settingsTab.scrollY = settingsScrollY;
            vm.settingsTab.rebindingActionId = rebindingActionId;
            vm.settingsTab.hoveredShortcutIndex = hoveredShortcutIndex;
            vm.settingsTab.hoveredShortcutUnassignIndex = hoveredShortcutUnassignIndex;
            vm.settingsTab.isResetShortcutsHovered = isResetShortcutsHovered;
            vm.settingsTab.isSidebarFocused = (activeFocusPane == FocusPane::SettingsCategoryList);
            vm.settingsTab.isContentFocused = (activeFocusPane == FocusPane::SettingsContent);
            vm.settingsTab.keyboardFocusedRow = keyboardFocusedIndex;
            vm.settingsTab.keyboardFocusedSubIndex = keyboardFocusedSubIndex;
            vm.settingsTab.autoplayProg = autoplayToggleAnim.current;
            vm.settingsTab.rememberPosProg = rememberPosToggleAnim.current;
            vm.settingsTab.hwAccelProg = hwAccelToggleAnim.current;
            vm.settingsTab.loopProg = loopToggleAnim.current;
            vm.settingsTab.customMpvConfigProg = customMpvConfigToggleAnim.current;
            vm.settingsTab.globalHotkeysProg = settings.globalHotkeysEnabled ? 1.0f : 0.0f;
            vm.settingsTab.trayIconProg = settings.trayIconEnabled ? 1.0f : 0.0f;
            vm.settingsTab.minimizeToTrayProg = settings.minimizeToTray ? 1.0f : 0.0f;
            vm.settingsTab.isKeyboardNavActive = isKeyboardNavActive;
            vm.settingsTab.pressState = settingsPressState;
            vm.settingsTab.mouseX = hubMouseX;
            vm.settingsTab.mouseY = hubMouseY;
            for (int i = 0; i < SettingsLayout::kSystemRowCount; ++i) {
                vm.settingsTab.systemRowHoverProg[i] = systemRowHoverAnim[i].current;
                vm.settingsTab.systemRowPressProg[i] = systemRowPressAnim[i].current;
            }
            break;
        }
    }
    // Autoscroll
    if (IsAutoscrolling()) {
        vm.autoscroll.isActive = true;
        vm.autoscroll.anchorPoint = autoscroll.anchorPoint;
        vm.autoscroll.dy = autoscroll.currentPoint.y - autoscroll.anchorPoint.y;
    }
    // Listen island (now-playing audio) anchored next to History.
    if (listenPlaybackActive || listenIslandRetained || listenIslandFade.current > 0.001f) {
        auto& li = vm.listenIsland;
        li.isActive = listenPlaybackActive || listenIslandRetained;
        li.isRadio = listenPlaybackIsRadio;
        li.paused = listenPlaybackActive ? listenPlaybackPaused : true;
        li.title = listenTitle;
        li.subtitle = listenSubtitle;
        li.volume = std::clamp(settings.volume, 0.0, 1.0);
        li.muted = settings.muted || (li.volume <= 0.001);
        li.eqPhase = (float)std::fmod(listenEqPhase, 1000.0);
        li.pillHovered = listenPillHovered;
        li.badgeHovered = listenBadgeHovered;
        li.muteHovered = listenMuteHovered;
        li.volumeHovered = listenVolumeHovered;
        li.volumeDragging = isListenVolumeDragging;
        li.seekHovered = listenSeekHovered || isListenSeekDragging;
        li.xHovered = listenXHovered;
        li.widthDip = std::max(180.0f, listenIslandWidthDip.current);
        li.heightDip = std::max(32.0f, listenIslandHeightDip.current);
        li.radiusDip = std::max(8.0f, listenIslandRadiusDip.current);
        li.expand = std::clamp(listenIslandExpand.current, 0.0f, 1.0f);
        li.fade = std::clamp(listenIslandFade.current, 0.0f, 1.0f);
        li.pillBounds = ListenPillBounds(dp);
        const float w = li.widthDip * dp, h = li.heightDip * dp;
        const float left = li.pillBounds.left;
        const float top = li.pillBounds.top;
        li.expandedBounds = D2D1::RectF(left, top, left + w, top + h);
        li.hitRect = li.expandedBounds;
        const bool queryTimeline = listenPlaybackActive && !listenPlaybackIsRadio; // suspended: progress resets, times hide
        const double pos = queryTimeline ? listenPlayer.GetPositionSec() : -1.0;
        const double dur = queryTimeline ? listenPlayer.GetDurationSec() : -1.0;
        li.progressRatio = (pos >= 0.0 && dur > 0.0) ? std::clamp(pos / dur, 0.0, 1.0) : 0.0;
        li.timeText = (pos >= 0.0) ? (FormatDuration(pos) + (dur > 0.0 ? L" / " + FormatDuration(dur) : L"")) : L"";
        li.remainingText = (dur > 0.0 && pos >= 0.0) ? (L"-" + FormatDuration(std::max(0.0, dur - pos))) : L"";
        // Always-present top row controls (Play/Pause, Mute, Volume slider, Close X).
        const D2D1_RECT_F& card = li.expandedBounds;
        const float rowCy = card.top + 18.0f * dp;
        const float badgeSz = 26.0f * dp;
        li.badgeBounds = D2D1::RectF(card.left + 6.0f * dp, rowCy - badgeSz * 0.5f,
                                     card.left + 6.0f * dp + badgeSz, rowCy + badgeSz * 0.5f);
        const float xSz = 24.0f * dp;
        li.xBounds = D2D1::RectF(card.right - 6.0f * dp - xSz, rowCy - xSz * 0.5f,
                                 card.right - 6.0f * dp, rowCy + xSz * 0.5f);
        // Expand chevron sits between the volume slider and the close button.
        li.chevronBounds = D2D1::RectF(li.xBounds.left - 20.0f * dp, rowCy - 7.0f * dp,
                                       li.xBounds.left - 6.0f * dp, rowCy + 7.0f * dp);
        const float volW = 52.0f * dp, volHitH = 22.0f * dp;
        const float volRight = li.chevronBounds.left - 4.0f * dp;
        const float volLeft = volRight - volW;
        li.volumeTrackBounds = D2D1::RectF(volLeft, rowCy - volHitH * 0.5f, volRight, rowCy + volHitH * 0.5f);
        const float muteSz = 24.0f * dp;
        const float muteRight = volLeft - 3.0f * dp;
        li.muteBounds = D2D1::RectF(muteRight - muteSz, rowCy - muteSz * 0.5f, muteRight, rowCy + muteSz * 0.5f);
        if (li.expand > 0.25f && !li.isRadio) {
            // Seek bar flanked by -15s/+15s skip buttons.
            const float pad = 14.0f * dp;
            const float skipW = 34.0f * dp, skipH = 20.0f * dp;
            const float barY = card.bottom - 14.0f * dp;
            li.skipBackBounds = D2D1::RectF(card.left + pad, barY - skipH * 0.5f, card.left + pad + skipW, barY + skipH * 0.5f);
            li.skipFwdBounds = D2D1::RectF(card.right - pad - skipW, barY - skipH * 0.5f, card.right - pad, barY + skipH * 0.5f);
            li.seekBounds = D2D1::RectF(li.skipBackBounds.right + 8.0f * dp, card.bottom - 22.0f * dp,
                                        li.skipFwdBounds.left - 8.0f * dp, card.bottom - 6.0f * dp);
        } else {
            li.seekBounds = D2D1::RectF(0, 0, 0, 0);
            li.skipBackBounds = D2D1::RectF(0, 0, 0, 0);
            li.skipFwdBounds = D2D1::RectF(0, 0, 0, 0);
        }
        li.skipBackHovered = listenSkipBackHovered;
        li.skipFwdHovered = listenSkipFwdHovered;
        listenIslandHitRect = li.hitRect;
        listenIslandBadgeBounds = li.badgeBounds;
        listenIslandMuteBounds = li.muteBounds;
        listenIslandVolumeBounds = li.volumeTrackBounds;
        listenIslandSeekBounds = li.seekBounds;
        listenIslandSkipBackBounds = li.skipBackBounds;
        listenIslandSkipFwdBounds = li.skipFwdBounds;
        listenIslandXBounds = li.xBounds;
    }
    // Import island: compact pill, hover-expanded card, complete check, exit.
    if (importIslandPhase != 0 || importIslandFade.current > 0.001f) {
        auto& ip = vm.importProgress;
        ip.phase = importIslandPhase;
        const size_t processed = importProcessed.load(std::memory_order_acquire);
        const size_t total = importTotal.load(std::memory_order_acquire);
        ip.processed = processed;
        ip.total = total;
        ip.progressRatio = total ? (double)processed / (double)total : 0.0;
        const std::wstring phase = importCancelRequested.load(std::memory_order_acquire) ? L"Canceling"
            : (total > 0 && processed >= total ? L"Finalizing" : L"Scanning and validating");
        ip.progressText = total > 0
            ? (phase + L" \u2014 " + std::to_wstring(std::min(processed, total)) + L" of " + std::to_wstring(total) + L" files")
            : phase + L" folders";
        ip.pillHovered = importPillHovered;
        ip.xHovered = importCancelHovered;
        ip.widthDip = std::max(8.0f, importIslandWidthDip.current);
        ip.heightDip = std::max(8.0f, importIslandHeightDip.current);
        ip.radiusDip = std::max(4.0f, importIslandRadiusDip.current);
        ip.expand = std::clamp(importIslandExpand.current, 0.0f, 1.0f);
        ip.fade = std::clamp(importIslandFade.current, 0.0f, 1.0f);
        ip.spinAngle = importSpinAngle;
        ip.pillBounds = ImportPillBounds(dp);
        {
            const float w = ip.widthDip * dp, h = ip.heightDip * dp;
            if (ip.heightDip < 30.0f) {
                const float cx = (ip.pillBounds.left + ip.pillBounds.right) * 0.5f;
                const float cy = (ip.pillBounds.top + ip.pillBounds.bottom) * 0.5f;
                ip.expandedBounds = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
            } else {
                const float right = ip.pillBounds.right;
                const float left = std::max(12.0f * dp, right - w);
                const float top = ip.pillBounds.top - 2.0f * dp * ip.expand;
                ip.expandedBounds = D2D1::RectF(left, top, right, top + h);
            }
        }
        ip.hitRect = (ip.expand > 0.05f) ? IslandRectUnion(ip.pillBounds, ip.expandedBounds) : ip.pillBounds;
        const float eA = ip.expand;
        if (eA > 0.05f) {
            const D2D1_RECT_F& card = ip.expandedBounds;
            const float pad = 15.0f * dp;
            const float xSz = 24.0f * dp;
            const float cyCard = card.top + 14.0f * dp + 14.0f * dp;
            ip.xBounds = D2D1::RectF(card.right - pad - xSz, cyCard - xSz * 0.5f, card.right - pad, cyCard + xSz * 0.5f);
        } else {
            ip.xBounds = D2D1::RectF(0, 0, 0, 0);
        }
        importIslandHitRect = ip.hitRect;
        importIslandXBounds = ip.xBounds;
    }
    // OSD Badge
    if (osdTimer > 0.0f) {
        vm.osdBadge.visible = true;
        float osdOpacity = IsReducedMotionEnabled() ? 1.0f : std::min(1.0f, osdTimer / MotionTokens::kModerate);
        D2D1_SIZE_F txtSz = renderer.MeasureString(osdText, FontType::Bold);
        float osdW = txtSz.width + 60.0f * dp, osdH = 36.0f * dp;
        float osdCX = (vm.winBounds.left + vm.winBounds.right) * 0.5f;
        vm.osdBadge.bounds = D2D1::RectF(osdCX - osdW * 0.5f, 20.0f * dp, osdCX + osdW * 0.5f, 20.0f * dp + osdH);
        vm.osdBadge.icon = osdIcon;
        vm.osdBadge.text = osdText;
        vm.osdBadge.opacity = osdOpacity;
    }
    // Context Menu
    if (isContextMenuOpen || contextMenuClosing) {
        PopulateContextMenuViewModel(vm.contextMenu);
    }
    vm.modal = BuildModalViewModel();
    // Global Search Surface
    if (globalSearchModel.IsOpen()) {
        vm.globalSearch.isOpen = true;
        vm.globalSearch.isFocused = (activeTextInputField == TextInputField::GlobalSearch);
        vm.globalSearch.isCloseHovered = isGlobalSearchCloseHovered;
        vm.globalSearch.query = globalSearchModel.GetQuery();
        const float cardW = std::min(440.0f * dp, winW - 36.0f * dp);
        const float cardH = std::min(390.0f * dp, winH - 106.0f * dp);
        const float cardLeft = 18.0f * dp, cardTop = 76.0f * dp;
        vm.globalSearch.searchBoxBounds = D2D1::RectF(cardLeft, cardTop, cardLeft + cardW, cardTop + cardH);
        vm.globalSearch.closeBounds = D2D1::RectF(cardLeft + cardW - 42.0f * dp, cardTop + 10.0f * dp,
            cardLeft + cardW - 10.0f * dp, cardTop + 42.0f * dp);
        vm.globalSearch.inputBounds = D2D1::RectF(cardLeft + 12.0f * dp, cardTop + 10.0f * dp,
            vm.globalSearch.closeBounds.left - 8.0f * dp, cardTop + 42.0f * dp);
        vm.globalSearch.listClipBounds = D2D1::RectF(cardLeft + 8.0f * dp, cardTop + 50.0f * dp,
            cardLeft + cardW - 8.0f * dp, cardTop + cardH - 26.0f * dp);
        vm.globalSearch.selectedIndex = globalSearchModel.GetSelectedIndex();
        vm.globalSearch.hoveredIndex = hoveredGlobalSearchIndex;
        auto& input = globalSearchModel.GetTextInputModel();
        vm.globalSearch.searchVisual.displayText = Text16ToWideString(input.GetDisplayText());
        vm.globalSearch.searchVisual.selectionStart = input.GetSelection().start;
        vm.globalSearch.searchVisual.selectionEnd = input.GetSelection().end;
        vm.globalSearch.searchVisual.caretIndex = input.GetCaret();
        vm.globalSearch.searchVisual.focused = vm.globalSearch.isFocused;
        vm.globalSearch.searchVisual.caretVisible = vm.globalSearch.isFocused && textInputCaretVisible;
        const auto& results = globalSearchModel.GetResults();
        vm.globalSearch.visibleItems.reserve(results.size());
        float itemY = cardTop + 52.0f * dp;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& srcItem = results[i];
            GlobalSearchItemViewModel itemVm;
            itemVm.filePath = srcItem.filePath;
            itemVm.title = srcItem.title;
            itemVm.fileName = srcItem.fileName;
            itemVm.durationText = srcItem.GetFormattedDuration();
            itemVm.progressRatio = srcItem.GetProgressRatio();
            itemVm.existsOnDisk = srcItem.existsOnDisk;
            itemVm.isSelected = (static_cast<int>(i) == vm.globalSearch.selectedIndex);
            itemVm.isHovered = (static_cast<int>(i) == hoveredGlobalSearchIndex);
            itemVm.itemRect = D2D1::RectF(cardLeft + 8.0f * dp, itemY, cardLeft + cardW - 8.0f * dp, itemY + 44.0f * dp);
            vm.globalSearch.visibleItems.push_back(std::move(itemVm));
            itemY += 46.0f * dp;
        }
    }
    return vm;
}

} // namespace Aperture
