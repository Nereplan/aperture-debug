#pragma once

#include "Common.h"
#include "Settings.h"
#include "MediaLibrary.h"
#include "MpvPlayer.h"
#include "Renderer.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cstddef>
#include <limits>

namespace Aperture {

// ============================================================================
// Foundational UI Enums & Types
// ============================================================================

enum class TabView { Media, Playlists, History, Settings };

enum class SettingsCategory { Appearance, Playback, Subtitles, Shortcuts, System };

enum class ModalType {
    None, NewPlaylist, NewSmartPlaylist, EditSmartPlaylist, DeletePlaylist, SeekStepInput, FrameSeekInput, CustomSettingInput, Confirmation,
    BookmarkInput, EditMetadataOverrides, SaveSubtitleProfile, RenameSubtitleProfile
};

struct ContextMenuItem {
    std::string id;
    std::wstring label;
    std::wstring shortcut = L"";
    IconType icon = IconType::Play;
    bool isSeparator = false, isChecked = false, isDanger = false;
    std::function<void()> onClick = nullptr;
    bool isDisabled = false, showCheckedIndicator = false;
};

struct SettingsPressState {
    enum class Target { None, Category, OptionRow, SystemAction, ResetShortcuts };
    Target target = Target::None;
    int index = -1, subIndex = -1;
    bool IsCategoryPressed(SettingsCategory cat) const { return target == Target::Category && index == static_cast<int>(cat); }
    bool IsOptionPressed(int row, int sub = -1) const {
        if (target != Target::OptionRow) return false;
        if (index != row) return false;
        return sub == -1 || subIndex == sub;
    }
    bool IsSystemActionPressed(int actionIdx) const { return target == Target::SystemAction && index == actionIdx; }
    bool IsResetShortcutsPressed() const { return target == Target::ResetShortcuts; }
};

// UI-facing snapshot of a TextInputModel. displayText may include temporary
// IME composition text, while selection/caret remain committed UTF-16 indices.
struct TextInputVisualState {
    static constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();
    std::wstring displayText;
    std::size_t selectionStart = 0;
    std::size_t selectionEnd = 0;
    std::size_t caretIndex = 0;
    bool hasCompositionRange = false;
    std::size_t compositionStart = 0;
    std::size_t compositionEnd = 0;
    std::size_t compositionCaretIndex = kNoIndex;
    bool focused = false, caretVisible = false;
};

struct ModalViewModel {
    ModalType type = ModalType::None;
    std::wstring title, prompt, message, confirmText;
    bool isDestructive = true;
    std::wstring textValue;
    TextInputVisualState visualState;
    bool isOkHovered = false, isCancelHovered = false, isOkPressed = false, isCancelPressed = false;
};

// ============================================================================
// Shared & Overlay Sub-ViewModels
// ============================================================================

struct SubtitleOverlayViewModel {
    bool visible = false;
    std::wstring text;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    std::string bgStyle = "box";
    D2D1_COLOR_F textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    float fontSizePoints = 18.0f;
    std::string position = "bottom";
};

struct DoubleTapFeedbackViewModel {
    bool visible = false, isForward = true, isPiP = false;
    double totalSeconds = 0.0;   // accumulated seek amount, e.g. 45 -> "+ 45"
    std::vector<float> chevronSlides; // unfinished slide-in tracks, one per press
    float signPop = 1.0f;        // 0 -> 1 sign pop progress (0.24s)
    float numberPop = 1.0f;      // 0 -> 1 number pop progress (0.22s)
    float appear = 1.0f;         // 0 -> 1 badge entrance on first press
    float fadeOut = 0.0f;        // 0..1 exit progress (fade + drift + scale)
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    // Snapshot of the opposite side while it fast-fades on direction switch.
    bool fadeSideVisible = false, fadeSideForward = true;
    double fadeSideSeconds = 0.0;
    std::vector<float> fadeSideChevronSlides;
    float fadeSideSignPop = 1.0f, fadeSideNumberPop = 1.0f, fadeSideAppear = 1.0f, fadeSideFadeOut = 1.0f;
};

struct PlayPauseBadgeViewModel {
    bool visible = false, isPlay = true;
    float animProgress = 0.0f;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
};

struct TimelineChapterMarker { double time = 0.0; float normalizedPosition = 0.0f; std::wstring title; bool isPassed = false; };

struct PlayerTopBarViewModel {
    bool visible = true;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    std::wstring title, chapterTitle;
    bool isBackHovered = false;
    LONG focusedControlId = 0;
    bool showKeyboardFocus = false;
    float opacity = 1.0f;
};

struct PlayerBottomBarViewModel {
    bool visible = true;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    bool isPlaying = false;
    double currentTime = 0.0, duration = 0.0, cacheDuration = 0.0;
    bool isScrubbing = false;
    double volume = 100.0;
    bool isMuted = false, isVolumeSliderHovered = false, isTimelineHovered = false;
    float timelineHoverX = -1.0f; // overlay-space cursor X while hovering the timeline
    float timelineHoverProgress = 0.0f; // 0.0 at rest, animates to 1.0 on hover/scrub
    bool isSubtitlesActive = false, isFullscreen = false, isPiP = false, isQueuePlayback = false;
    bool isFirstQueueButtonDisabled = false;
    int hoveredButtonId = 0; // 1:Play, 2:Next, 3:Mute, 4:CC, 5:Settings, 6:PiP, 7:Full, 8:Prev
    LONG focusedControlId = 0;
    bool showKeyboardFocus = false;
    float opacity = 1.0f;
    // Shared snapshot: the passed/not-passed set only changes when playback
    // crosses a chapter boundary, so the vector is cached by the app and
    // shared into each per-frame view model instead of rebuilt per frame.
    std::shared_ptr<const std::vector<TimelineChapterMarker>> chapterMarkers;
    std::wstring currentChapterTitle;
};

struct PiPOverlayViewModel {
    bool visible = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    std::wstring title;
    bool isPlaying = false;
    double currentTime = 0.0, duration = 0.0;
    bool isScrubbing = false, canPlayPrevious = false, canPlayNext = false, isPinned = false;
    int hoveredButtonId = 0; // 1: Play/Pause, 2: Restore, 3: Close, 4: Previous, 5: Next, 6: Audio, 7: Pin
    LONG focusedControlId = 0;
    bool showKeyboardFocus = false;
    float opacity = 1.0f;
    bool isVolumeSliderOpen = false;
    float volume = 1.0f;
    bool isMuted = false;
    D2D1_RECT_F volumeSliderBounds = { 0, 0, 0, 0 };
    float volumeSliderAnim = 0.0f;
};

struct SettingsFlyoutViewModel {
    bool isOpen = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    std::string currentMenu = "main";
    int hoveredOptionIndex = -1;
    std::vector<MpvTrackInfo> subTracks;
    int activeSubIndex = -1;
    std::vector<MpvTrackInfo> audioTracks;
    int activeAudioIndex = -1;
    double currentSpeed = 1.0;
    std::string currentAspect = "auto";
    bool isLoop = false, isAutoplay = false, isPrivateMode = false;
    // Borrowed for synchronous UI-thread rendering, like visible media rows.
    // Opening a flyout must not copy a potentially library-sized queue.
    const std::vector<std::wstring>* pQueueItems = nullptr;
    const std::vector<std::wstring>& QueueItems() const {
        static const std::vector<std::wstring> empty;
        return pQueueItems ? *pQueueItems : empty;
    }
    size_t queueIndex = 0, queueFirstVisible = 0;
    float animProgress = 1.0f;
};

struct OsdBadgeViewModel {
    bool visible = false, isPiP = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    IconType icon = IconType::VolumeHigh;
    std::wstring text;
    float opacity = 1.0f;
};

struct DiagnosticsOverlayViewModel {
    bool visible = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    PlayerDiagnostics diagnostics;
    int audioDelayMs = 0, subDelayMs = 0;
};

struct ContextMenuViewModel {
    bool isOpen = false;
    D2D1_POINT_2F origin = { 0.0f, 0.0f };
    std::vector<ContextMenuItem> items;
    int hoveredIndex = -1;
    float animProgress = 1.0f;
};

struct JumpAheadViewModel {
    bool visible = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    float opacity = 0.0f;
    float scale = 1.0f;
    bool isHovered = false;
    bool isPressed = false;
    std::wstring text = L"Jump ahead";
};

// ============================================================================
// Complete Player Overlay Render View-Model Snapshot
// ============================================================================

struct PlayerRenderViewModel {
    D2D1_RECT_F winBounds = { 0, 0, 0, 0 };
    float dpiScale = 1.0f;
    bool isPiP = false;
    SubtitleOverlayViewModel subtitle;
    DoubleTapFeedbackViewModel doubleTap;
    PlayPauseBadgeViewModel playPauseBadge;
    PlayerTopBarViewModel topBar;
    PlayerBottomBarViewModel bottomBar;
    PiPOverlayViewModel pipOverlay;
    SettingsFlyoutViewModel settingsFlyout;
    OsdBadgeViewModel osdBadge;
    JumpAheadViewModel jumpAhead;
    DiagnosticsOverlayViewModel diagnosticsOverlay;
    ContextMenuViewModel contextMenu;
    ModalViewModel modal;
};

// ============================================================================
// Hub Sub-ViewModels
// ============================================================================

struct NavBarViewModel {
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    TabView activeTab = TabView::Media;
    TabView hoveredTab = (TabView)-1;
    bool isSettingsHovered = false, isOpenHovered = false;
    float settingsRotationDegrees = 0.0f;
    bool isPlaylistsNavDropHovered = false;
    int focusedNavIndex = -1;
    // Animated hover mixes (0..1) for smooth hover transitions; pressedNavIndex
    // mirrors App::pressedNavIndex (0..2 tabs, 3 settings, 4 open file).
    float mediaHoverMix = 0.0f, playlistHoverMix = 0.0f, historyHoverMix = 0.0f, settingsHoverMix = 0.0f;
    float openHoverMix = 0.0f;
    int pressedNavIndex = -1;
};

struct VisibleMediaGroupHeaderViewModel {
    std::wstring key, title, subtitle, countText;
    D2D1_RECT_F headerRect = { 0, 0, 0, 0 };
    bool isSpecial = false, isSeries = false, isCollapsed = false, isHovered = false;
    bool isSelected = false, isDeleteHovered = false;
    D2D1_RECT_F deleteRect = { 0, 0, 0, 0 };
    D2D1_RECT_F dragHandleRect = { 0, 0, 0, 0 };
};

struct VisibleMediaCardViewModel {
    size_t originalIndex = 0;
    // Borrowed for one frame; points into MediaLibrary::mediaItems. The card
    // cannot mutate the library item, so per-card overrides (a file that no
    // longer exists on disk) live beside the pointer.
    const MediaItem* pItem = nullptr;
    std::string effectiveStatus;
    D2D1_RECT_F itemRect = { 0, 0, 0, 0 };
    bool isHovered = false, isSelected = false, isPinned = false;
    float previewOpacity = 0.0f;
    ID2D1Bitmap* pLivePreviewBitmap = nullptr;
    bool isCloseHovered = false;
    float liftY = 0.0f;
    bool isPlaceholder = false;
};

enum class EmptyStateType { MediaEmptyLibrary, MediaNoResults, HistoryEmpty };

struct EmptyStateButtonViewModel {
    std::wstring text;
    IconType icon = IconType::Plus;
    bool isPrimary = false, isHovered = false, isPressed = false, isFocused = false;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
};

struct EmptyStateViewModel {
    bool visible = false;
    EmptyStateType type = EmptyStateType::MediaEmptyLibrary;
    IconType icon = IconType::MediaFilm;
    std::wstring title, body;
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    D2D1_RECT_F cardBounds = { 0, 0, 0, 0 };
    std::vector<EmptyStateButtonViewModel> buttons;
};

struct MediaTabViewModel {
    bool isEmpty = false;
    D2D1_RECT_F clipBounds = { 0, 0, 0, 0 };
    EmptyStateViewModel emptyState;
    std::vector<VisibleMediaCardViewModel> visibleCards;
    // Grouping toolbar control
    D2D1_RECT_F toolbarBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F searchBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F sortBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F groupControlBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F flatBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F folderBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F seriesBtnBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F duplicatesBtnBounds = { 0, 0, 0, 0 };
    int activeGroupMode = 0; // 0: Flat, 1: Folders, 2: Series, 3: Duplicates
    int hoveredGroupBtn = -1; // -1: None, 0: Flat, 1: Folders, 2: Series, 3: Duplicates
    bool searchHovered = false, searchFocused = false, searchClearHovered = false, hasSearchQuery = false;
    D2D1_RECT_F searchClearBounds = { 0, 0, 0, 0 };
    std::wstring filterQuery;
    TextInputVisualState searchVisual;
    bool sortHovered = false, compactToolbar = false, showSearchLabel = true, showSummary = true;
    std::wstring sortLabel, summaryText;
    D2D1_RECT_F summaryTextBounds = { 0, 0, 0, 0 };
    // Visible group headers
    std::vector<VisibleMediaGroupHeaderViewModel> visibleGroupHeaders;
    bool isMediaDrag = false;
    int dragFromIndex = -1;
    D2D1_RECT_F dragCloneRect = { 0, 0, 0, 0 };
    const MediaItem* pDragCloneItem = nullptr;
    bool hasDragClone = false;
    bool isFolderDrag = false;
    int dragFromFolderIndex = -1, dragToFolderIndex = -1;
    float folderDragOffsetY = 0.0f;
    VisibleMediaGroupHeaderViewModel dragCloneFolder;
};

struct PlaylistRailItemViewModel {
    std::string id, name;
    bool isSmart = false;
    int totalVids = 0, watchedCount = 0;
    float progressRatio = 0.0f;
    size_t rawItemCount = 0;
    bool isActive = false, isFocused = false, isHovered = false;
    D2D1_RECT_F itemRect = { 0, 0, 0, 0 };
    D2D1_RECT_F titleRect = { 0, 0, 0, 0 };
    D2D1_RECT_F subRect = { 0, 0, 0, 0 };
    float ringCX = 0.0f, ringCY = 0.0f, ringRadius = 0.0f;
    bool isInlineRenaming = false;
};

struct PlaylistsRailViewModel {
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    std::string activePlaylistId;
    int hoveredIndex = -1;
    bool isAddHovered = false;
    float scrollY = 0.0f;
    int dragFromIndex = -1, dragToIndex = -1;
    float dragOffsetY = 0.0f;
    bool isRailFocused = false;
    int focusedItemIndex = -1;
    bool isInlineRenaming = false;
    std::string inlineRenamePlaylistId;
    TextInputVisualState inlineRenameVisual;
    std::vector<PlaylistRailItemViewModel> items;
    // OLE folder payload hovering the rail: previews "drop to create playlist".
    bool isFolderDropActive = false;
    bool hasDragClone = false;
    D2D1_RECT_F dragCloneRect = { 0, 0, 0, 0 };
    std::string dragCloneName;
    size_t dragCloneItemCount = 0;
};

struct PlaylistResumeCardViewModel {
    bool hasResumeCard = false;
    D2D1_RECT_F cardRect = { 0, 0, 0, 0 };
    D2D1_RECT_F thumbRect = { 0, 0, 0, 0 };
    D2D1_RECT_F tagRect = { 0, 0, 0, 0 };
    D2D1_RECT_F titleRect = { 0, 0, 0, 0 };
    D2D1_RECT_F timeRect = { 0, 0, 0, 0 };
    std::wstring filePath, title, tagText, timeText;
    double progressRatio = 0.0;
    bool isResumeFocused = false, isResumeHovered = false;
    // Live hover preview (mirrors the media-row expanded preview).
    ID2D1Bitmap* pLivePreviewBitmap = nullptr;
    float previewOpacity = 0.0f;
};

struct VisiblePlaylistEpisodeViewModel {
    size_t originalIndex = 0;
    std::wstring filePath, title, subtitle;
    double durationSec = 0.0, lastPosSec = 0.0, progressRatio = 0.0;
    bool isHovered = false, isDeleteHovered = false, isSelected = false;
    D2D1_RECT_F itemRect = { 0, 0, 0, 0 };
    bool isPlaceholder = false;
    float titleExpandProgress = 0.0f;
};

struct PlaylistDetailViewModel {
    bool hasPlaylist = false;
    std::string playlistId, playlistName;
    std::wstring metaStr;
    bool isSmart = false, isEditRulesHovered = false, isAddFilesHovered = false, isPlayAllHovered = false;
    bool isDeletePlHovered = false, isResumeHovered = false, isDropTargetActive = false, isEmpty = false;
    // Drop target active while the OLE payload carries a folder; the banner
    // then advertises "add folder's videos" instead of "drop files".
    bool isFolderDrop = false;
    PlaylistResumeCardViewModel resumeCard;
    std::wstring filterQuery;
    bool gridView = false, searchFocused = false;
    TextInputVisualState searchVisual;
    size_t visibleCount = 0;
    D2D1_RECT_F listClipBounds = { 0, 0, 0, 0 };
    float scrollY = 0.0f;
    std::vector<VisiblePlaylistEpisodeViewModel> visibleEpisodes;
    D2D1_RECT_F dropSlotRect = {};
    bool hasDragClone = false;
    D2D1_RECT_F dragCloneRect = { 0, 0, 0, 0 };
    VisiblePlaylistEpisodeViewModel dragCloneEpisode;
};

struct PlaylistsTabViewModel { PlaylistsRailViewModel rail; PlaylistDetailViewModel detail; };

enum class HistoryTimeGroup { Today = 0, Yesterday, ThisWeek, Older };

inline const wchar_t* GetHistoryTimeGroupTitle(HistoryTimeGroup group) {
    switch (group) {
        case HistoryTimeGroup::Today: return L"Today";
        case HistoryTimeGroup::Yesterday: return L"Yesterday";
        case HistoryTimeGroup::ThisWeek: return L"This week";
        case HistoryTimeGroup::Older: return L"Older";
    }
    return L"Older";
}

struct HistoryGroupSummary {
    HistoryTimeGroup group = HistoryTimeGroup::Older;
    std::wstring title, countText;
    size_t startIndex = 0, count = 0;
    D2D1_RECT_F headerRect = { 0, 0, 0, 0 };
};

struct VisibleHistoryItemViewModel {
    size_t originalIndex = 0;
    // Borrowed for one frame: points into MediaLibrary::history, which is only
    // mutated on the UI thread between frames. Avoids copying the record's
    // strings and extraFields for every row on every render.
    const HistoryRecord* pRecord = nullptr;
    D2D1_RECT_F itemRect = { 0, 0, 0, 0 };
    bool isHovered = false, isDeleteHovered = false, isSelected = false, isKeyboardFocused = false;
    std::wstring formattedTime, formattedDuration, displayTitle;
};

inline std::wstring FormatHistoryProgress(const HistoryRecord& record) {
    double pos = std::isfinite(record.lastPosSec) ? std::max(0.0, record.lastPosSec) : 0.0;
    const double duration = std::isfinite(record.durationSec) ? std::max(0.0, record.durationSec) : 0.0;
    if (duration > 0.0) {
        pos = std::min(pos, duration);
        if (pos >= duration * 0.95) { return L"Completed \x00B7 " + FormatDuration(duration); }
        return L"Progress " + FormatDuration(pos) + L" / " + FormatDuration(duration);
    }
    if (pos > 0.0) { return L"Progress " + FormatDuration(pos); }
    return L"Opened";
}

struct HistoryTimelineProjectionOptions { int64_t nowTimestamp = 0; };

struct HistoryTimelineProjection {
    std::vector<HistoryGroupSummary> groups;
    std::vector<size_t> itemIndices;
    std::vector<HistoryRecord> items;
    size_t totalCount = 0;
    bool IsEmpty() const { return items.empty(); }
    size_t GroupCount() const { return groups.size(); }
};

inline HistoryTimelineProjection ProjectHistoryTimeline( const std::vector<HistoryRecord>& rawHistory,
    const HistoryTimelineProjectionOptions& options = {} ) {
    HistoryTimelineProjection proj;
    if (rawHistory.empty()) return proj;
    int64_t nowSec = options.nowTimestamp > 0 ? options.nowTimestamp : GetCurrentTimestampSec();
    std::time_t ttNow = (std::time_t)nowSec;
    std::tm tmNow{};
    localtime_s(&tmNow, &ttNow);
    std::tm tmToday = tmNow;
    tmToday.tm_hour = 0;
    tmToday.tm_min = 0;
    tmToday.tm_sec = 0;
    int64_t todayStart = (int64_t)mktime(&tmToday);
    std::tm tmYesterday = tmToday;
    tmYesterday.tm_mday -= 1;
    int64_t yesterdayStart = (int64_t)mktime(&tmYesterday);
    std::tm tmThisWeek = tmToday;
    tmThisWeek.tm_mday -= 6;
    int64_t thisWeekStart = (int64_t)mktime(&tmThisWeek);
    std::vector<size_t> todayIndices;
    std::vector<size_t> yesterdayIndices;
    std::vector<size_t> thisWeekIndices;
    std::vector<size_t> olderIndices;
    for (size_t i = 0; i < rawHistory.size(); ++i) {
        int64_t ts = rawHistory[i].watchedTimestamp;
        if (ts <= 0) { olderIndices.push_back(i); } else if (ts >= todayStart) {
            todayIndices.push_back(i);
        } else if (ts >= yesterdayStart) { yesterdayIndices.push_back(i); } else if (ts >= thisWeekStart) {
            thisWeekIndices.push_back(i);
        } else { olderIndices.push_back(i); }
    }
    auto stableRecencyComp = [&](size_t idxA, size_t idxB) {
        const auto& a = rawHistory[idxA];
        const auto& b = rawHistory[idxB];
        if (a.watchedTimestamp != b.watchedTimestamp) { return a.watchedTimestamp > b.watchedTimestamp; }
        return idxA < idxB;
    };
    std::stable_sort(todayIndices.begin(), todayIndices.end(), stableRecencyComp);
    std::stable_sort(yesterdayIndices.begin(), yesterdayIndices.end(), stableRecencyComp);
    std::stable_sort(thisWeekIndices.begin(), thisWeekIndices.end(), stableRecencyComp);
    std::stable_sort(olderIndices.begin(), olderIndices.end(), stableRecencyComp);
    auto addGroup = [&](HistoryTimeGroup grpType, const std::vector<size_t>& indices) {
        if (indices.empty()) return;
        HistoryGroupSummary summary;
        summary.group = grpType;
        summary.title = GetHistoryTimeGroupTitle(grpType);
        summary.count = indices.size();
        summary.countText = std::to_wstring(indices.size()) + (indices.size() == 1 ? L" item" : L" items");
        summary.startIndex = proj.itemIndices.size();
        proj.groups.push_back(summary);
        for (size_t idx : indices) { proj.itemIndices.push_back(idx); proj.items.push_back(rawHistory[idx]); }
    };
    addGroup(HistoryTimeGroup::Today, todayIndices);
    addGroup(HistoryTimeGroup::Yesterday, yesterdayIndices);
    addGroup(HistoryTimeGroup::ThisWeek, thisWeekIndices);
    addGroup(HistoryTimeGroup::Older, olderIndices);
    proj.totalCount = proj.itemIndices.size();
    return proj;
}

struct HistoryTabViewModel {
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    bool isEmpty = false;
    EmptyStateViewModel emptyState;
    // Borrowed for one frame; see VisibleHistoryItemViewModel::pRecord.
    const std::vector<HistoryRecord>* pHistory = nullptr;
    std::vector<HistoryGroupSummary> groups;
    std::vector<VisibleHistoryItemViewModel> visibleItems;
    int hoveredIndex = -1, hoveredDeleteIndex = -1;
    bool isClearHovered = false, isClearPressed = false;
    float scrollY = 0.0f;
    std::unordered_set<size_t> selectedIndices;
};

struct SettingsTabViewModel {
    D2D1_RECT_F bounds = { 0, 0, 0, 0 };
    SettingsCategory activeCat = SettingsCategory::Appearance;
    SettingsCategory hoveredCat = (SettingsCategory)-1;
    int hoveredRowIndex = -1;
    float scrollY = 0.0f;
    std::string rebindingActionId;
    int hoveredShortcutIndex = -1;
    bool isResetShortcutsHovered = false, isSidebarFocused = false, isContentFocused = false;
    int keyboardFocusedRow = 0, keyboardFocusedSubIndex = 0;
    float autoplayProg = 1.0f, rememberPosProg = 1.0f, hwAccelProg = 1.0f, loopProg = 0.0f;
    float customMpvConfigProg = 0.0f;
    bool isKeyboardNavActive = false;
    SettingsPressState pressState;
    float mouseX = -1.0f, mouseY = -1.0f;
};

struct AutoscrollViewModel {
    bool isActive = false;
    D2D1_POINT_2F anchorPoint = { 0.0f, 0.0f };
    float dy = 0.0f;
};

struct ImportProgressViewModel {
    bool isActive = false;
    size_t processed = 0, total = 0;
    double progressRatio = 0.0;
    std::wstring progressText;
    bool isCancelHovered = false;
    D2D1_RECT_F cardBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F cancelBounds = { 0, 0, 0, 0 };
};
struct GlobalSearchItemViewModel {
    std::wstring filePath, title, fileName, durationText;
    std::wstring sourceBadge;
    double progressRatio = 0.0;
    bool isSelected = false, isHovered = false, existsOnDisk = true;
    D2D1_RECT_F itemRect = { 0, 0, 0, 0 };
};

struct GlobalSearchSurfaceViewModel {
    bool isOpen = false, isFocused = false, isCloseHovered = false;
    std::wstring query;
    TextInputVisualState searchVisual;
    D2D1_RECT_F overlayBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F searchBoxBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F inputBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F closeBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F listClipBounds = { 0, 0, 0, 0 };
    int selectedIndex = -1, hoveredIndex = -1;
    size_t totalMatches = 0;
    std::vector<GlobalSearchItemViewModel> visibleItems;
};

// ============================================================================
// Complete Hub Render View-Model Snapshot
// ============================================================================

struct HubRenderViewModel {
    D2D1_RECT_F winBounds = { 0, 0, 0, 0 };
    D2D1_RECT_F viewBounds = { 0, 0, 0, 0 };
    float dpiScale = 1.0f;
    TabView currentTab = TabView::Media;
    NavBarViewModel navBar;
    MediaTabViewModel mediaTab;
    PlaylistsTabViewModel playlistsTab;
    HistoryTabViewModel historyTab;
    SettingsTabViewModel settingsTab;
    AutoscrollViewModel autoscroll;
    ImportProgressViewModel importProgress;
    OsdBadgeViewModel osdBadge;
    ContextMenuViewModel contextMenu;
    ModalViewModel modal;
    GlobalSearchSurfaceViewModel globalSearch;
};

} // namespace Aperture
