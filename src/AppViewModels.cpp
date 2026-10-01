#include "App.h"
#include "Reorder.h"
#include "AppLayout.h"
#include "Presentation.h"
#include <ctime>
#include <unordered_map>

namespace Aperture {

namespace {
constexpr ULONGLONG kMissingFileCacheTtlMs = 4000;
constexpr size_t kMissingFileCacheMaxEntries = 4096;
}

bool App::IsMediaFileMissing(const std::wstring& path) {
    if (path.empty()) return true;
    const ULONGLONG now = GetTickCount64();
    auto it = m_missingFileCache.find(path);
    if (it != m_missingFileCache.end() && now - it->second.checkedTick < kMissingFileCacheTtlMs) {
        return it->second.missing;
    }
    const bool missing = !FileExists(path);
    if (it != m_missingFileCache.end()) {
        it->second.missing = missing;
        it->second.checkedTick = now;
    } else {
        if (m_missingFileCache.size() >= kMissingFileCacheMaxEntries) { m_missingFileCache.clear(); }
        m_missingFileCache.emplace(path, MissingFileEntry{ missing, now });
    }
    return missing;
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
    const uint64_t version = player.GetChaptersVersion();
    const double duration = player.GetDuration();
    const double currentTime = player.GetCurrentTime();
    // The isPassed set is constant between chapter crossings; quarter-second
    // granularity caps rebuilds at four per second worst case.
    const long long quarter = (long long)(currentTime * 4.0);
    if (m_chapterMarkersSnapshot && version == m_chapterMarkersSnapshotVersion &&
        quarter == m_chapterMarkersSnapshotQuarter && duration == m_chapterMarkersSnapshotDuration) {
        return m_chapterMarkersSnapshot;
    }
    auto markers = std::make_shared<std::vector<TimelineChapterMarker>>();
    if (duration > 0.0) {
        markers->reserve(std::min<size_t>(chapters.size(), 500));
        for (size_t i = 0; i < chapters.size() && i < 500; ++i) {
            const auto& ch = chapters[i];
            TimelineChapterMarker marker;
            marker.time = ch.time;
            marker.normalizedPosition = std::clamp(static_cast<float>(ch.time / duration), 0.0f, 1.0f);
            marker.title = ch.title.length() > 80 ? ch.title.substr(0, 77) + L"..." : ch.title;
            marker.isPassed = (ch.time <= currentTime);
            markers->push_back(std::move(marker));
        }
    }
    m_chapterMarkersSnapshot = std::move(markers);
    m_chapterMarkersSnapshotVersion = version;
    m_chapterMarkersSnapshotQuarter = quarter;
    m_chapterMarkersSnapshotDuration = duration;
    return m_chapterMarkersSnapshot;
}

const HistoryTimelineProjection& App::GetHistoryProjection() {
    // Group buckets depend on the wall-clock day; ordering and membership only
    // change when the library changes, so the projection is stable between
    // those events instead of being rebuilt (and re-sorted) every frame.
    const int64_t nowSec = GetCurrentTimestampSec();
    const std::time_t ttNow = (std::time_t)nowSec;
    std::tm tmNow{};
    localtime_s(&tmNow, &ttNow);
    const int64_t day = tmNow.tm_year * 366 + tmNow.tm_yday;
    if (m_historyProjectionVersion == library.GetVersion() && m_historyProjectionDay == day &&
        m_historyProjectionSize == library.history.size()) {
        return m_historyProjection;
    }
    m_historyProjection = ProjectHistoryTimeline(library.history);
    m_historyProjectionVersion = library.GetVersion();
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
        const auto pipLayout = Presentation::PiP(RectWidth(vm.winBounds), RectHeight(vm.winBounds), dp);
        vm.pipOverlay.volumeSliderBounds = pipLayout.volumeSlider;
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
        vm.bottomBar.hoveredButtonId = hoveredPlayerButtonId;
        vm.bottomBar.focusedControlId = playerKeyboardFocusId;
        vm.bottomBar.showKeyboardFocus = isKeyboardNavActive;
        vm.bottomBar.opacity = controlsOpacity;
        const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
        double duration = player.GetDuration();
        double currentTime = player.GetCurrentTime();
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
            if (vm.settingsFlyout.subTracks[i].isSelected) { vm.settingsFlyout.activeSubIndex = (int)i; break; }
        }
        vm.settingsFlyout.audioTracks = GetCachedAudioTracks();
        vm.settingsFlyout.activeAudioIndex = -1;
        for (size_t i = 0; i < vm.settingsFlyout.audioTracks.size(); ++i) {
            if (vm.settingsFlyout.audioTracks[i].isSelected) { vm.settingsFlyout.activeAudioIndex = (int)i; break; }
        }
        vm.settingsFlyout.currentSpeed = player.GetPlaybackRate();
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
        vm.diagnosticsOverlay.bounds = vm.winBounds;
        vm.diagnosticsOverlay.diagnostics = player.GetDiagnostics();
        vm.diagnosticsOverlay.audioDelayMs = settings.audioDelayMs;
        vm.diagnosticsOverlay.subDelayMs = settings.subDelayMs;
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
        vm.contextMenu.isOpen = true;
        vm.contextMenu.origin = contextMenuOrigin;
        vm.contextMenu.items = contextMenuItems;
        vm.contextMenu.hoveredIndex = hoveredContextMenuItem;
        vm.contextMenu.animProgress = contextMenuAnim.current;
    }
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
            vm.mediaTab.folderBtnBounds = toolbarGeometry.folder;
            vm.mediaTab.seriesBtnBounds = toolbarGeometry.series;
            mediaSeriesBtnBounds = vm.mediaTab.seriesBtnBounds;
            vm.mediaTab.duplicatesBtnBounds = toolbarGeometry.duplicates;
            vm.mediaTab.activeGroupMode = static_cast<int>(libraryGroupMode);
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
            vm.mediaTab.compactToolbar = toolbarGeometry.compact;
            vm.mediaTab.showSearchLabel = !toolbarGeometry.compact;
            vm.mediaTab.showSummary = toolbarGeometry.showSummary;
            vm.mediaTab.sortLabel = GetLibrarySortLabel();
            static const LibraryQueryProjection kPendingProjection;
            static const std::vector<size_t> kPendingVisibleIndices;
            const auto& proj = libraryReady ? GetCurrentMediaProjection() : kPendingProjection;
            const auto& visibleIndices = libraryReady ? m_cachedVisibleMediaIndices : kPendingVisibleIndices;
            size_t totalCount = library.mediaItems.size();
            const bool hasSearch = !mediaSearchInput.GetText().empty();
            if (!libraryReady) {
                vm.mediaTab.summaryText = libraryLoadFailed ? L"Library unavailable" : L"Loading library\x2026";
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
                    size_t dupEntries = (proj.items.size() > proj.groups.size()) ? (proj.items.size() - proj.groups.size()) : 0;
                    vm.mediaTab.summaryText = std::to_wstring(proj.groups.size()) + L" duplicate groups \x2022 " + std::to_wstring(dupEntries) + L" duplicates";
                }
            } else { vm.mediaTab.summaryText = std::to_wstring(totalCount) + L" videos"; }
            vm.mediaTab.summaryTextBounds = toolbarGeometry.summary;
            bool showEmptyState = (libraryGroupMode == LibraryGroupMode::Flat) ? visibleIndices.empty()
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
                        vm.mediaTab.emptyState.body = L"No duplicate videos were detected. Switch back to Flat to see every file.";
                    } else {
                        vm.mediaTab.emptyState.title = L"No matching media found";
                        vm.mediaTab.emptyState.body = L"No media matches the current grouping. Switch back to Flat to see every file.";
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
                    for (size_t g = 0; g < proj.groups.size(); ++g) {
                        if (g >= m_mediaLayoutCache.groupHeaderOffsetsDIP.size()) break;
                        const auto& grp = proj.groups[g];
                        float headerTopDIP = m_mediaLayoutCache.groupHeaderOffsetsDIP[g];
                        float headerTopPx = listTop + (headerTopDIP * dp) - mediaScrollY;
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
                            float delSz = 15.0f * dp;
                            float delY = hvm.headerRect.top + (RectHeight(hvm.headerRect) - delSz) * 0.5f;
                            hvm.deleteRect = D2D1::RectF(hvm.headerRect.right - 24.0f * dp, delY, hvm.headerRect.right - 8.0f * dp, delY + delSz);
                            float dragHandleW = 12.0f * dp, dragHandleH = 14.0f * dp;
                            float dragHandleY = hvm.headerRect.top + (RectHeight(hvm.headerRect) - dragHandleH) * 0.5f;
                            hvm.dragHandleRect = D2D1::RectF(hvm.headerRect.left + 4.0f * dp, dragHandleY, hvm.headerRect.left + 4.0f * dp + dragHandleW, dragHandleY + dragHandleH);
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
                    vm.mediaTab.dragToFolderIndex = dragToIndex;
                    vm.mediaTab.folderDragOffsetY = dragCurrentY - dragStartY;
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
            vm.playlistsTab.rail.hoveredIndex = hoveredPlaylistRailIndex;
            vm.playlistsTab.rail.isFolderDropActive = isRailFolderDropActive;
            vm.playlistsTab.rail.isAddHovered = isPlaylistAddHovered;
            vm.playlistsTab.rail.scrollY = playlistRailScrollY;
            vm.playlistsTab.rail.dragFromIndex = (activeDragType == DragType::PlaylistRail) ? dragFromIndex : -1;
            vm.playlistsTab.rail.dragToIndex = (activeDragType == DragType::PlaylistRail) ? dragToIndex : -1;
            vm.playlistsTab.rail.dragOffsetY = (activeDragType == DragType::PlaylistRail) ? (dragCurrentY - dragStartY) : 0.0f;
            vm.playlistsTab.rail.isRailFocused = isKeyboardNavActive && (activeFocusPane == FocusPane::PlaylistRail);
            vm.playlistsTab.rail.focusedItemIndex = keyboardFocusedIndex;
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
            vm.playlistsTab.rail.items.reserve(library.playlists.size());
            for (size_t i = 0; i < library.playlists.size(); ++i) {
                if (railDragFrom >= 0 && (int)i == railDragFrom) { curRailY += itemH + 6.0f * dp; continue; }
                float renderY = curRailY;
                if (railDragFrom >= 0 && railDragTo >= 0) {
                    const int slot = Reorder::Slot(static_cast<int>(i), railDragFrom, railDragTo);
                    renderY += CardDragOffset(static_cast<int>(i), 0, (slot - static_cast<int>(i)) * (itemH + 6.0f * dp)).y;
                }
                const auto& pl = library.playlists[i];
                // Stats come from the cached projection; the raw item count is
                // the same materialized path count it is derived from.
                PlaylistStats plStats = library.GetPlaylistStats(pl.id);
                PlaylistRailItemViewModel rItem;
                rItem.id = pl.id;
                rItem.name = pl.name;
                rItem.isSmart = pl.IsSmart();
                rItem.rawItemCount = (size_t)plStats.totalCount;
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
                float railClipTop = vm.playlistsTab.rail.bounds.top + headerH - 4.0f * dp;
                float railClipBottom = vm.playlistsTab.rail.bounds.bottom - bottomBtnH - 18.0f * dp;
                if (renderY + itemH >= railClipTop && renderY <= railClipBottom) { vm.playlistsTab.rail.items.push_back(std::move(rItem)); }
                curRailY += itemH + 6.0f * dp;
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
                vm.playlistsTab.detail.isEditRulesHovered = isPlaylistEditRulesHovered;
                vm.playlistsTab.detail.isDeletePlHovered = isPlaylistDeleteHovered;
                vm.playlistsTab.detail.isResumeHovered = isPlaylistResumeHovered;
                vm.playlistsTab.detail.isDropTargetActive = isPlaylistDropTargetActive;
                vm.playlistsTab.detail.isFolderDrop = isPlaylistDropTargetActive && oleDragPayloadHasFolder;
                vm.playlistsTab.detail.isEmpty = plItemPaths.empty();
                if (!plItemPaths.empty()) {
                    size_t resumeIndex = library.GetPlaylistResumeIndex(plItemPaths);
                    const std::wstring* pResumePath = (resumeIndex < plItemPaths.size()) ? &plItemPaths[resumeIndex] : &plItemPaths[0];
                    MediaItem* pResumeMedia = pResumePath ? library.FindByPath(*pResumePath) : nullptr;
                    float detailHeadH = 50.0f * dp, resumeH = 112.0f * dp;
                    float curDetY = vm.playlistsTab.detail.listClipBounds.top + 14.0f * dp + detailHeadH + 16.0f * dp;
                    float paneRight = std::max(vm.playlistsTab.detail.listClipBounds.left + 120.0f * dp, vm.playlistsTab.detail.listClipBounds.right - 26.0f * dp);
                    vm.playlistsTab.detail.resumeCard.hasResumeCard = true;
                    vm.playlistsTab.detail.resumeCard.cardRect = D2D1::RectF(vm.playlistsTab.detail.listClipBounds.left, curDetY, paneRight, curDetY + resumeH);
                    vm.playlistsTab.detail.resumeCard.isResumeFocused = isKeyboardNavActive && (activeFocusPane == FocusPane::PlaylistDetail && keyboardFocusedIndex == -1);
                    vm.playlistsTab.detail.resumeCard.isResumeHovered = isPlaylistResumeHovered;
                    if (previewPlayer.IsActive() && previewPlayer.GetCurrentPreviewPath() == vm.playlistsTab.detail.resumeCard.filePath) {
                        vm.playlistsTab.detail.resumeCard.pLivePreviewBitmap = previewPlayer.GetPreviewBitmap();
                        vm.playlistsTab.detail.resumeCard.previewOpacity = resumePreviewAnim.current;
                    }
                    float thumbW = std::min(220.0f * dp, std::max(80.0f * dp, RectWidth(vm.playlistsTab.detail.resumeCard.cardRect) * 0.30f));
                    float thumbH = std::min(84.0f * dp, std::max(45.0f * dp, resumeH - 28.0f * dp));
                    float thumbX = vm.playlistsTab.detail.resumeCard.cardRect.left + 16.0f * dp;
                    float thumbY = vm.playlistsTab.detail.resumeCard.cardRect.top + (resumeH - thumbH) * 0.5f;
                    vm.playlistsTab.detail.resumeCard.thumbRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
                    vm.playlistsTab.detail.resumeCard.filePath = pResumePath ? *pResumePath : L"";
                    vm.playlistsTab.detail.resumeCard.progressRatio = pResumeMedia ? pResumeMedia->GetProgressRatio() : 0.0;
                    // Cache for the Update()-side hover-preview driver.
                    resumePreviewPath = vm.playlistsTab.detail.resumeCard.filePath;
                    resumeResumePos = (pResumeMedia && pResumeMedia->lastPosSec > 5.0) ? pResumeMedia->lastPosSec : 0.0;
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
            vm.historyTab.selectedIndices = selectedHistoryIndices;
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
                float curY = vm.viewBounds.top + 87.0f * dp - historyScrollY;
                const float kGroupHeaderH = 34.0f * dp, kItemH = 76.0f * dp, kItemGap = 8.0f * dp, kGroupGap = 16.0f * dp;
                // Only rows inside the (margin-extended) viewport get view
                // models; offscreen rows are pure layout arithmetic.
                const float clipTop = vm.viewBounds.top - 80.0f * dp;
                const float clipBottom = vm.viewBounds.bottom + 80.0f * dp;
                for (size_t g = 0; g < proj.groups.size(); ++g) {
                    if (g > 0) curY += kGroupGap;
                    HistoryGroupSummary grp = proj.groups[g];
                    grp.headerRect = D2D1::RectF(vm.viewBounds.left + 12.0f * dp, curY, vm.viewBounds.right - 24.0f * dp, curY + kGroupHeaderH);
                    if (grp.headerRect.bottom >= clipTop && grp.headerRect.top <= clipBottom) { vm.historyTab.groups.push_back(grp); }
                    curY += kGroupHeaderH + 6.0f * dp;
                    for (size_t idx = grp.startIndex; idx < grp.startIndex + grp.count; ++idx) {
                        size_t origIdx = proj.itemIndices[idx];
                        if (origIdx >= library.history.size()) continue;
                        const HistoryRecord& rec = library.history[origIdx];
                        const D2D1_RECT_F itemRect = D2D1::RectF(vm.viewBounds.left + 12.0f * dp, curY, vm.viewBounds.right - 24.0f * dp, curY + kItemH);
                        curY += kItemH + kItemGap;
                        if (itemRect.bottom < clipTop || itemRect.top > clipBottom) continue;
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
            vm.settingsTab.hoveredCat = hoveredSettingsCat;
            vm.settingsTab.hoveredRowIndex = hoveredSettingsRow;
            vm.settingsTab.scrollY = settingsScrollY;
            vm.settingsTab.rebindingActionId = rebindingActionId;
            vm.settingsTab.hoveredShortcutIndex = hoveredShortcutIndex;
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
    // Import Progress
    if (importJobActive) {
        vm.importProgress.isActive = true;
        const float cardW = std::min(500.0f * dp, std::max(300.0f * dp, winW - 40.0f * dp));
        const float cardH = 184.0f * dp;
        const float cardL = (winW - cardW) * 0.5f;
        const float cardT = std::max(70.0f * dp, (winH - cardH) * 0.5f);
        vm.importProgress.cardBounds = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
        const size_t processed = importProcessed.load(std::memory_order_acquire);
        const size_t total = importTotal.load(std::memory_order_acquire);
        vm.importProgress.processed = processed;
        vm.importProgress.total = total;
        vm.importProgress.progressRatio = total ? (double)processed / (double)total : 0.0;
        const std::wstring phase = importCancelRequested.load(std::memory_order_acquire) ? L"Canceling"
            : (total > 0 && processed >= total ? L"Finalizing" : L"Scanning and validating");
        vm.importProgress.progressText = total > 0
            ? (phase + L" — " + std::to_wstring(std::min(processed, total)) + L" of " + std::to_wstring(total) + L" files")
            : phase + L" folders";
        const float cancelW = 112.0f * dp;
        vm.importProgress.cancelBounds = D2D1::RectF(vm.importProgress.cardBounds.right - 24.0f * dp - cancelW, vm.importProgress.cardBounds.bottom - 52.0f * dp, vm.importProgress.cardBounds.right - 24.0f * dp, vm.importProgress.cardBounds.bottom - 16.0f * dp);
        vm.importProgress.isCancelHovered = importCancelHovered;
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
        vm.contextMenu.isOpen = true;
        vm.contextMenu.origin = contextMenuOrigin;
        vm.contextMenu.items = contextMenuItems;
        vm.contextMenu.hoveredIndex = hoveredContextMenuItem;
        vm.contextMenu.animProgress = contextMenuAnim.current;
    }
    // Modal
    if (activeModal != ModalType::None) {
        vm.modal.type = activeModal;
        vm.modal.title = modalTitle;
        vm.modal.prompt = modalPrompt;
        vm.modal.message = modalMessage;
        vm.modal.confirmText = modalConfirmText;
        vm.modal.isDestructive = modalIsDestructive;
        vm.modal.textValue = Text16ToWideString(modalInput.GetText());
        const bool confirmation = activeModal == ModalType::Confirmation || activeModal == ModalType::DeletePlaylist;
        vm.modal.isOkHovered = isModalOkHovered || modalKeyboardFocus == (confirmation ? 0 : 1);
        vm.modal.isCancelHovered = isModalCancelHovered || modalKeyboardFocus == (confirmation ? 1 : 2);
        vm.modal.isOkPressed = isModalOkPressed;
        vm.modal.isCancelPressed = isModalCancelPressed;
        if (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist || activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput || activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput || activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides || activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile) {
            vm.modal.visualState.displayText = Text16ToWideString(modalInput.GetDisplayText());
            vm.modal.visualState.selectionStart = modalInput.GetSelection().start;
            vm.modal.visualState.selectionEnd = modalInput.GetSelection().end;
            vm.modal.visualState.caretIndex = modalInput.GetCaret();
            vm.modal.visualState.focused = (activeTextInputField == TextInputField::Modal);
            vm.modal.visualState.caretVisible = vm.modal.visualState.focused && textInputCaretVisible;
            if (modalInput.IsComposing()) {
                const auto compositionRange = modalInput.GetCompositionRange();
                vm.modal.visualState.hasCompositionRange = true;
                vm.modal.visualState.compositionStart = compositionRange.start;
                vm.modal.visualState.compositionEnd = compositionRange.end;
                vm.modal.visualState.compositionCaretIndex = modalInput.GetDisplayCaret();
            }
        }
    }
    // Global Search Surface
    if (globalSearchModel.IsOpen()) {
        vm.globalSearch.isOpen = true;
        vm.globalSearch.isFocused = (activeTextInputField == TextInputField::GlobalSearch);
        vm.globalSearch.isCloseHovered = isGlobalSearchCloseHovered;
        vm.globalSearch.query = globalSearchModel.GetQuery();
        vm.globalSearch.overlayBounds = vm.winBounds;
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
        vm.globalSearch.totalMatches = globalSearchModel.GetTotalMatchCount();
        vm.globalSearch.visibleItems.reserve(results.size());
        float itemY = cardTop + 52.0f * dp;
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& srcItem = results[i];
            GlobalSearchItemViewModel itemVm;
            itemVm.filePath = srcItem.filePath;
            itemVm.title = srcItem.title;
            itemVm.fileName = srcItem.fileName;
            itemVm.durationText = srcItem.GetFormattedDuration();
            itemVm.sourceBadge = srcItem.GetSourceDisplayName();
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
