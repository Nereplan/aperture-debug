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


void App::HandleItemSelection(FocusPane pane, size_t index, bool isShift, bool isCtrl, size_t totalCount) {
    if (totalCount == 0) return;
    if (index >= totalCount) index = totalCount - 1;
    std::unordered_set<size_t>* pSet = nullptr;
    if (pane == FocusPane::MediaList) pSet = &selectedMediaIndices;
    else if (pane == FocusPane::PlaylistRail) pSet = &selectedPlaylistIndices;
    else if (pane == FocusPane::PlaylistDetail) pSet = &selectedEpisodeIndices;
    else if (pane == FocusPane::HistoryList) pSet = &selectedHistoryIndices;
    if (!pSet) return;
    activeFocusPane = pane;
    isKeyboardNavActive = true;
    const auto toStoredIndex = [&](size_t visibleIndex) -> size_t {
        if (pane != FocusPane::MediaList) return visibleIndex;
        const auto& visibleIndices = GetVisibleMediaIndices();
        return visibleIndex < visibleIndices.size() ? visibleIndices[visibleIndex] : visibleIndex;
    };
    if (isShift) {
        int anchor = (selectionAnchorIndex >= 0 && selectionAnchorIndex < (int)totalCount) ? selectionAnchorIndex : 0;
        int start = std::min(anchor, (int)index);
        int end = std::max(anchor, (int)index);
        pSet->clear();
        for (int i = start; i <= end; ++i) { pSet->insert(toStoredIndex((size_t)i)); }
        keyboardFocusedIndex = (int)index;
    } else if (isCtrl) {
        const size_t storedIndex = toStoredIndex(index);
        if (pSet->count(storedIndex) > 0) { if (pSet->size() > 1) pSet->erase(storedIndex); } else { pSet->insert(storedIndex); }
        selectionAnchorIndex = (int)index;
        keyboardFocusedIndex = (int)index;
    } else { pSet->clear(); pSet->insert(toStoredIndex(index)); selectionAnchorIndex = (int)index; keyboardFocusedIndex = (int)index; }
    if (pane == FocusPane::MediaList) { selectedMediaIndex = (int)toStoredIndex(index); }
}


void App::SelectAllInCurrentPane() {
    if (currentTab == TabView::Media) {
        if (libraryGroupMode == LibraryGroupMode::Folder && !selectedFolderKeys.empty()) {
            const auto& proj = GetCurrentMediaProjection();
            for (const auto& g : proj.groups) selectedFolderKeys.insert(g.key);
            InvalidateRect(hWnd, NULL, FALSE);
            return;
        }
        selectedMediaIndices.clear();
        for (size_t i = 0; i < library.mediaItems.size(); ++i) selectedMediaIndices.insert(i);
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (!visibleIndices.empty()) keyboardFocusedIndex = (int)visibleIndices.size() - 1;
    } else if (currentTab == TabView::Playlists) {
        if (activeFocusPane == FocusPane::PlaylistRail) {
            selectedPlaylistIndices.clear();
            for (size_t i = 0; i < library.playlists.size(); ++i) selectedPlaylistIndices.insert(i);
        } else {
            Playlist* pl = library.FindPlaylist(activePlaylistId);
            if (pl) {
                selectedEpisodeIndices.clear();
                const auto visible = GetVisiblePlaylistIndices();
                for (size_t i : visible) selectedEpisodeIndices.insert(i);
            }
        }
    } else if (currentTab == TabView::History) {
        selectedHistoryIndices.clear();
        for (size_t i = 0; i < library.history.size(); ++i) selectedHistoryIndices.insert(i);
    }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ClearSelectionInCurrentPane() {
    if (currentTab == TabView::Media) {
        selectedFolderKeys.clear();
        selectedMediaIndices.clear();
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size())
            selectedMediaIndices.insert(visibleIndices[keyboardFocusedIndex]);
    } else if (currentTab == TabView::Playlists) {
        if (activeFocusPane == FocusPane::PlaylistRail) { selectedPlaylistIndices.clear(); } else { selectedEpisodeIndices.clear(); }
    } else if (currentTab == TabView::History) { selectedHistoryIndices.clear(); }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::ArmLibraryUndo(const std::wstring& actionLabel) {
    try {
        undoLibrarySnapshot = std::make_unique<MediaLibrary>(library);
        undoLibraryActionLabel = actionLabel;
    } catch (...) { undoLibrarySnapshot.reset(); undoLibraryActionLabel.clear(); }
}


bool App::UndoLastLibraryAction() {
    if (!undoLibrarySnapshot) return false;
    library = std::move(*undoLibrarySnapshot);
    undoLibrarySnapshot.reset();
    const std::wstring action = std::move(undoLibraryActionLabel);
    undoLibraryActionLabel.clear();
    selectedMediaIndices.clear();
    selectedPlaylistIndices.clear();
    selectedEpisodeIndices.clear();
    selectedHistoryIndices.clear();
    InvalidateLibraryDerivedState();
    SaveLibrary();
    ShowOSD(IconType::Refresh, action.empty() ? L"Change undone" : action + L" undone");
    InvalidateRect(hWnd, NULL, FALSE);
    return true;
}


void App::ConfirmDeleteSelectedMedia() {
    if (selectedMediaIndices.empty()) {
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[hoveredMediaIndex]);
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[keyboardFocusedIndex]);
        }
    }
    if (selectedMediaIndices.empty()) return;
    std::vector<std::string> idsToDelete;
    idsToDelete.reserve(selectedMediaIndices.size());
    for (size_t index : selectedMediaIndices) {
        if (index < library.mediaItems.size()) { idsToDelete.push_back(library.mediaItems[index].id); }
    }
    if (idsToDelete.empty()) return;
    const size_t count = idsToDelete.size();
    std::wstring message = (count == 1)
        ? L"Are you sure you want to remove this item from the library? The media file on disk will not be deleted."
        : L"Are you sure you want to remove " + std::to_wstring(count) + L" selected items from the library? The media files on disk will not be deleted.";
    ShowConfirmationModal(L"Remove from Library", message, L"Remove", [this, idsToDelete]() {
        ArmLibraryUndo(L"Library removal");
        std::vector<MediaLibrary::RemovedMediaInfo> removedItems;
        size_t skipped = 0;
        const size_t removed = library.RemoveMedia(idsToDelete, &skipped, true, &removedItems);
        SaveLibrary();
        if (removed > 0) {
            // Drain every producer before deleting disk entries, then cancel
            // stale renderer requests and wait for any write already in flight.
            if (previewInitialized || previewInitAttempted) previewPlayer.Shutdown();
            previewInitialized = false;
            previewInitAttempted = false;
            hubExtractors[0].Shutdown();
            hubExtractors[1].Shutdown();
            renderer.ClearThumbnailCache();
            for (const auto& item : removedItems) {
                PurgeThumbnailCacheForFile(item.filePath, item.durationSec);
            }
            // Also removes dimension-dependent legacy hub keys, whose hashes cannot be reconstructed reliably after shell extraction.
            PruneOrphanedHubThumbnailCache();
            cachedVisibleVideoPaths.clear();
            lastVisibleCollectMs = 0;
            lastPrefetchSig = 0;
        }
        selectedMediaIndices.clear();
        m_mediaQueryEngine.InvalidateCache();
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (!visibleIndices.empty()) {
            keyboardFocusedIndex = std::min(keyboardFocusedIndex, (int)visibleIndices.size() - 1);
            if (keyboardFocusedIndex >= 0) selectedMediaIndices.insert(visibleIndices[keyboardFocusedIndex]);
        } else { keyboardFocusedIndex = -1; }
        if (removed > 0) {
            if (skipped == 0) {
                ShowOSD(IconType::Trash, L"Removed " + std::to_wstring(removed) + L" item" + (removed == 1 ? L"" : L"s") + L" from library \u2022 Ctrl+Z to undo");
            } else {
                ShowOSD(IconType::Trash, L"Removed " + std::to_wstring(removed) + L" items (" + std::to_wstring(skipped) + L" skipped) \u2022 Ctrl+Z to undo");
            }
        } else { ShowOSD(IconType::Trash, L"No items removed"); }
        InvalidateRect(hWnd, NULL, FALSE);
    });
}


void App::BulkSetWatched(bool watched) {
    if (selectedMediaIndices.empty()) {
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[hoveredMediaIndex]);
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[keyboardFocusedIndex]);
        }
    }
    if (selectedMediaIndices.empty()) return;
    std::vector<std::wstring> paths;
    paths.reserve(selectedMediaIndices.size());
    for (size_t idx : selectedMediaIndices) {
        if (idx < library.mediaItems.size()) { paths.push_back(library.mediaItems[idx].filePath); }
    }
    if (paths.empty()) return;
    size_t skipped = 0;
    size_t completed = library.SetWatched(paths, watched, &skipped);
    SaveLibrary();
    if (watched) {
        if (completed > 0 && skipped == 0) {
            ShowOSD(IconType::Checkmark, L"Marked " + std::to_wstring(completed) + L" item" + (completed == 1 ? L"" : L"s") + L" as watched");
        } else if (completed > 0 && skipped > 0) {
            ShowOSD(IconType::Checkmark, L"Marked " + std::to_wstring(completed) + L" as watched (" + std::to_wstring(skipped) + L" skipped)");
        } else { ShowOSD(IconType::Checkmark, L"No items updated"); }
    } else {
        if (completed > 0 && skipped == 0) {
            ShowOSD(IconType::Refresh, L"Marked " + std::to_wstring(completed) + L" item" + (completed == 1 ? L"" : L"s") + L" as unwatched");
        } else if (completed > 0 && skipped > 0) {
            ShowOSD(IconType::Refresh, L"Marked " + std::to_wstring(completed) + L" as unwatched (" + std::to_wstring(skipped) + L" skipped)");
        } else { ShowOSD(IconType::Refresh, L"No items updated"); }
    }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::BulkAddToPlaylist(const std::string& playlistId) {
    Playlist* pl = library.FindPlaylist(playlistId);
    if (!pl || pl->IsSmart()) return;
    std::wstring plName = Utf8ToUtf16(pl->name);
    if (selectedMediaIndices.empty()) {
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[hoveredMediaIndex]);
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size()) {
            selectedMediaIndices.insert(visibleIndices[keyboardFocusedIndex]);
        }
    }
    if (selectedMediaIndices.empty()) return;
    std::vector<std::wstring> paths;
    paths.reserve(selectedMediaIndices.size());
    for (size_t idx : selectedMediaIndices) {
        if (idx < library.mediaItems.size()) { paths.push_back(library.mediaItems[idx].filePath); }
    }
    if (paths.empty()) return;
    size_t skipped = 0;
    size_t added = library.AddToPlaylist(playlistId, paths, &skipped);
    SaveLibrary();
    if (added > 0 && skipped == 0) {
        ShowOSD(IconType::FolderPlaylist, L"Added " + std::to_wstring(added) + L" item" + (added == 1 ? L"" : L"s") + L" to " + plName);
    } else if (added > 0 && skipped > 0) {
        ShowOSD(IconType::FolderPlaylist, L"Added " + std::to_wstring(added) + L" to " + plName + L" (" + std::to_wstring(skipped) + L" duplicates skipped)");
    } else if (added == 0 && skipped > 0) {
        ShowOSD(IconType::FolderPlaylist, L"Items already in " + plName);
    } else { ShowOSD(IconType::FolderPlaylist, L"No items added"); }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::DeleteSelectedInCurrentPane() {
    if (currentTab == TabView::Media) {
        if ((libraryGroupMode == LibraryGroupMode::Folder || libraryGroupMode == LibraryGroupMode::Series) && !selectedFolderKeys.empty()) {
            RemoveSelectedFolders();
            return;
        }
        ConfirmDeleteSelectedMedia();
    } else if (currentTab == TabView::Playlists) {
        if (activeFocusPane == FocusPane::PlaylistDetail) {
            Playlist* pl = library.FindPlaylist(activePlaylistId);
            if (!pl || pl->itemPaths.empty()) return;
            if (selectedEpisodeIndices.empty() && hoveredPlaylistDetailItemIndex >= 0)
                selectedEpisodeIndices.insert((size_t)hoveredPlaylistDetailItemIndex);
            if (selectedEpisodeIndices.empty()) return;
            const size_t count = EraseSelectedIndices(pl->itemPaths, selectedEpisodeIndices);
            selectedEpisodeIndices.clear();
            ClearPlaylistCardHover();
            playlistCardExpands.clear();
            library.NotifyPlaylistEdited();
            SaveLibrary();
            if (!pl->itemPaths.empty()) {
                keyboardFocusedIndex = std::min(keyboardFocusedIndex, (int)pl->itemPaths.size() - 1);
                if (keyboardFocusedIndex >= 0) selectedEpisodeIndices.insert((size_t)keyboardFocusedIndex);
            } else { keyboardFocusedIndex = -1; }
            ShowOSD(IconType::Trash, L"Removed " + std::to_wstring(count) + L" items from playlist");
            InvalidateRect(hWnd, NULL, FALSE);
        } else if (activeFocusPane == FocusPane::PlaylistRail) {
            if (selectedPlaylistIndices.empty() && hoveredPlaylistRailIndex >= 0)
                selectedPlaylistIndices.insert((size_t)hoveredPlaylistRailIndex);
            if (selectedPlaylistIndices.empty()) return;
            const size_t count = EraseSelectedIndices(library.playlists, selectedPlaylistIndices);
            selectedPlaylistIndices.clear();
            if (!library.playlists.empty()) {
                activePlaylistId = library.playlists[0].id;
                keyboardFocusedIndex = 0;
            } else { activePlaylistId = ""; keyboardFocusedIndex = -1; }
            library.NotifyPlaylistEdited();
            SaveLibrary();
            ShowOSD(IconType::Trash, L"Removed " + std::to_wstring(count) + L" playlists");
            InvalidateRect(hWnd, NULL, FALSE);
        }
    } else if (currentTab == TabView::History) {
        if (selectedHistoryIndices.empty() && hoveredHistoryIndex >= 0) { selectedHistoryIndices.insert((size_t)hoveredHistoryIndex); }
        if (selectedHistoryIndices.empty()) return;
        const size_t count = EraseSelectedIndices(library.history, selectedHistoryIndices);
        selectedHistoryIndices.clear();
        SaveLibrary();
        if (!library.history.empty()) {
            keyboardFocusedIndex = std::min(keyboardFocusedIndex, (int)library.history.size() - 1);
            if (keyboardFocusedIndex >= 0) selectedHistoryIndices.insert((size_t)keyboardFocusedIndex);
        } else { keyboardFocusedIndex = -1; }
        ShowOSD(IconType::Trash, L"Removed " + std::to_wstring(count) + L" history records");
        InvalidateRect(hWnd, NULL, FALSE);
    }
}


void App::PlaySelectedInCurrentPane() {
    if (currentTab == TabView::Media) {
        const auto& visibleIndices = GetVisibleMediaIndices();
        std::vector<std::wstring> queue;
        if (!selectedMediaIndices.empty()) {
            std::vector<size_t> sorted(selectedMediaIndices.begin(), selectedMediaIndices.end());
            std::sort(sorted.begin(), sorted.end());
            for (size_t idx : sorted) {
                if (idx < library.mediaItems.size()) { queue.push_back(library.mediaItems[idx].filePath); }
            }
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)visibleIndices.size()) {
            size_t origIdx = visibleIndices[keyboardFocusedIndex];
            if (origIdx < library.mediaItems.size()) { queue.push_back(library.mediaItems[origIdx].filePath); }
        } else if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
            size_t origIdx = visibleIndices[hoveredMediaIndex];
            if (origIdx < library.mediaItems.size()) { queue.push_back(library.mediaItems[origIdx].filePath); }
        }
        if (!queue.empty()) {
            if (queue.size() > 1) { playbackSession.OpenQueue(queue, 0); } else { playbackSession.OpenDirect(queue[0], 0.0); }
            OpenAndPlayMedia(queue[0], 0.0, queue.size() > 1);
        }
    } else if (currentTab == TabView::Playlists) {
        Playlist* pl = library.FindPlaylist(activePlaylistId);
        if (!pl || pl->itemPaths.empty()) return;
        if (activeFocusPane == FocusPane::PlaylistRail) { PlayPlaylist(activePlaylistId, 0); return; }
        if (!selectedEpisodeIndices.empty()) {
            if (selectedEpisodeIndices.size() == 1) { PlayPlaylist(activePlaylistId, *selectedEpisodeIndices.begin()); return; }
            std::vector<size_t> sorted(selectedEpisodeIndices.begin(), selectedEpisodeIndices.end());
            std::sort(sorted.begin(), sorted.end());
            std::vector<std::wstring> queue;
            for (size_t idx : sorted) {
                if (idx < pl->itemPaths.size()) { queue.push_back(pl->itemPaths[idx]); }
            }
            if (!queue.empty()) {
                if (queue.size() > 1) { playbackSession.OpenQueue(queue, 0); } else { playbackSession.OpenDirect(queue[0], 0.0); }
                OpenAndPlayMedia(queue[0], 0.0, queue.size() > 1);
            }
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)pl->itemPaths.size()) {
            PlayPlaylist(activePlaylistId, (size_t)keyboardFocusedIndex);
        } else if (hoveredPlaylistDetailItemIndex >= 0 && hoveredPlaylistDetailItemIndex < (int)pl->itemPaths.size()) {
            PlayPlaylist(activePlaylistId, (size_t)hoveredPlaylistDetailItemIndex);
        }
    } else if (currentTab == TabView::History) {
        if (!selectedHistoryIndices.empty()) {
            size_t first = *selectedHistoryIndices.begin();
            if (first < library.history.size()) { OpenAndPlayMedia(library.history[first].filePath, library.history[first].lastPosSec); }
        } else if (keyboardFocusedIndex >= 0 && keyboardFocusedIndex < (int)library.history.size()) {
            OpenAndPlayMedia(library.history[keyboardFocusedIndex].filePath, library.history[keyboardFocusedIndex].lastPosSec);
        } else if (hoveredHistoryIndex >= 0 && hoveredHistoryIndex < (int)library.history.size()) {
            OpenAndPlayMedia(library.history[hoveredHistoryIndex].filePath, library.history[hoveredHistoryIndex].lastPosSec);
        }
    }
}


void App::AutoScrollToFocusedItem() {
    float dp = renderer.GetDpiScale();
    RECT rc;
    GetClientRect(hWnd, &rc);
    const float rcW = (float)(rc.right - rc.left);
    const float winH = (float)(rc.bottom - rc.top);
    float viewH = RectHeight(Presentation::HubView(rcW, winH, dp));
    if (currentTab == TabView::Media) {
        const auto& visibleIndices = GetVisibleMediaIndices();
        if (keyboardFocusedIndex < 0 || keyboardFocusedIndex >= (int)visibleIndices.size()) return;
        EnsureMediaLayoutCache();
        float curY = GetMediaRowTopDIP(keyboardFocusedIndex) * dp;
        float focusH = GetMediaRowHeightDIP(keyboardFocusedIndex) * dp;
        if (curY < mediaScrollY) {
            mediaScrollY = std::max(0.0f, curY - 14.0f * dp);
        } else if (curY + focusH > mediaScrollY + viewH) { mediaScrollY = curY + focusH - viewH + 20.0f * dp; }
    } else if (currentTab == TabView::Playlists) {
        if (activeFocusPane == FocusPane::PlaylistRail) {
            if (keyboardFocusedIndex < 0 || keyboardFocusedIndex >= (int)library.playlists.size()) return;
            float itemH = 52.0f * dp;
            float curY = keyboardFocusedIndex * (itemH + 6.0f * dp);
            float railViewH = viewH - 48.0f * dp - 34.0f * dp - 18.0f * dp;
            if (curY < playlistRailScrollY) {
                playlistRailScrollY = std::max(0.0f, curY);
            } else if (curY + itemH > playlistRailScrollY + railViewH) { playlistRailScrollY = curY + itemH - railViewH; }
            ClampScrollOffsets();
        } else if (activeFocusPane == FocusPane::PlaylistDetail) {
            Playlist* pl = library.FindPlaylist(activePlaylistId);
            if (!pl) return;
            if (keyboardFocusedIndex == -1) { playlistDetailScrollY = 0.0f; ClampScrollOffsets(); return; }
            if (keyboardFocusedIndex < 0 || keyboardFocusedIndex >= (int)pl->itemPaths.size()) return;
            const auto visible = GetVisiblePlaylistIndices();
            size_t displayIndex = 0;
            auto found = std::find(visible.begin(), visible.end(), (size_t)keyboardFocusedIndex);
            if (found == visible.end()) return;
            displayIndex = (size_t)std::distance(visible.begin(), found);
            float itemH = playlistGridView ? 166.0f * dp : 62.0f * dp, itemGap = playlistGridView ? 10.0f * dp : 6.0f * dp;
            float winW = (float)std::max<LONG>(1, rc.right - rc.left);
            float padX = 18.0f * dp;
            float availableW = std::max(1.0f * dp, winW - padX * 2.0f);
            float railW = std::min(260.0f * dp, std::max(120.0f * dp, availableW * 0.42f));
            float detailLeft = std::min(winW - padX, padX + railW + 16.0f * dp);
            float paneW = std::max(120.0f * dp, winW - padX - detailLeft - 26.0f * dp);
            size_t gridColumns = playlistGridView ? (size_t)std::max(1.0f, std::floor((paneW + itemGap) / (190.0f * dp))) : 1;
            float curY = playlistGridView ? (displayIndex / gridColumns) * (itemH + itemGap) : displayIndex * (itemH + itemGap);
            float detailViewH = viewH - 14.0f * dp - 50.0f * dp - 16.0f * dp - 112.0f * dp - 16.0f * dp - 32.0f * dp - 12.0f * dp;
            if (curY < playlistDetailScrollY) {
                playlistDetailScrollY = std::max(0.0f, curY);
            } else if (curY + itemH + 40.0f * dp > playlistDetailScrollY + detailViewH) {
                playlistDetailScrollY = curY + itemH + 40.0f * dp - detailViewH;
            }
            ClampScrollOffsets();
        }
    } else if (currentTab == TabView::History) {
        if (keyboardFocusedIndex < 0 || keyboardFocusedIndex >= (int)library.history.size()) return;
        const HistoryTimelineProjection& proj = GetHistoryProjection();
        float headerY = 0.0f, curY = 0.0f, itemH = 0.0f;
        bool found = false;
        for (size_t g = 0; g < proj.groups.size(); ++g) {
            const auto& group = proj.groups[g];
            const HistoryGroupLayout layout(headerY, group.count, dp);
            headerY = layout.NextHeaderTop();
            const auto begin = proj.itemIndices.begin() + group.startIndex;
            const auto end = begin + group.count;
            const auto focused = std::find(begin, end, static_cast<size_t>(keyboardFocusedIndex));
            if (focused != end) {
                curY = layout.RowTop(static_cast<size_t>(focused - begin));
                itemH = layout.rowHeight;
                found = true;
                break;
            }
        }
        if (found) {
            float histViewH = viewH - kHistoryContentInset * dp;
            if (curY < historyScrollY) {
                historyScrollY = std::max(0.0f, curY);
            } else if (curY + itemH > historyScrollY + histViewH) { historyScrollY = curY + itemH - histViewH + 10.0f * dp; }
        }
    } else if (currentTab == TabView::Settings && activeFocusPane == FocusPane::SettingsContent) {
        int rowCount = currentSettingsCat == SettingsCategory::Appearance ? 2 :
            (currentSettingsCat == SettingsCategory::Shortcuts ? (int)settings.GetShortcutActionListRef().size() + 1 :
                (currentSettingsCat == SettingsCategory::System ? SettingsLayout::kSystemRowCount : 6));
        if (keyboardFocusedIndex < 0 || keyboardFocusedIndex >= rowCount) return;
        const float sectionTop = (SettingsLayout::kContentStartOffset +
            (currentSettingsCat == SettingsCategory::Subtitles ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve)) * dp;
        float focusTop = sectionTop, focusH = 46.0f * dp;
        if (currentSettingsCat == SettingsCategory::Appearance) {
            const auto& themes = settings.GetAllThemes();
            const auto languages = settings.GetAvailableLanguages();
            const float themeCardH = 46.0f * dp;
            const size_t themeRows = (themes.size() + 1) / 2;
            const float themeGridH = themeRows > 0 ? themeRows * (themeCardH + 8.0f * dp) - 8.0f * dp : 0.0f;
            focusTop = sectionTop + 28.0f * dp;
            if (keyboardFocusedIndex == 0) { focusH = std::max(themeCardH, themeGridH); } else {
                focusTop += themeRows * (themeCardH + 8.0f * dp) + 16.0f * dp + 28.0f * dp;
                const float langH = 34.0f * dp;
                const size_t languageRows = (languages.size() + 2) / 3;
                focusH = languageRows > 0 ? languageRows * (langH + 8.0f * dp) - 8.0f * dp : langH;
            }
        } else if (currentSettingsCat == SettingsCategory::Playback) {
            const float cardGap = SettingsLayout::kSectionCardGap * dp, titleH = SettingsLayout::kPlaybackCardTitleHeight * dp;
            const float rowH = SettingsLayout::kPlaybackBehaviorRowHeight * dp, behaviorTop = sectionTop;
            if (keyboardFocusedIndex <= 3) {
                focusTop = behaviorTop + titleH + keyboardFocusedIndex * rowH;
                focusH = rowH;
            } else if (keyboardFocusedIndex == 4) {
                focusTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap;
                focusH = SettingsLayout::kPlaybackSimpleCardHeight * dp;
            } else if (keyboardFocusedIndex == 5) {
                focusTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap +
                    SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap;
                focusH = SettingsLayout::kPlaybackSimpleCardHeight * dp;
            } else if (keyboardFocusedIndex == 6) {
                focusTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap +
                    2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) + 42.0f * dp;
                focusH = 34.0f * dp;
            } else if (keyboardFocusedIndex == 7) {
                focusTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap +
                    2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) +
                    SettingsLayout::kPlaybackSpeedCardHeight * dp + cardGap + titleH;
                focusH = SettingsLayout::kPlaybackMpvSwitchHeight * dp;
            } else {
                focusTop = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap +
                    2.0f * (SettingsLayout::kPlaybackSimpleCardHeight * dp + cardGap) +
                    SettingsLayout::kPlaybackSpeedCardHeight * dp + cardGap + SettingsLayout::kPlaybackMpvActionTop * dp +
                    (keyboardFocusedSubIndex >= 0 ? keyboardFocusedSubIndex : 0) *
                        (SettingsLayout::kPlaybackMpvActionHeight + SettingsLayout::kPlaybackMpvActionGap) * dp;
                focusH = SettingsLayout::kPlaybackMpvActionHeight * dp;
            }
        } else if (currentSettingsCat == SettingsCategory::Subtitles) {
            const float cardGap = SettingsLayout::kSubtitleCardGap * dp;
            const float cardHeights[] = {SettingsLayout::kSubtitleTrackCardHeight * dp, SettingsLayout::kSubtitleTrackCardHeight * dp,
                SettingsLayout::kSubtitleFontCardHeight * dp, SettingsLayout::kSubtitleColorCardHeight * dp,
                SettingsLayout::kSubtitleStyleCardHeight * dp, SettingsLayout::kSubtitlePositionCardHeight * dp};
            const int row = std::clamp(keyboardFocusedIndex, 0, 5);
            focusTop = sectionTop + SettingsLayout::kSubtitlePreviewCardHeight * dp + cardGap;
            for (int i = 0; i < row; ++i) focusTop += cardHeights[i] + cardGap;
            focusH = cardHeights[row];
        } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
            if (keyboardFocusedIndex == 0) {
                focusTop = (SettingsLayout::kContentStartOffset * dp) - 2.0f * dp;
                focusH = 26.0f * dp;
            } else { focusTop = sectionTop + 32.0f * dp + (keyboardFocusedIndex - 1) * 43.0f * dp; focusH = 38.0f * dp; }
        } else if (currentSettingsCat == SettingsCategory::System) {
            const float actionsTop = sectionTop + SettingsLayout::kSystemActionsTop * dp;
            focusTop = actionsTop + keyboardFocusedIndex * (58.0f + 10.0f) * dp;
            focusH = 58.0f * dp;
        }
        const float scrollViewportTop = sectionTop;
        if (focusTop - settingsScrollY < scrollViewportTop) settingsScrollY = std::max(0.0f, focusTop - scrollViewportTop);
        else if (focusTop + focusH - settingsScrollY > viewH) settingsScrollY = focusTop + focusH - viewH;
        ClampScrollOffsets();
    }
}

} // namespace Aperture
