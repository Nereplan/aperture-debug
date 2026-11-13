#include "App.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "Localization.h"
#include "Presentation.h"
#include <array>

namespace Aperture {

namespace {

void MixShortcutsSignature(unsigned long long& signature, const std::map<std::string, ShortcutBinding>& shortcuts) {
    for (const auto& pair : shortcuts) {
        MixTooltipSignature(signature, Fnv1a64(pair.first.data(), pair.first.size()));
        const ShortcutBinding& b = pair.second;
        MixTooltipSignature(signature,
            static_cast<unsigned long long>(b.vkCode) | (static_cast<unsigned long long>(b.ctrl) << 32) |
            (static_cast<unsigned long long>(b.alt) << 40) | (static_cast<unsigned long long>(b.shift) << 48));
    }
}

void PruneInactiveToolIds(TooltipManager& mgr, const std::unordered_set<TooltipManager::ToolId>& activeIds) {
    for (TooltipManager::ToolId id : mgr.GetToolIds()) {
        if (activeIds.find(id) == activeIds.end()) mgr.Remove(id);
    }
}

struct NavWidthCache {
    const Renderer* rendererPtr = nullptr;
    int dpQ = 0;
    bool isRtl = false;
    std::string fontFamily;
    float textScale = 1.0f;
    std::string locale;
    std::array<std::pair<std::wstring, float>, 8> entries{};
    size_t count = 0;

    float Measure(Renderer& renderer, const std::string& fontFam, bool rtl, int curDpQ, const std::wstring& text) {
        const float curTextScale = renderer.GetTextScale();
        const std::string& curLocale = Localization::Instance().GetCurrentLocale();
        if (rendererPtr != &renderer || dpQ != curDpQ || isRtl != rtl || fontFamily != fontFam ||
            std::abs(textScale - curTextScale) > 0.001f || locale != curLocale) {
            rendererPtr = &renderer;
            dpQ = curDpQ;
            isRtl = rtl;
            fontFamily = fontFam;
            textScale = curTextScale;
            locale = curLocale;
            count = 0;
        }
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].first == text) return entries[i].second;
        }
        const float w = renderer.MeasureString(text, FontType::Medium).width;
        if (count >= entries.size()) count = 0;
        entries[count++] = { text, w };
        return w;
    }
};

} // namespace


void App::RefreshHubTooltips(bool force) {
    if (!hWnd) return;
    if (!hubTooltips.IsCreated() && !EnsureHubTooltips()) return;
    if (playbackSession.IsOpen()) {
        hubTooltips.CancelDeferredRefresh();
        if (hubTooltips.IsActive()) hubTooltips.SetActive(false);
        hubTooltips.HideForKeyboard();
        return;
    }
    const bool scrollAnimating = IsAutoscrolling() ||
        (touchPanPane != AutoscrollPane::None &&
         (touchHub.phase() == Touch::Phase::Panning || touchPanVelocity != 0.0f || touchSpringBack));
    if (!force && hubTooltipSignature != 0 && scrollAnimating) return;
    RECT client{};
    if (!GetClientRect(hWnd, &client)) return;
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    const float winW = static_cast<float>(std::max<LONG>(1, client.right - client.left));
    const float winH = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
    unsigned long long signature = 0x6a09e667f3bcc909ull;
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(winW)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(winH)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(dp * 1000.0f)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(currentTab));
    MixTooltipSignature(signature, static_cast<unsigned long long>(activeFocusPane));
    MixTooltipSignature(signature, static_cast<unsigned long long>(topNavFocusedIndex + 2));
    MixTooltipSignature(signature, static_cast<unsigned long long>(activeModal));
    MixTooltipSignature(signature, isContextMenuOpen ? 1ull : 0ull);
    MixTooltipSignature(signature, library.GetVersion());
    MixTooltipSignature(signature, static_cast<unsigned long long>(currentSettingsCat));
    MixTooltipSignature(signature, static_cast<unsigned long long>(settingsDescTools.size()));
    for (const SettingsDescTool& tool : settingsDescTools) {
        MixTooltipSignature(signature, Fnv1a64(tool.text.c_str(), tool.text.size() * sizeof(wchar_t)));
        MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(tool.rect.left)) |
            (static_cast<unsigned long long>(std::lround(tool.rect.top)) << 32));
        MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(tool.rect.right)) |
            (static_cast<unsigned long long>(std::lround(tool.rect.bottom)) << 32));
    }
    // Mix the raw binding fields: formatting a wstring per binding per frame
    // allocated ~2n strings on every render just to keep the signature equal.
    MixShortcutsSignature(signature, settings.shortcuts);
    const unsigned long long nonScrollSignature = signature;
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(mediaScrollY)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(historyScrollY)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(settingsScrollY)));
    const bool visible = activeModal == ModalType::None && !isContextMenuOpen;
    if (!force && signature == hubTooltipSignature) {
        if (hubTooltips.IsActive() != visible) hubTooltips.SetActive(visible);
        if (!visible) hubTooltips.HideForKeyboard();
        return;
    }
    struct HubTooltipScrollCache {
        const App* owner = nullptr;
        unsigned long long nonScrollSig = 0;
    };
    static HubTooltipScrollCache s_scrollCache;
    if (!force && visible && hubTooltipSignature != 0 && s_scrollCache.owner == this &&
        nonScrollSignature == s_scrollCache.nonScrollSig) {
        // Only scroll offsets changed: coalesce per-row measurements and Win32
        // tooltip IPC until scrolling settles (~80 ms).
        hubTooltips.ScheduleDeferredRefresh(80, [this]() {
            if (hWnd && IsWindow(hWnd)) RefreshHubTooltips(true);
        });
        return;
    }
    hubTooltips.CancelDeferredRefresh();
    s_scrollCache = { this, nonScrollSignature };
    hubTooltipSignature = signature;
    hubTooltips.SetActive(visible);
    if (!visible) { hubTooltips.HideForKeyboard(); return; }
    std::unordered_set<TooltipManager::ToolId> activeIds;
    const auto add = [&](TooltipManager::ToolId id, const std::wstring& text, const RECT& rect) {
        activeIds.insert(id);
        hubTooltips.AddOrUpdate(id, text, rect);
    };
    const std::wstring tabNames[] = { L"Media", L"Playlists", L"History" };
    const bool isRtl = Localization::Instance().IsRtl();
    static NavWidthCache s_navWidthCache;
    const int dpQ = static_cast<int>(std::lround(dp * 1000.0f));
    const auto nav = Presentation::Nav(winW, dp, isRtl, [&](const std::wstring& t) {
        return s_navWidthCache.Measure(renderer, settings.uiFontFamily, isRtl, dpQ, t);
    });
    for (int i = 0; i < 3; ++i)
        add(1001 + static_cast<TooltipManager::ToolId>(i), tabNames[i], TooltipClientRect(nav.tabs[i].left, nav.tabY, nav.tabs[i].right, nav.tabY + nav.tabH));
    add(1004, Loc::Tr("nav.settings"), TooltipClientRect(nav.settings.left, nav.tabY, nav.settings.right, nav.tabY + nav.tabH));
    add(1005, TooltipActionLabel(settings, Loc::Tr("nav.open_file"), "openFile"),
        TooltipClientRect(nav.open.left, nav.tabY, nav.open.right, nav.tabY + nav.tabH));
    const D2D1_RECT_F view = Presentation::HubView(winW, winH, dp);
    if (currentTab == TabView::Media) {
        const auto toolbar = ComputeMediaToolbarGeometry(D2D1::RectF(view.left, view.top, view.right, view.top + 32.0f * dp), dp);
        add(1048, L"Filter media by title, filename, or series (Ctrl+F)", TooltipClientRect(toolbar.search.left, toolbar.search.top, toolbar.search.right, toolbar.search.bottom));
        add(1049, L"Sort: " + GetLibrarySortLabel(), TooltipClientRect(toolbar.sort.left, toolbar.sort.top, toolbar.sort.right, toolbar.sort.bottom));
        EnsureMediaLayoutCache();
        const auto& visibleIndices = GetVisibleMediaIndices();
        const float listTop = view.top + 44.0f * dp;
        const auto range = GetVisibleMediaRowRange(mediaScrollY / dp, RectHeight(view) / dp, visibleIndices.size(), 1);
        for (size_t visiblePos = range.first; visiblePos < range.second; ++visiblePos) {
            if (visiblePos >= visibleIndices.size()) continue;
            const size_t libraryIndex = visibleIndices[visiblePos];
            if (libraryIndex >= library.mediaItems.size()) continue;
            const MediaItem& item = library.mediaItems[libraryIndex];
            const std::wstring title = item.GetDisplayTitle();
            const float y = listTop + GetMediaRowTopDIP(visiblePos) * dp - mediaScrollY;
            const float height = GetMediaRowHeightDIP(visiblePos) * dp;
            const D2D1_RECT_F row = D2D1::RectF(view.left + dp, y, view.right - 2.0f * dp, y + height);
            const float titleWidth = std::max(80.0f * dp, RectWidth(row) - 250.0f * dp);
            if (renderer.MeasureString(title, FontType::Title).width > titleWidth) {
                add(20000 + static_cast<TooltipManager::ToolId>(visiblePos), title,
                    TooltipClientRect(row.left, row.top, row.right, row.bottom));
            }
        }
    } else if (currentTab == TabView::History) {
        const HistoryTimelineProjection& projection = GetHistoryProjection();
        float headerY = view.top + kHistoryContentInset * dp - historyScrollY;
        for (size_t groupIndex = 0; groupIndex < projection.groups.size(); ++groupIndex) {
            const auto& group = projection.groups[groupIndex];
            const HistoryGroupLayout layout(headerY, group.count, dp);
            headerY = layout.NextHeaderTop();
            const auto [firstRow, lastRow] = layout.VisibleRows(view.top, view.bottom);
            for (size_t rowIndex = firstRow; rowIndex < lastRow; ++rowIndex) {
                const size_t i = group.startIndex + rowIndex;
                if (i >= projection.itemIndices.size()) break;
                const size_t historyIndex = projection.itemIndices[i];
                if (historyIndex >= library.history.size()) continue;
                const HistoryRecord& record = library.history[historyIndex];
                const std::wstring title = record.title.empty() ? GetFileNameFromPath(record.filePath) : record.title;
                const float y = layout.RowTop(rowIndex);
                const D2D1_RECT_F row = D2D1::RectF(view.left + 12.0f * dp, y, view.right - 24.0f * dp, y + layout.rowHeight);
                const float titleWidth = std::max(80.0f * dp, RectWidth(row) - 150.0f * dp);
                if (row.bottom >= view.top && row.top <= view.bottom &&
                    renderer.MeasureString(title, FontType::Medium).width > titleWidth) {
                    add(30000 + static_cast<TooltipManager::ToolId>(historyIndex), title,
                        TooltipClientRect(row.left, row.top, row.right, row.bottom));
                }
            }
        }
    }
    if (currentTab == TabView::Settings) {
        // Compact layout hides row descriptions; hovering re-surfaces them.
        for (size_t i = 0; i < settingsDescTools.size(); ++i) {
            const SettingsDescTool& tool = settingsDescTools[i];
            add(40000 + static_cast<TooltipManager::ToolId>(i), tool.text,
                TooltipClientRect(tool.rect.left, tool.rect.top, tool.rect.right, tool.rect.bottom));
        }
    }
    if (currentTab == TabView::Playlists) {
        const float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(view) * 0.42f));
        const D2D1_RECT_F detail = D2D1::RectF(view.left + railW + 16.0f * dp, view.top, view.right, view.bottom);
        add(1101, L"New Playlist", TooltipClientRect(view.left + railW - 40.0f * dp, view.top + 7.0f * dp, view.left + railW - 10.0f * dp, view.top + 42.0f * dp));
        Playlist* playlist = library.FindPlaylist(activePlaylistId);
        if (playlist && !playlist->itemPaths.empty()) {
            const float toolbarY = detail.top + 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp;
            add(1102, L"Play All", TooltipClientRect(detail.left + 16.0f * dp, detail.top + 116.0f * dp, detail.left + 116.0f * dp, detail.top + 152.0f * dp));
            add(1103, L"Resume Playlist", TooltipClientRect(detail.left + 124.0f * dp, detail.top + 116.0f * dp, detail.left + 244.0f * dp, detail.top + 152.0f * dp));
            add(1104, L"Filter Episodes", TooltipClientRect(detail.left, toolbarY, detail.left + 230.0f * dp, toolbarY + 32.0f * dp));
            add(1105, playlistGridView ? L"Switch to List View" : L"Switch to Grid View",
                TooltipClientRect(std::max(detail.left, detail.right - 90.0f * dp), toolbarY, detail.right - 26.0f * dp, toolbarY + 32.0f * dp));
        } else if (playlist && !playlist->IsSmart()) {
            const float paneRight = std::max(detail.left + 120.0f * dp, detail.right - 26.0f * dp);
            const float dropTop = detail.top + 14.0f * dp + 50.0f * dp + 16.0f * dp;
            const float centerX = (detail.left + paneRight) * 0.5f;
            const float addFilesW = 118.0f * dp, addFilesH = 34.0f * dp;
            add(1106, L"Add files to this playlist", TooltipClientRect( centerX - addFilesW * 0.5f, dropTop + 168.0f * dp,
                centerX + addFilesW * 0.5f, dropTop + 168.0f * dp + addFilesH));
        }
    } else if (currentTab == TabView::History) {
        D2D1_SIZE_F clearSize = renderer.MeasureString(L"Clear History", FontType::Medium);
        float clearW = clearSize.width + 24.0f * dp, clearH = 32.0f * dp;
        float clearTop = view.top + (46.0f * dp - clearH) * 0.5f;
        add(1201, L"Clear Watch History", TooltipClientRect(view.right - clearW - 6.0f * dp, clearTop,
            view.right - 6.0f * dp, clearTop + clearH));
    }
    PruneInactiveToolIds(hubTooltips, activeIds);
}


void App::RefreshPlayerTooltips(bool force) {
    if (!playbackSession.IsOpen()) {
        if (playerTooltips.IsCreated()) {
            if (playerTooltips.IsActive()) playerTooltips.SetActive(false);
            playerTooltips.HideForKeyboard();
        }
        playerTooltipSignature = 0;
        return;
    }
    if (!playerTooltips.IsCreated() && !EnsurePlayerTooltips()) return;
    RECT client{};
    if (!GetClientRect(hWnd, &client)) return;
    const float dp = std::max(0.5f, overlayRenderer.GetDpiScale());
    const float winW = static_cast<float>(std::max<LONG>(1, client.right - client.left));
    const float winH = static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
    const bool controlsVisible = controlsOpacity > 0.02f && !isSettingsFlyoutOpen && !isContextMenuOpen;
    unsigned long long signature = 0xbb67ae8584caa73bull;
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(winW)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(winH)));
    MixTooltipSignature(signature, static_cast<unsigned long long>(std::lround(dp * 1000.0f)));
    MixTooltipSignature(signature, playbackSession.IsPiP() ? 1ull : 0ull);
    MixTooltipSignature(signature, controlsVisible ? 1ull : 0ull);
    MixTooltipSignature(signature, player.IsPlaying() ? 1ull : 0ull);
    MixTooltipSignature(signature, player.IsMuted() ? 1ull : 0ull);
    MixTooltipSignature(signature, playbackSession.IsFullscreen() ? 1ull : 0ull);
    MixTooltipSignature(signature, playbackSession.IsPiPPinned() ? 1ull : 0ull);
    MixTooltipSignature(signature, playbackSession.IsQueuePlayback() ? 1ull : 0ull);
    MixTooltipSignature(signature, playbackSession.GetQueueSize());
    MixTooltipSignature(signature, playbackSession.GetQueueIndex());
    MixShortcutsSignature(signature, settings.shortcuts);
    if (!force && signature == playerTooltipSignature) {
        if (playerTooltips.IsActive() != controlsVisible) playerTooltips.SetActive(controlsVisible);
        if (!controlsVisible) playerTooltips.HideForKeyboard();
        return;
    }
    playerTooltipSignature = signature;
    playerTooltips.SetActive(controlsVisible);
    if (!controlsVisible) { playerTooltips.HideForKeyboard(); return; }
    std::unordered_set<TooltipManager::ToolId> activeIds;
    const auto addRect = [&](TooltipManager::ToolId id, const std::wstring& text, const D2D1_RECT_F& rect) {
        activeIds.insert(id);
        playerTooltips.AddOrUpdate(id, text, TooltipClientRect(rect.left, rect.top, rect.right, rect.bottom));
    };
    if (playbackSession.IsPiP()) {
        const auto pip = Presentation::PiP(winW, winH, dp);
        addRect(2011, TooltipActionLabel(settings, L"Close Player", "closePlayer"), pip.close);
        addRect(2012, L"Restore Player", pip.restore);
        addRect(2013, TooltipActionLabel(settings, player.IsPlaying() ? L"Pause" : L"Play", "togglePlay"), pip.playPause);
        addRect(2014, TooltipActionLabel(settings, L"Previous in queue", "playPrevious"), pip.previous);
        addRect(2015, TooltipActionLabel(settings, L"Next in queue", "playNext"), pip.next);
        addRect(2016, L"Audio", pip.audio);
        addRect(2017, playbackSession.IsPiPPinned() ? L"Unpin from top" : L"Pin on top", pip.pin);
        PruneInactiveToolIds(playerTooltips, activeIds);
        return;
    }
    const bool queuePlayback = playbackSession.IsQueuePlayback();
    const auto layout = Presentation::Player(winW, winH, dp, queuePlayback);
    addRect(2001, TooltipActionLabel(settings, Loc::Tr("ux.back_library"), "closePlayer"), layout.back);
    if (queuePlayback) { addRect(2009, TooltipActionLabel(settings, L"Previous in queue", "playPrevious"), layout.prev); }
    addRect(2002, TooltipActionLabel(settings, player.IsPlaying() ? L"Pause" : L"Play", "togglePlay"), layout.play);
    addRect(2003, TooltipActionLabel(settings, Presentation::NextLabel(playbackSession.IsQueuePlayback(), playbackSession.GetQueueIndex(), playbackSession.GetQueueSize()), "playNext"), layout.next);
    addRect(2004, TooltipActionLabel(settings, player.IsMuted() ? L"Unmute" : L"Mute", "toggleMute"), layout.mute);
    addRect(2008, TooltipActionLabel(settings, playbackSession.IsFullscreen() ? L"Exit Fullscreen" : L"Fullscreen", "toggleFullscreen"), layout.fullscreen);
    if (layout.showPip) addRect(2007, TooltipActionLabel(settings, L"Picture-in-Picture", "togglePiP"), layout.pip);
    addRect(2006, TooltipActionLabel(settings, L"Settings", "openSettings"), layout.settings);
    if (layout.showSubtitles) addRect(2005, TooltipActionLabel(settings, L"Subtitles / CC", "toggleSubtitles"), layout.subtitles);
    PruneInactiveToolIds(playerTooltips, activeIds);
}


void App::HideTooltipKeyboardFocus() { hubTooltips.HideForKeyboard(); playerTooltips.HideForKeyboard(); }


void App::ShowHubTooltipForKeyboardFocus() {
    if (playbackSession.IsOpen() || activeFocusPane != FocusPane::TopNavBar || activeModal != ModalType::None || isContextMenuOpen) {
        hubTooltips.HideForKeyboard();
        return;
    }
    RefreshHubTooltips();
    const int index = std::clamp(topNavFocusedIndex, 0, 4);
    hubTooltips.ShowForKeyboard(1001 + static_cast<TooltipManager::ToolId>(index));
}


void App::QueueAccessibilityAction(LONG actionId) {
    if (!hWnd || shutdownComplete || actionId <= 0) return;
    PostMessageW(hWnd, kAccessibilityActionMessage, static_cast<WPARAM>(actionId), 0);
}


void App::InvokeAccessibilityAction(LONG actionId) {
    const auto it = accessibilityActions.find(actionId);
    if (it == accessibilityActions.end() || !it->second) return;
    const std::function<void()> action = it->second;
    action();
    accessibilityDirty = true;
    InvalidateRect(hWnd, NULL, FALSE);
    if (playbackSession.IsOpen()) Render();
}


void App::AnnounceStatus(const std::wstring& text, bool force) {
    if (text.empty() || shutdownComplete) return;
    if (mainThreadId != 0 && GetCurrentThreadId() != mainThreadId) {
        {
            std::lock_guard<std::mutex> lock(pendingAnnouncementMutex);
            if (pendingAnnouncements.size() < 32) { pendingAnnouncements.push_back(text); }
        }
        if (hWnd) { PostMessageW(hWnd, kAccessibilityAnnouncementMessage, 0, 0); }
        return;
    }
    std::wstring cleanText = text.substr(0, 512);
    while (!cleanText.empty() && iswspace(cleanText.front())) cleanText.erase(cleanText.begin());
    while (!cleanText.empty() && iswspace(cleanText.back())) cleanText.pop_back();
    if (cleanText.empty()) return;
    const bool isVolumeOrSeek = (cleanText.find(L'%') != std::wstring::npos) || (cleanText.find(L"Seek") == 0) ||
                                (cleanText.find(L"Volume") == 0) || (cleanText.find(L"Unmuted") == 0) || (cleanText == L"Muted");
    const ULONGLONG now = GetTickCount64();
    if (!force && cleanText == lastAnnouncedText && (now - lastAnnouncementTimeMs < 1000)) { return; }
    if (isVolumeOrSeek && !force && (now - lastVolumeOrSeekAnnounceMs < 150)) { pendingCoalescedAnnouncement = cleanText; return; }
    if (isVolumeOrSeek) { lastVolumeOrSeekAnnounceMs = now; }
    lastAnnouncedText = cleanText;
    lastAnnouncementTimeMs = now;
    currentLiveAnnouncement = cleanText;
    pendingCoalescedAnnouncement.clear();
    accessibilityTree.AnnounceLiveRegion(kStatusLiveRegionId, cleanText);
}


void App::ProcessPendingAnnouncements() {
    std::vector<std::wstring> queued;
    { std::lock_guard<std::mutex> lock(pendingAnnouncementMutex); queued.swap(pendingAnnouncements); }
    for (const auto& msg : queued) { AnnounceStatus(msg); }
}

} // namespace Aperture
