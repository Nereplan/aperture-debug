#pragma once

#include "../support/TestFramework.h"
#include "../../src/RenderViewModel.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Settings.h"

namespace Aperture::Test {

TEST_CASE(RenderViewModel, PlayerOverlaySnapshotDefaults) {
    PlayerRenderViewModel vm;
    TEST_ASSERT_EQ(vm.winBounds.left, 0.0f);
    TEST_ASSERT_EQ(vm.winBounds.top, 0.0f);
    TEST_ASSERT_EQ(vm.winBounds.right, 0.0f);
    TEST_ASSERT_EQ(vm.winBounds.bottom, 0.0f);
    TEST_ASSERT_EQ(vm.dpiScale, 1.0f);
    TEST_ASSERT(!vm.isPiP);

    // Subtitle defaults
    TEST_ASSERT(!vm.subtitle.visible);
    TEST_ASSERT(vm.subtitle.text.empty());

    // Double-tap & Play/Pause ripple defaults
    TEST_ASSERT(!vm.doubleTap.visible);
    TEST_ASSERT(vm.doubleTap.isForward);
    TEST_ASSERT_EQ(vm.doubleTap.totalSeconds, 0.0);
    TEST_ASSERT(vm.doubleTap.chevronSlides.empty());
    TEST_ASSERT_EQ(vm.doubleTap.signPop, 1.0f);
    TEST_ASSERT_EQ(vm.doubleTap.numberPop, 1.0f);
    TEST_ASSERT(!vm.doubleTap.fadeSideVisible);
    TEST_ASSERT(!vm.playPauseBadge.visible);

    // Top & Bottom bar defaults
    TEST_ASSERT(vm.topBar.visible);
    TEST_ASSERT(!vm.topBar.isBackHovered);
    TEST_ASSERT_EQ(vm.topBar.opacity, 1.0f);

    TEST_ASSERT(vm.bottomBar.visible);
    TEST_ASSERT(!vm.bottomBar.isPlaying);
    TEST_ASSERT_EQ(vm.bottomBar.currentTime, 0.0);
    TEST_ASSERT_EQ(vm.bottomBar.duration, 0.0);
    TEST_ASSERT_EQ(vm.bottomBar.volume, 100.0);
    TEST_ASSERT(!vm.bottomBar.isMuted);
    TEST_ASSERT(!vm.bottomBar.isScrubbing);
    TEST_ASSERT(!vm.bottomBar.isFullscreen);
    TEST_ASSERT(!vm.bottomBar.isPiP);
    TEST_ASSERT_EQ(vm.bottomBar.timelineHoverProgress, 0.0f);

    // Flyout & Diagnostics & Context Menu defaults
    TEST_ASSERT(!vm.settingsFlyout.isOpen);
    TEST_ASSERT_EQ(vm.settingsFlyout.currentMenu, std::string("main"));
    TEST_ASSERT(!vm.osdBadge.visible);
    TEST_ASSERT(!vm.diagnosticsOverlay.visible);
    TEST_ASSERT(!vm.contextMenu.isOpen);
}

TEST_CASE(RenderViewModel, HubSnapshotDefaultsAndTabInvariants) {
    HubRenderViewModel vm;
    TEST_ASSERT_EQ(vm.currentTab, TabView::Media);
    TEST_ASSERT_EQ(vm.navBar.activeTab, TabView::Media);
    TEST_ASSERT_EQ(vm.navBar.focusedNavIndex, -1);
    TEST_ASSERT(!vm.navBar.isSettingsHovered);
    TEST_ASSERT(!vm.navBar.isOpenHovered);

    // Media tab
    TEST_ASSERT(!vm.mediaTab.isEmpty);
    TEST_ASSERT(vm.mediaTab.visibleCards.empty());
    TEST_ASSERT(!vm.mediaTab.isMediaDrag);
    TEST_ASSERT_EQ(vm.mediaTab.dragFromIndex, -1);

    // Playlists tab
    TEST_ASSERT(!vm.playlistsTab.rail.isRailFocused);
    TEST_ASSERT(vm.playlistsTab.rail.items.empty());
    TEST_ASSERT(!vm.playlistsTab.detail.hasPlaylist);
    TEST_ASSERT(!vm.playlistsTab.detail.resumeCard.hasResumeCard);
    TEST_ASSERT(vm.playlistsTab.detail.visibleEpisodes.empty());

    // History tab
    TEST_ASSERT(vm.historyTab.pHistory == nullptr);
    TEST_ASSERT_EQ(vm.historyTab.hoveredIndex, -1);
    TEST_ASSERT_EQ(vm.historyTab.hoveredDeleteIndex, -1);
    TEST_ASSERT(!vm.historyTab.isClearHovered);
    TEST_ASSERT(!vm.historyTab.isClearPressed);

    // Settings tab
    TEST_ASSERT_EQ(vm.settingsTab.activeCat, SettingsCategory::Appearance);
    TEST_ASSERT_EQ(vm.settingsTab.hoveredRowIndex, -1);
    TEST_ASSERT(!vm.settingsTab.isSidebarFocused);
    TEST_ASSERT(!vm.settingsTab.isContentFocused);
    for (int i = 0; i < SettingsLayout::kSystemRowCount; ++i) {
        TEST_ASSERT_EQ(vm.settingsTab.systemRowHoverProg[i], 0.0f);
        TEST_ASSERT_EQ(vm.settingsTab.systemRowPressProg[i], 0.0f);
    }

    // Overlays & Modals
    TEST_ASSERT(!vm.autoscroll.isActive);
    TEST_ASSERT(!vm.importProgress.isActive);
    TEST_ASSERT(!vm.osdBadge.visible);
    TEST_ASSERT(!vm.contextMenu.isOpen);
    TEST_ASSERT_EQ(vm.modal.type, ModalType::None);
}

TEST_CASE(RenderViewModel, VisibleMediaCardBoundedProjection) {
    MediaTabViewModel mediaVm;
    mediaVm.clipBounds = D2D1::RectF(0.0f, 84.0f, 1200.0f, 800.0f);

    // The view model borrows library items, so the test owns the backing store.
    std::vector<MediaItem> ownedItems;
    ownedItems.reserve(3);
    // Simulate deriving a bounded projection for 3 visible items out of a 10,000 item library
    for (size_t i = 100; i < 103; ++i) {
        ownedItems.emplace_back();
        MediaItem& item = ownedItems.back();
        item.id = "item_" + std::to_string(i);
        item.title = L"Video " + std::to_wstring(i);
        item.durationSec = 120.0;
        item.lastPosSec = 60.0;
        VisibleMediaCardViewModel card;
        card.originalIndex = i;
        card.pItem = &item;
        card.effectiveStatus = item.status;
        card.itemRect = D2D1::RectF(20.0f, 84.0f + (i - 100) * 80.0f, 1180.0f, 84.0f + (i - 100) * 80.0f + 76.0f);
        card.isHovered = (i == 101);
        card.isSelected = false;
        card.isPinned = false;
        card.liftY = (i == 101) ? 1.5f : 0.0f;
        mediaVm.visibleCards.push_back(std::move(card));
    }

    TEST_ASSERT_EQ(mediaVm.visibleCards.size(), 3ull);
    TEST_ASSERT_EQ(mediaVm.visibleCards[0].originalIndex, 100ull);
    TEST_ASSERT_EQ(mediaVm.visibleCards[0].pItem->title, std::wstring(L"Video 100"));
    TEST_ASSERT_EQ(mediaVm.visibleCards[0].liftY, 0.0f);
    TEST_ASSERT(!mediaVm.visibleCards[0].isHovered);

    TEST_ASSERT_EQ(mediaVm.visibleCards[1].originalIndex, 101ull);
    TEST_ASSERT_EQ(mediaVm.visibleCards[1].pItem->title, std::wstring(L"Video 101"));
    TEST_ASSERT_EQ(mediaVm.visibleCards[1].liftY, 1.5f);
    TEST_ASSERT(mediaVm.visibleCards[1].isHovered);

    // Verify progress ratio invariant
    TEST_ASSERT_EQ(mediaVm.visibleCards[0].pItem->GetProgressRatio(), 0.5);
}

TEST_CASE(RenderViewModel, PlaylistRailAndDetailSnapshotInvariants) {
    PlaylistsTabViewModel plVm;

    // Rail derivation
    PlaylistRailItemViewModel railItem1;
    railItem1.id = "pl_favorites";
    railItem1.name = "Favorites";
    railItem1.totalVids = 10;
    railItem1.watchedCount = 4;
    railItem1.progressRatio = 0.4f;
    railItem1.isActive = true;
    plVm.rail.items.push_back(railItem1);

    PlaylistRailItemViewModel railItem2;
    railItem2.id = "pl_watchlater";
    railItem2.name = "Watch Later";
    railItem2.totalVids = 0;
    railItem2.watchedCount = 0;
    railItem2.progressRatio = 0.0f;
    railItem2.isActive = false;
    plVm.rail.items.push_back(railItem2);

    TEST_ASSERT_EQ(plVm.rail.items.size(), 2ull);
    TEST_ASSERT(plVm.rail.items[0].isActive);
    TEST_ASSERT_EQ(plVm.rail.items[0].progressRatio, 0.4f);
    TEST_ASSERT(!plVm.rail.items[1].isActive);
    TEST_ASSERT_EQ(plVm.rail.items[1].progressRatio, 0.0f);

    // Detail pane derivation
    plVm.detail.hasPlaylist = true;
    plVm.detail.playlistId = "pl_favorites";
    plVm.detail.playlistName = "Favorites";
    plVm.detail.metaStr = L"10 videos \x00B7 1.2 GB \x00B7 1h 30m total \x00B7 4 watched";
    plVm.detail.isEmpty = false;

    // Featured resume card
    plVm.detail.resumeCard.hasResumeCard = true;
    plVm.detail.resumeCard.title = L"Episode 5";
    plVm.detail.resumeCard.progressRatio = 0.75;
    plVm.detail.resumeCard.tagText = L"CONTINUE WATCHING";

    TEST_ASSERT(plVm.detail.hasPlaylist);
    TEST_ASSERT_EQ(plVm.detail.playlistName, std::string("Favorites"));
    TEST_ASSERT(plVm.detail.resumeCard.hasResumeCard);
    TEST_ASSERT_EQ(plVm.detail.resumeCard.tagText, std::wstring(L"CONTINUE WATCHING"));
    TEST_ASSERT_EQ(plVm.detail.resumeCard.progressRatio, 0.75);
}

TEST_CASE(RenderViewModel, ImportProgressAndDiagnosticsSnapshotInvariants) {
    // Import progress snapshot
    ImportProgressViewModel importVm;
    importVm.isActive = true;
    importVm.processed = 45;
    importVm.total = 100;
    importVm.progressRatio = 0.45;
    importVm.progressText = L"45 of 100 files";
    importVm.isCancelHovered = false;

    TEST_ASSERT(importVm.isActive);
    TEST_ASSERT_EQ(importVm.processed, 45ull);
    TEST_ASSERT_EQ(importVm.total, 100ull);
    TEST_ASSERT_EQ(importVm.progressRatio, 0.45);
    TEST_ASSERT_EQ(importVm.progressText, std::wstring(L"45 of 100 files"));

    // Diagnostics overlay snapshot
    DiagnosticsOverlayViewModel diagVm;
    diagVm.visible = true;
    diagVm.diagnostics.filePath = L"C:\\videos\\demo.mkv";
    diagVm.diagnostics.videoCodec = L"h264";
    diagVm.diagnostics.audioCodec = L"aac";
    diagVm.diagnostics.videoFps = 60.0;
    diagVm.diagnostics.displayFps = 59.94;
    diagVm.diagnostics.droppedFrames = 2;
    diagVm.audioDelayMs = 50;
    diagVm.subDelayMs = -100;

    TEST_ASSERT(diagVm.visible);
    TEST_ASSERT_EQ(diagVm.diagnostics.filePath, std::wstring(L"C:\\videos\\demo.mkv"));
    TEST_ASSERT_EQ(diagVm.diagnostics.videoCodec, std::wstring(L"h264"));
    TEST_ASSERT_EQ(diagVm.diagnostics.audioCodec, std::wstring(L"aac"));
    TEST_ASSERT_EQ(diagVm.diagnostics.videoFps, 60.0);
    TEST_ASSERT_EQ(diagVm.diagnostics.droppedFrames, 2ll);
    TEST_ASSERT_EQ(diagVm.audioDelayMs, 50);
    TEST_ASSERT_EQ(diagVm.subDelayMs, -100);
}

TEST_CASE(RenderViewModel, SettingsFlyoutAndContextMenuSnapshots) {
    SettingsFlyoutViewModel flyoutVm;
    flyoutVm.isOpen = true;
    flyoutVm.currentMenu = "audio";
    flyoutVm.currentSpeed = 1.25;
    flyoutVm.currentAspect = "16:9";
    flyoutVm.isLoop = true;
    flyoutVm.isAutoplay = false;

    MpvTrackInfo track1;
    track1.id = 1;
    track1.type = "audio";
    track1.title = L"English Stereo";
    track1.isSelected = true;
    flyoutVm.audioTracks.push_back(track1);

    MpvTrackInfo track2;
    track2.id = 2;
    track2.type = "audio";
    track2.title = L"Commentary";
    track2.isSelected = false;
    flyoutVm.audioTracks.push_back(track2);
    flyoutVm.activeAudioIndex = 0;

    TEST_ASSERT(flyoutVm.isOpen);
    TEST_ASSERT_EQ(flyoutVm.currentMenu, std::string("audio"));
    TEST_ASSERT_EQ(flyoutVm.currentSpeed, 1.25);
    TEST_ASSERT_EQ(flyoutVm.audioTracks.size(), 2ull);
    TEST_ASSERT_EQ(flyoutVm.activeAudioIndex, 0);
    TEST_ASSERT(flyoutVm.isLoop);
    TEST_ASSERT(!flyoutVm.isAutoplay);

    ContextMenuViewModel menuVm;
    menuVm.isOpen = true;
    menuVm.origin = { 150.0f, 200.0f };
    menuVm.hoveredIndex = 1;

    ContextMenuItem item1;
    item1.id = "play";
    item1.label = L"Play";
    menuVm.items.push_back(item1);

    ContextMenuItem item2;
    item2.id = "delete";
    item2.label = L"Delete";
    item2.isDanger = true;
    menuVm.items.push_back(item2);

    TEST_ASSERT(menuVm.isOpen);
    TEST_ASSERT_EQ(menuVm.origin.x, 150.0f);
    TEST_ASSERT_EQ(menuVm.origin.y, 200.0f);
    TEST_ASSERT_EQ(menuVm.items.size(), 2ull);
    TEST_ASSERT_EQ(menuVm.hoveredIndex, 1);
    TEST_ASSERT(menuVm.items[1].isDanger);
}

TEST_CASE(RenderViewModel, ChapterMarkersSnapshotAndGeometry) {
    PlayerRenderViewModel vm;
    TEST_ASSERT(!vm.bottomBar.chapterMarkers || vm.bottomBar.chapterMarkers->empty());
    TEST_ASSERT(vm.bottomBar.currentChapterTitle.empty());
    TEST_ASSERT(vm.topBar.chapterTitle.empty());

    double duration = 200.0;
    double currentTime = 60.0;

    std::vector<MpvChapterInfo> chapters = {
        { 0, L"Prologue", 0.0 },
        { 1, L"Act I: The Beginning", 30.0 },
        { 2, L"Act II: The Confrontation", 90.0 },
        { 3, L"Epilogue", 180.0 }
    };

    auto markers = std::make_shared<std::vector<TimelineChapterMarker>>();
    for (const auto& ch : chapters) {
        TimelineChapterMarker marker;
        marker.time = ch.time;
        marker.normalizedPosition = std::clamp(static_cast<float>(ch.time / duration), 0.0f, 1.0f);
        marker.title = ch.title;
        marker.isPassed = (ch.time <= currentTime);
        markers->push_back(marker);
    }
    vm.bottomBar.chapterMarkers = std::const_pointer_cast<const std::vector<TimelineChapterMarker>>(markers);
    const auto& chapterMarkers = *vm.bottomBar.chapterMarkers;

    TEST_ASSERT_EQ(chapterMarkers.size(), 4ull);
    TEST_ASSERT_EQ(chapterMarkers[0].normalizedPosition, 0.0f);
    TEST_ASSERT_EQ(chapterMarkers[1].normalizedPosition, 0.15f);
    TEST_ASSERT_EQ(chapterMarkers[2].normalizedPosition, 0.45f);
    TEST_ASSERT_EQ(chapterMarkers[3].normalizedPosition, 0.90f);

    // Check isPassed state
    TEST_ASSERT(chapterMarkers[0].isPassed);
    TEST_ASSERT(chapterMarkers[1].isPassed);
    TEST_ASSERT(!chapterMarkers[2].isPassed);
    TEST_ASSERT(!chapterMarkers[3].isPassed);

    // Current chapter calculation
    std::wstring curTitle;
    for (int i = (int)chapters.size() - 1; i >= 0; --i) {
        if (chapters[i].time <= currentTime + 0.25) {
            curTitle = chapters[i].title;
            break;
        }
    }
    vm.bottomBar.currentChapterTitle = curTitle;
    vm.topBar.chapterTitle = curTitle;

    TEST_ASSERT_EQ(vm.bottomBar.currentChapterTitle, std::wstring(L"Act I: The Beginning"));
    TEST_ASSERT_EQ(vm.topBar.chapterTitle, std::wstring(L"Act I: The Beginning"));
}

TEST_CASE(RenderViewModel, ChapterMarkersMalformedAndBoundedProjection) {
    PlayerRenderViewModel vm;

    // 1. Zero duration handling
    double zeroDuration = 0.0;
    TimelineChapterMarker zeroMarker;
    zeroMarker.time = 10.0;
    zeroMarker.normalizedPosition = (zeroDuration > 0.0) ? std::clamp(static_cast<float>(zeroMarker.time / zeroDuration), 0.0f, 1.0f) : 0.0f;
    TEST_ASSERT_EQ(zeroMarker.normalizedPosition, 0.0f);

    // 2. Out of bounds time / negative time handling
    double duration = 100.0;
    double negativeTime = -50.0;
    TimelineChapterMarker negMarker;
    negMarker.time = std::max(0.0, negativeTime);
    negMarker.normalizedPosition = std::clamp(static_cast<float>(negMarker.time / duration), 0.0f, 1.0f);
    TEST_ASSERT_EQ(negMarker.normalizedPosition, 0.0f);

    double overTime = 150.0;
    TimelineChapterMarker overMarker;
    overMarker.time = overTime;
    overMarker.normalizedPosition = std::clamp(static_cast<float>(overMarker.time / duration), 0.0f, 1.0f);
    TEST_ASSERT_EQ(overMarker.normalizedPosition, 1.0f);

    // 3. Overly long title bounding
    std::wstring longTitle(300, L'A');
    std::wstring boundedTitle = longTitle.length() > 80 ? longTitle.substr(0, 77) + L"..." : longTitle;
    TEST_ASSERT_EQ(boundedTitle.length(), 80ull);
    TEST_ASSERT_EQ(boundedTitle.substr(77), std::wstring(L"..."));
}

TEST_CASE(RenderViewModel, EmptyStateViewModelDefaults) {
    EmptyStateViewModel vm;
    TEST_ASSERT(!vm.visible);
    TEST_ASSERT_EQ(vm.type, EmptyStateType::MediaEmptyLibrary);
    TEST_ASSERT_EQ(vm.icon, IconType::MediaFilm);
    TEST_ASSERT(vm.title.empty());
    TEST_ASSERT(vm.body.empty());
    TEST_ASSERT(vm.buttons.empty());
}

TEST_CASE(RenderViewModel, MediaTabEmptyLibrarySnapshot) {
    MediaTabViewModel mediaVm;
    mediaVm.isEmpty = true;
    mediaVm.emptyState.visible = true;
    mediaVm.emptyState.type = EmptyStateType::MediaEmptyLibrary;
    mediaVm.emptyState.icon = IconType::MediaFilm;
    mediaVm.emptyState.title = L"Your Media Library is empty";
    mediaVm.emptyState.body = L"Open video files or add a folder to watch. You can also drag and drop media files anywhere into the window.";

    EmptyStateButtonViewModel openBtn;
    openBtn.text = L"Open File";
    openBtn.icon = IconType::Plus;
    openBtn.isPrimary = true;
    mediaVm.emptyState.buttons.push_back(openBtn);

    EmptyStateButtonViewModel folderBtn;
    folderBtn.text = L"Add Folder";
    folderBtn.icon = IconType::FolderPlaylist;
    folderBtn.isPrimary = false;
    mediaVm.emptyState.buttons.push_back(folderBtn);

    TEST_ASSERT(mediaVm.isEmpty);
    TEST_ASSERT(mediaVm.emptyState.visible);
    TEST_ASSERT_EQ(mediaVm.emptyState.type, EmptyStateType::MediaEmptyLibrary);
    TEST_ASSERT_EQ(mediaVm.emptyState.icon, IconType::MediaFilm);
    TEST_ASSERT_EQ(mediaVm.emptyState.buttons.size(), 2ull);
    TEST_ASSERT(mediaVm.emptyState.buttons[0].isPrimary);
    TEST_ASSERT_EQ(mediaVm.emptyState.buttons[0].text, std::wstring(L"Open File"));
    TEST_ASSERT(!mediaVm.emptyState.buttons[1].isPrimary);
    TEST_ASSERT_EQ(mediaVm.emptyState.buttons[1].text, std::wstring(L"Add Folder"));
}

TEST_CASE(RenderViewModel, MediaTabNoResultsSnapshot) {
    MediaTabViewModel mediaVm;
    mediaVm.isEmpty = true;
    mediaVm.emptyState.visible = true;
    mediaVm.emptyState.type = EmptyStateType::MediaNoResults;
    mediaVm.emptyState.icon = IconType::Search;
    mediaVm.emptyState.title = L"No duplicate media found";
    mediaVm.emptyState.body = L"No duplicate videos were detected in your library. Reset grouping to view all media files.";

    EmptyStateButtonViewModel clearFilterBtn;
    clearFilterBtn.text = L"Clear Filters";
    clearFilterBtn.icon = IconType::Refresh;
    clearFilterBtn.isPrimary = true;
    mediaVm.emptyState.buttons.push_back(clearFilterBtn);

    TEST_ASSERT(mediaVm.isEmpty);
    TEST_ASSERT_EQ(mediaVm.emptyState.type, EmptyStateType::MediaNoResults);
    TEST_ASSERT_EQ(mediaVm.emptyState.icon, IconType::Search);
    TEST_ASSERT_EQ(mediaVm.emptyState.buttons.size(), 1ull);
    TEST_ASSERT(mediaVm.emptyState.buttons[0].isPrimary);
    TEST_ASSERT_EQ(mediaVm.emptyState.buttons[0].text, std::wstring(L"Clear Filters"));
}

TEST_CASE(RenderViewModel, HistoryTabEmptySnapshot) {
    HistoryTabViewModel histVm;
    histVm.isEmpty = true;
    histVm.emptyState.visible = true;
    histVm.emptyState.type = EmptyStateType::HistoryEmpty;
    histVm.emptyState.icon = IconType::HistoryClock;
    histVm.emptyState.title = L"No watch history yet";
    histVm.emptyState.body = L"Media you play will show up here automatically with your saved playback positions and timestamps.";

    EmptyStateButtonViewModel openBtn;
    openBtn.text = L"Open File";
    openBtn.icon = IconType::Plus;
    openBtn.isPrimary = true;
    histVm.emptyState.buttons.push_back(openBtn);

    TEST_ASSERT(histVm.isEmpty);
    TEST_ASSERT_EQ(histVm.emptyState.type, EmptyStateType::HistoryEmpty);
    TEST_ASSERT_EQ(histVm.emptyState.icon, IconType::HistoryClock);
    TEST_ASSERT_EQ(histVm.emptyState.buttons.size(), 1ull);
    TEST_ASSERT(histVm.emptyState.buttons[0].isPrimary);
    TEST_ASSERT_EQ(histVm.emptyState.buttons[0].text, std::wstring(L"Open File"));
}

TEST_CASE(RenderViewModel, JumpAheadViewModelDefaultsAndSemantics) {
    PlayerRenderViewModel vm;
    TEST_ASSERT(!vm.jumpAhead.visible);
    TEST_ASSERT_EQ(vm.jumpAhead.opacity, 0.0f);
    TEST_ASSERT_EQ(vm.jumpAhead.scale, 1.0f);
    TEST_ASSERT(!vm.jumpAhead.isHovered);
    TEST_ASSERT(!vm.jumpAhead.isPressed);
    TEST_ASSERT_EQ(vm.jumpAhead.text, std::wstring(L"Jump ahead"));

    // Verify properties can be mutated for active presentation
    vm.jumpAhead.visible = true;
    vm.jumpAhead.opacity = 0.85f;
    vm.jumpAhead.scale = 0.98f;
    vm.jumpAhead.isPressed = true;
    vm.jumpAhead.isHovered = true;
    vm.jumpAhead.bounds = D2D1::RectF(100.0f, 200.0f, 248.0f, 242.0f);

    TEST_ASSERT(vm.jumpAhead.visible);
    TEST_ASSERT_EQ(vm.jumpAhead.opacity, 0.85f);
    TEST_ASSERT_EQ(vm.jumpAhead.scale, 0.98f);
    TEST_ASSERT(vm.jumpAhead.isPressed);
    TEST_ASSERT(vm.jumpAhead.isHovered);
    TEST_ASSERT_EQ(vm.jumpAhead.bounds.right - vm.jumpAhead.bounds.left, 148.0f);
    TEST_ASSERT_EQ(vm.jumpAhead.bounds.bottom - vm.jumpAhead.bounds.top, 42.0f);
}

TEST_CASE(RenderViewModel, FlyoutBorrowsLargeQueueForTheFrame) {
    SettingsFlyoutViewModel vm;
    TEST_ASSERT(vm.QueueItems().empty());
    std::vector<std::wstring> queue(25000, L"C:\\Media\\episode.mkv");
    vm.pQueueItems = &queue;
    SettingsFlyoutViewModel nextFrame = vm;
    TEST_ASSERT(nextFrame.QueueItems().data() == queue.data());
    TEST_ASSERT_EQ(nextFrame.QueueItems().size(), size_t(25000));
}

TEST_CASE(RenderViewModel, SettingsTabSystemRowDynamics) {
    SettingsTabViewModel vm;
    for (int i = 0; i < SettingsLayout::kSystemRowCount; ++i) {
        TEST_ASSERT_EQ(vm.systemRowHoverProg[i], 0.0f);
        TEST_ASSERT_EQ(vm.systemRowPressProg[i], 0.0f);
    }

    vm.systemRowHoverProg[2] = 1.0f;
    vm.systemRowPressProg[2] = 0.5f;

    SettingsTabViewModel copy = vm;
    TEST_ASSERT_EQ(copy.systemRowHoverProg[2], 1.0f);
    TEST_ASSERT_EQ(copy.systemRowPressProg[2], 0.5f);
    TEST_ASSERT_EQ(copy.systemRowHoverProg[0], 0.0f);
    TEST_ASSERT(!copy.pressState.IsSystemActionPressed(2));
    copy.pressState = { SettingsPressState::Target::SystemAction, 2, -1 };
    TEST_ASSERT(copy.pressState.IsSystemActionPressed(2));
    TEST_ASSERT(!copy.pressState.IsSystemActionPressed(0));
}

} // namespace Aperture::Test
