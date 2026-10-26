#include "Reorder.h"
#include "App.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "CollectionAlgorithms.h"
#include "FeatureGate.h"
#include "IpcProtocol.h"
#include "MediaFoundation.h"
#include "OnlineSubtitles.h"
#include "Updater.h"
#include "Diagnostics.h"
#include "ChannelPolicy.h"
#include "JumpList.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "WindowGeometry.h"
#include "resource.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <imm.h>
#include <numeric>

namespace Aperture {

const std::vector<size_t>& App::GetVisiblePlaylistIndices() {
    std::wstring currentQuery = Text16ToWideString(playlistSearchInput.GetText());
    const auto& revisions = library.GetRevisions();
    const bool sameRequest = cachedVisiblePlaylistId == activePlaylistId && cachedVisibleSearchQuery == currentQuery &&
        cachedVisibleRevisions.catalog == revisions.catalog && cachedVisibleRevisions.playlists == revisions.playlists;
    // Manual membership has no external refresh deadline. Smart playlists
    // resolve their current path generation before reusing row indices.
    if (sameRequest && !cachedVisiblePlaylistIsSmart) return cachedVisiblePlaylistIndices;
    const Playlist* playlist = library.FindPlaylist(activePlaylistId);
    if (!playlist) { cachedVisiblePlaylistId.clear(); cachedVisiblePlaylistIndices.clear(); return cachedVisiblePlaylistIndices; }
    const auto pathView = library.ResolvePlaylistPaths(*playlist);
    const auto currentRevisions = library.GetRevisions();
    if (sameRequest && cachedVisiblePathsGeneration == pathView.generation &&
        cachedVisibleRevisions.catalog == currentRevisions.catalog) {
        return cachedVisiblePlaylistIndices;
    }
    cachedVisiblePlaylistIndices.clear();
    cachedVisiblePlaylistId = activePlaylistId;
    cachedVisibleSearchQuery = currentQuery;
    cachedVisibleRevisions = currentRevisions;
    cachedVisiblePathsGeneration = pathView.generation;
    cachedVisiblePlaylistIsSmart = playlist->IsSmart();
    const auto& itemPaths = pathView.paths;
    if (itemPaths.empty()) return cachedVisiblePlaylistIndices;
    if (currentQuery.empty()) {
        cachedVisiblePlaylistIndices.resize(itemPaths.size());
        std::iota(cachedVisiblePlaylistIndices.begin(), cachedVisiblePlaylistIndices.end(), 0);
        return cachedVisiblePlaylistIndices;
    }
    if (cachedSearchKeyPlaylistId != activePlaylistId || cachedSearchKeyRevisions.catalog != currentRevisions.catalog ||
        cachedSearchKeyRevisions.playlists != currentRevisions.playlists || cachedSearchPathsGeneration != pathView.generation ||
        cachedPlaylistSearchKeys.size() != itemPaths.size()) {
        cachedSearchKeyPlaylistId = activePlaylistId;
        cachedSearchKeyRevisions = currentRevisions;
        cachedSearchPathsGeneration = pathView.generation;
        cachedPlaylistSearchKeys.clear();
        cachedPlaylistSearchKeys.reserve(itemPaths.size());
        for (const auto& path : itemPaths) {
            const MediaItem* item = library.FindByPath(path);
            std::wstring key = item ? item->title : GetFileNameFromPath(path);
            key.push_back(L'\n');
            key += GetFileNameFromPath(path);
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
            cachedPlaylistSearchKeys.push_back(std::move(key));
        }
    }
    cachedVisiblePlaylistIndices.reserve(itemPaths.size());
    std::wstring lowerQuery = currentQuery;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
    for (size_t i = 0; i < cachedPlaylistSearchKeys.size(); ++i) {
        if (cachedPlaylistSearchKeys[i].find(lowerQuery) != std::wstring::npos) { cachedVisiblePlaylistIndices.push_back(i); }
    }
    return cachedVisiblePlaylistIndices;
}

class ApertureDropTarget : public IDropTarget {
public:
    App* app = nullptr;
    LONG refCount = 1;
    ApertureDropTarget(App* pApp) : app(pApp), refCount(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppvObject = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&refCount);
        if (count == 0) { delete this; return 0; }
        return count;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        if (app) app->OnOleDragEnter(pDataObj, pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        if (app) app->OnOleDragOver(pt);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { if (app) app->OnOleDragLeave(); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        if (!pdwEffect) return E_POINTER;
        *pdwEffect = DROPEFFECT_COPY;
        if (app) app->OnOleDrop(pDataObj, pt);
        return S_OK;
    }
};

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TABLET_QUERYSYSTEMGESTURESTATUS) { return TabletSystemGestureStatus(); }
    App* pApp = (App*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (pApp) { return pApp->HandleOverlayMessage(hwnd, uMsg, wParam, lParam); }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

App::App() {
    mainThreadId = GetCurrentThreadId();
    auto logMpvInitialization = [](const MpvInitializationDiagnostic& diagnostic) {
        auto error = AppError::Playback(Utf8ToUtf16(diagnostic.message), diagnostic.errorCode);
        error.severity = diagnostic.essential ? ErrorSeverity::Error : ErrorSeverity::Warning;
        Diagnostics::Log(error);
    };
    player.SetInitializationDiagnosticSink(logMpvInitialization);
    previewPlayer.SetInitializationDiagnosticSink(logMpvInitialization);
    for (auto& extractor : hubExtractors) extractor.SetInitializationDiagnosticSink(logMpvInitialization);
    commandDispatcher.SetContext(this);
    playlistSearchInput.SetMaxLength(512);
    mediaSearchInput.SetMaxLength(512);
    modalInput.SetMaxLength(256);
    playbackSpeedInput.SetMaxLength(32);
    inlineRenameInput.SetMaxLength(256);
}

void App::DismissPopups() {
    if (isSettingsFlyoutOpen) { CloseSettingsFlyout(); }
    if (isContextMenuOpen) { CloseContextMenu(); }
}

App::FocusSnapshot App::CaptureCurrentFocus(TransientSurfaceType surfaceType) const {
    FocusSnapshot s;
    s.surfaceType = surfaceType;
    s.tab = currentTab;
    s.pane = activeFocusPane;
    s.topNavIndex = topNavFocusedIndex;
    s.keyboardIndex = keyboardFocusedIndex;
    s.keyboardSubIndex = keyboardFocusedSubIndex;
    s.textInputField = activeTextInputField;
    s.libraryVersion = library.GetVersion();
    s.playerActive = playbackSession.IsOpen();
    s.playerButtonId = hoveredPlayerButtonId;
    if (s.playerActive) {
        if (playerKeyboardFocusId > 0) {
            s.accessibilityId = playerKeyboardFocusId;
            if (playerKeyboardFocusId == 9006) s.entityId = "settings_button";
        } else if (playbackSession.IsPiP()) {
            switch (hoveredPlayerButtonId) {
            case 1: s.accessibilityId = 9020; break;
            case 2: s.accessibilityId = 9021; break;
            case 3: s.accessibilityId = 9022; break;
            case 4: s.accessibilityId = 9023; break;
            case 5: s.accessibilityId = 9024; break;
            case 6: s.accessibilityId = 9025; break;
            case 7: s.accessibilityId = 9026; break;
            default: s.accessibilityId = 9000; break;
            }
        } else if (isBackHovered) { s.accessibilityId = 9001; } else if (hoveredPlayerButtonId == 5 || isSettingsFlyoutOpen) {
            s.accessibilityId = 9006;
            s.entityId = "settings_button";
        } else if (hoveredPlayerButtonId == 1) { s.accessibilityId = 9002; } else if (hoveredPlayerButtonId == 2) {
            s.accessibilityId = 9011;
        } else if (hoveredPlayerButtonId == 3) { s.accessibilityId = 9004; } else if (hoveredPlayerButtonId == 4) {
            s.accessibilityId = 9005;
        } else if (hoveredPlayerButtonId == 6) { s.accessibilityId = 9007; } else if (hoveredPlayerButtonId == 7) {
            s.accessibilityId = 9008;
        } else if (hoveredPlayerButtonId == 8) { s.accessibilityId = 9010; } else { s.accessibilityId = 9000; }
        return s;
    }
    if (activeFocusPane == FocusPane::TopNavBar) {
        s.accessibilityId = 100 + std::clamp(topNavFocusedIndex, 0, 4);
    } else if (activeFocusPane == FocusPane::MediaList) {
        const_cast<App*>(this)->EnsureMediaLayoutCache();
        const auto& visible = const_cast<App*>(this)->GetVisibleMediaIndices();
        if (keyboardFocusedIndex >= 0 && static_cast<size_t>(keyboardFocusedIndex) < visible.size()) {
            size_t origIdx = visible[keyboardFocusedIndex];
            if (origIdx < library.mediaItems.size()) {
                s.entityPath = library.mediaItems[origIdx].filePath;
                s.accessibilityId = AccessibilityStableId(1000, s.entityPath);
            }
        } else { s.accessibilityId = 1081 + std::clamp(keyboardFocusedIndex == 1 ? 1 : 0, 0, 1); }
    } else if (activeFocusPane == FocusPane::PlaylistRail) {
        if (keyboardFocusedIndex >= 0 && static_cast<size_t>(keyboardFocusedIndex) < library.playlists.size()) {
            s.entityId = library.playlists[keyboardFocusedIndex].id;
            s.accessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(s.entityId));
        } else if (!activePlaylistId.empty()) {
            s.entityId = activePlaylistId;
            s.accessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId));
        } else { s.accessibilityId = 3000; }
    } else if (activeFocusPane == FocusPane::PlaylistDetail) {
        const Playlist* pl = library.FindPlaylist(activePlaylistId);
        if (pl) {
            s.entityId = pl->id;
            if (activeTextInputField == TextInputField::PlaylistSearch) {
                s.accessibilityId = 3050;
            } else if (keyboardFocusedIndex >= 0 && static_cast<size_t>(keyboardFocusedIndex) < pl->itemPaths.size()) {
                s.entityPath = pl->itemPaths[keyboardFocusedIndex];
                s.accessibilityId = AccessibilityStableId(3200, s.entityPath);
            } else { s.accessibilityId = 3050; }
        } else { s.accessibilityId = 3000; }
    } else if (activeFocusPane == FocusPane::HistoryList) {
        if (keyboardFocusedIndex >= 0 && static_cast<size_t>(keyboardFocusedIndex) < library.history.size()) {
            s.entityPath = library.history[keyboardFocusedIndex].filePath;
            s.accessibilityId = AccessibilityStableId(4000, s.entityPath);
        } else { s.accessibilityId = 4081; }
    } else if (activeFocusPane == FocusPane::SettingsCategoryList) {
        s.accessibilityId = 500 + std::clamp(keyboardFocusedIndex >= 0 ? keyboardFocusedIndex : static_cast<int>(currentSettingsCat), 0, 7);
    } else if (activeFocusPane == FocusPane::SettingsContent) {
        if (currentSettingsCat == SettingsCategory::Shortcuts) {
            if (keyboardFocusedIndex == 0) { s.accessibilityId = 600; } else {
                auto scList = settings.GetShortcutActionListRef();
                const float dp = std::max(0.5f, renderer.GetDpiScale());
                const bool scSingleColumn = SettingsLayout::ShortcutsSingleColumn(SettingsContentWidth(dp), dp);
                size_t midSc = scSingleColumn ? scList.size() : (scList.size() + 1) / 2;
                int row = keyboardFocusedIndex, sub = keyboardFocusedSubIndex;
                size_t idx = (sub == 0) ? static_cast<size_t>(row - 1) : (midSc + static_cast<size_t>(row - 1));
                if (idx < scList.size()) {
                    s.entityId = scList[idx].first;
                    s.accessibilityId = 600 + static_cast<LONG>(idx) + 1;
                } else { s.accessibilityId = 600; }
            }
        } else { s.accessibilityId = 600 + std::max(0, keyboardFocusedIndex); }
    }
    return s;
}

void App::PushFocusSnapshot(TransientSurfaceType surfaceType) {
    if (surfaceType == TransientSurfaceType::Modal && !focusRestoreStack.empty()) {
        if (focusRestoreStack.back().surfaceType == TransientSurfaceType::ContextMenu) {
            FocusSnapshot inherited = focusRestoreStack.back();
            inherited.surfaceType = TransientSurfaceType::Modal;
            if (focusRestoreStack.size() >= kMaxFocusStackDepth) { focusRestoreStack.erase(focusRestoreStack.begin()); }
            focusRestoreStack.push_back(std::move(inherited));
            return;
        }
    }
    if (focusRestoreStack.size() >= kMaxFocusStackDepth) { focusRestoreStack.erase(focusRestoreStack.begin()); }
    FocusSnapshot s = CaptureCurrentFocus(surfaceType);
    focusRestoreStack.push_back(std::move(s));
}

bool App::PopAndRestoreFocus(TransientSurfaceType expectedSurfaceType) {
    if (focusRestoreStack.empty()) return false;
    FocusSnapshot snapshot;
    if (expectedSurfaceType == TransientSurfaceType::None) { snapshot = focusRestoreStack.back(); focusRestoreStack.pop_back(); } else {
        bool found = false;
        for (auto it = focusRestoreStack.rbegin(); it != focusRestoreStack.rend(); ++it) {
            if (it->surfaceType == expectedSurfaceType) {
                snapshot = *it;
                auto forwardIt = (it + 1).base();
                focusRestoreStack.erase(forwardIt, focusRestoreStack.end());
                found = true;
                break;
            }
        }
        if (!found) { snapshot = focusRestoreStack.back(); focusRestoreStack.pop_back(); }
    }
    RestoreLogicalFocus(snapshot);
    return true;
}

void App::ActivateCreatedPlaylist(const std::string& playlistId) {
    if (playlistId.empty()) return;
    currentTab = TabView::Playlists;
    activePlaylistId = playlistId;
    activeFocusPane = FocusPane::PlaylistRail;
    playlistDetailScrollY = 0.0f;
    playlistSearchInput.SetText(u"");
    selectedPlaylistIndices.clear();
    selectedEpisodeIndices.clear();
    selectionAnchorIndex = -1;
    keyboardFocusedIndex = -1;
    LONG targetAccessibilityId = 0;
    for (size_t i = 0; i < library.playlists.size(); ++i) {
        if (library.playlists[i].id == playlistId) {
            keyboardFocusedIndex = static_cast<int>(i);
            selectedPlaylistIndices.insert(i);
            targetAccessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(playlistId));
            break;
        }
    }
    MarkAccessibilityDirty();
    RefreshAccessibilityTree(true);
    if (targetAccessibilityId > 0) { accessibilityTree.SetFocusedNode(targetAccessibilityId); }
    InvalidateRect(hWnd, nullptr, FALSE);
}

void App::RestoreLogicalFocus(const FocusSnapshot& snapshot) {
    if (!hWnd || shutdownComplete) return;
    LONG targetAccessibilityId = snapshot.accessibilityId;
    if (snapshot.playerActive && playbackSession.IsOpen()) {
        if (snapshot.accessibilityId >= 9001 && snapshot.accessibilityId <= 9026) {
            playerKeyboardFocusId = snapshot.accessibilityId;
            isKeyboardNavActive = true;
        }
        if (snapshot.playerButtonId >= 0) { hoveredPlayerButtonId = snapshot.playerButtonId; }
        if (snapshot.accessibilityId > 0) { targetAccessibilityId = snapshot.accessibilityId; }
    } else {
        currentTab = snapshot.tab;
        activeFocusPane = snapshot.pane;
        switch (activeFocusPane) {
        case FocusPane::TopNavBar: {
            topNavFocusedIndex = std::clamp(snapshot.topNavIndex, 0, 4);
            keyboardFocusedIndex = -1;
            targetAccessibilityId = 100 + topNavFocusedIndex;
            break;
        }
        case FocusPane::MediaList: {
            const auto& visible = GetVisibleMediaIndices();
            if (!visible.empty()) {
                int targetIdx = -1;
                if (!snapshot.entityPath.empty()) {
                    for (size_t i = 0; i < visible.size(); ++i) {
                        if (visible[i] < library.mediaItems.size() && library.mediaItems[visible[i]].filePath == snapshot.entityPath) {
                            targetIdx = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (targetIdx >= 0) {
                    keyboardFocusedIndex = targetIdx;
                    selectedMediaIndex = static_cast<int>(visible[targetIdx]);
                    targetAccessibilityId = AccessibilityStableId(1000, snapshot.entityPath);
                } else {
                    int fallbackIdx = std::clamp(snapshot.keyboardIndex, 0, static_cast<int>(visible.size()) - 1);
                    keyboardFocusedIndex = fallbackIdx;
                    selectedMediaIndex = static_cast<int>(visible[fallbackIdx]);
                    if (visible[fallbackIdx] < library.mediaItems.size())
                        targetAccessibilityId = AccessibilityStableId(1000, library.mediaItems[visible[fallbackIdx]].filePath);
                }
                AutoScrollToFocusedItem();
            } else {
                keyboardFocusedIndex = std::clamp(snapshot.keyboardIndex, 0, 1);
                selectedMediaIndex = -1;
                targetAccessibilityId = 1081 + keyboardFocusedIndex;
            }
            break;
        }
        case FocusPane::PlaylistRail: {
            if (!library.playlists.empty()) {
                int targetIdx = -1;
                if (!snapshot.entityId.empty()) {
                    for (size_t i = 0; i < library.playlists.size(); ++i) {
                        if (library.playlists[i].id == snapshot.entityId) { targetIdx = static_cast<int>(i); break; }
                    }
                }
                if (targetIdx >= 0) {
                    keyboardFocusedIndex = targetIdx;
                    activePlaylistId = library.playlists[targetIdx].id;
                    targetAccessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId));
                } else {
                    int fallbackIdx = std::clamp(snapshot.keyboardIndex, 0, static_cast<int>(library.playlists.size()) - 1);
                    keyboardFocusedIndex = fallbackIdx;
                    activePlaylistId = library.playlists[fallbackIdx].id;
                    targetAccessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId));
                }
                AutoScrollToFocusedItem();
            } else { keyboardFocusedIndex = 0; activePlaylistId.clear(); targetAccessibilityId = 3000; }
            break;
        }
        case FocusPane::PlaylistDetail: {
            Playlist* pl = library.FindPlaylist(activePlaylistId.empty() ? snapshot.entityId : activePlaylistId);
            if (pl) {
                activePlaylistId = pl->id;
                if (snapshot.textInputField == TextInputField::PlaylistSearch) {
                    FocusTextInput(TextInputField::PlaylistSearch);
                    targetAccessibilityId = 3050;
                } else if (!pl->itemPaths.empty()) {
                    const auto& visible = GetVisiblePlaylistIndices();
                    int targetIdx = -1;
                    if (!snapshot.entityPath.empty()) {
                        for (size_t i = 0; i < visible.size(); ++i) {
                            if (visible[i] < pl->itemPaths.size() && pl->itemPaths[visible[i]] == snapshot.entityPath) {
                                targetIdx = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (targetIdx >= 0) {
                        keyboardFocusedIndex = targetIdx;
                        targetAccessibilityId = AccessibilityStableId(3200, snapshot.entityPath);
                    } else {
                        int fallbackIdx = std::clamp(snapshot.keyboardIndex, 0, static_cast<int>(visible.size()) - 1);
                        keyboardFocusedIndex = fallbackIdx;
                        if (fallbackIdx >= 0 && fallbackIdx < static_cast<int>(visible.size()) && visible[fallbackIdx] < pl->itemPaths.size())
                            targetAccessibilityId = AccessibilityStableId(3200, pl->itemPaths[visible[fallbackIdx]]);
                    }
                    AutoScrollToFocusedItem();
                } else {
                    keyboardFocusedIndex = 0;
                    targetAccessibilityId = (!pl->IsSmart() && snapshot.accessibilityId == 3056) ? 3056 : 3050;
                }
            } else {
                activeFocusPane = FocusPane::PlaylistRail;
                if (!library.playlists.empty()) {
                    int fallbackIdx = std::clamp(snapshot.keyboardIndex, 0, static_cast<int>(library.playlists.size()) - 1);
                    keyboardFocusedIndex = fallbackIdx;
                    activePlaylistId = library.playlists[fallbackIdx].id;
                    targetAccessibilityId = AccessibilityStableId(3001, Utf8ToUtf16(activePlaylistId));
                } else { keyboardFocusedIndex = 0; activePlaylistId.clear(); targetAccessibilityId = 3000; }
            }
            break;
        }
        case FocusPane::HistoryList: {
            if (!library.history.empty()) {
                int targetIdx = -1;
                if (!snapshot.entityPath.empty()) {
                    for (size_t i = 0; i < library.history.size(); ++i) {
                        if (library.history[i].filePath == snapshot.entityPath) { targetIdx = static_cast<int>(i); break; }
                    }
                }
                if (targetIdx >= 0) {
                    keyboardFocusedIndex = targetIdx;
                    targetAccessibilityId = AccessibilityStableId(4000, snapshot.entityPath);
                } else {
                    int fallbackIdx = std::clamp(snapshot.keyboardIndex, 0, static_cast<int>(library.history.size()) - 1);
                    keyboardFocusedIndex = fallbackIdx;
                    if (fallbackIdx >= 0 && fallbackIdx < static_cast<int>(library.history.size()))
                        targetAccessibilityId = AccessibilityStableId(4000, library.history[fallbackIdx].filePath);
                }
                AutoScrollToFocusedItem();
            } else { keyboardFocusedIndex = 0; targetAccessibilityId = 4081; }
            break;
        }
        case FocusPane::SettingsCategoryList: {
            keyboardFocusedIndex = std::clamp(snapshot.keyboardIndex, 0, 7);
            currentSettingsCat = static_cast<SettingsCategory>(keyboardFocusedIndex);
            hoveredSettingsRow = -1;
            targetAccessibilityId = 500 + keyboardFocusedIndex;
            break;
        }
        case FocusPane::SettingsContent: {
            if (currentSettingsCat == SettingsCategory::Shortcuts) {
                auto scList = settings.GetShortcutActionListRef();
                const float dp = std::max(0.5f, renderer.GetDpiScale());
                const bool scSingleColumn = SettingsLayout::ShortcutsSingleColumn(SettingsContentWidth(dp), dp);
                size_t midSc = scSingleColumn ? scList.size() : (scList.size() + 1) / 2;
                if (snapshot.keyboardIndex == 0) {
                    keyboardFocusedIndex = 0;
                    keyboardFocusedSubIndex = 0;
                    targetAccessibilityId = 600;
                } else {
                    int targetRow = snapshot.keyboardIndex, targetSub = snapshot.keyboardSubIndex;
                    if (!snapshot.entityId.empty()) {
                        for (size_t i = 0; i < scList.size(); ++i) {
                            if (scList[i].first == snapshot.entityId) {
                                targetRow = (i < midSc) ? static_cast<int>(i + 1) : static_cast<int>(i - midSc + 1);
                                targetSub = (i < midSc) ? 0 : 1;
                                break;
                            }
                        }
                    }
                    keyboardFocusedIndex = std::clamp(targetRow, 0, static_cast<int>(midSc));
                    keyboardFocusedSubIndex = std::clamp(targetSub, 0, 1);
                    size_t actualIdx = (keyboardFocusedSubIndex == 1) ? (midSc + keyboardFocusedIndex - 1) : static_cast<size_t>(keyboardFocusedIndex - 1);
                    targetAccessibilityId = 600 + static_cast<LONG>(actualIdx) + 1;
                }
            } else {
                const bool simpleSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Simple;
                const bool customSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Custom;
                int rowCount = (currentSettingsCat == SettingsCategory::Appearance) ? 2 :
                               (currentSettingsCat == SettingsCategory::Playback) ? SettingsLayout::PlaybackRowCount(simpleSettings) :
                               (currentSettingsCat == SettingsCategory::Subtitles) ? 6 :
                               (currentSettingsCat == SettingsCategory::Experience) ? (customSettings ? static_cast<int>(FeatureGate::Catalog().size()) + 1 : 1) :
                               (currentSettingsCat == SettingsCategory::System) ? SettingsLayout::SystemRowCount(simpleSettings) : 1;
                keyboardFocusedIndex = std::clamp(snapshot.keyboardIndex, 0, std::max(0, rowCount - 1));
                keyboardFocusedSubIndex = snapshot.keyboardSubIndex;
                targetAccessibilityId = 600 + keyboardFocusedIndex;
            }
            AutoScrollToFocusedItem();
            break;
        }
        }
        isKeyboardNavActive = true;
    }
    accessibilityDirty = true;
    RefreshAccessibilityTree(true);
    if (targetAccessibilityId > 0) { accessibilityTree.SetFocusedNode(targetAccessibilityId); }
    InvalidateRect(hWnd, nullptr, FALSE);
    if (playbackSession.IsOpen()) { Render(); }
}

void App::DismissActiveModal(bool confirmed) {
    if (activeModal == ModalType::None) return;
    const auto onConfirm = modalOnConfirm;
    const auto onCancel = modalOnCancel;
    activeModal = ModalType::None;
    modalFocusOwner = ModalType::None;
    modalKeyboardFocus = 0;
    renamePlaylistId.clear();
    editingSmartPlaylistId.clear();
    modalOnCancel = nullptr;
    modalOnConfirm = nullptr;
    BlurTextInput();
    if (confirmed) { if (onConfirm) onConfirm(); } else { if (onCancel) onCancel(); }
    const bool modalSnapshotStillPending = std::any_of( focusRestoreStack.begin(), focusRestoreStack.end(),
        [](const FocusSnapshot& snapshot) { return snapshot.surfaceType == TransientSurfaceType::Modal; });
    if (modalSnapshotStillPending) { PopAndRestoreFocus(TransientSurfaceType::Modal); }
}

bool App::ArePopupsOpen() const { return isSettingsFlyoutOpen || isContextMenuOpen; }

void App::RequestRender() { Render(); }

void App::TogglePlaybackDiagnostics() { showPlaybackDiagnostics = !showPlaybackDiagnostics; RequestRender(); }

bool App::IsPlaybackDiagnosticsVisible() const { return showPlaybackDiagnostics; }

void App::ToggleLogViewer() {
    showLogViewer = !showLogViewer;
    if (showLogViewer) {
        logViewerLines.clear();
        // Same path Diagnostics::Log resolves and writes: <exeDir>\diagnostics\aperture.log
        // (rotated at 1 MB, so the file never exceeds a small tail read).
        const std::wstring logPath = GetExeDir() + L"\\diagnostics\\aperture.log";
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath.c_str(), L"rb") == 0 && f) {
            if (fseek(f, 0, SEEK_END) == 0) {
                const long fileSize = ftell(f);
                const long kTailBytes = 64 * 1024;
                long readSize = fileSize > 0 ? fileSize : 0;
                bool skippedHead = false;
                if (readSize > kTailBytes) {
                    fseek(f, readSize - kTailBytes, SEEK_SET);
                    readSize = kTailBytes;
                    skippedHead = true;
                } else if (fseek(f, 0, SEEK_SET) != 0) { readSize = 0; }
                if (readSize > 0) {
                    std::string utf8(static_cast<size_t>(readSize), '\0');
                    utf8.resize(fread(utf8.data(), 1, utf8.size(), f));
                    // When the head was skipped, drop the partial first line.
                    if (skippedHead) {
                        const size_t newline = utf8.find('\n');
                        utf8.erase(0, newline == std::string::npos ? utf8.size() : newline + 1);
                    }
                    size_t start = 0;
                    while (start < utf8.size()) {
                        size_t end = utf8.find('\n', start);
                        if (end == std::string::npos) end = utf8.size();
                        std::string line = utf8.substr(start, end - start);
                        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                        if (!line.empty()) logViewerLines.push_back(Utf8ToUtf16(line));
                        start = end + 1;
                    }
                }
            }
            fclose(f);
        }
        // Newest entries last, like a terminal tail; cap at the last 30 lines.
        if (logViewerLines.size() > 30) { logViewerLines.erase(logViewerLines.begin(), logViewerLines.end() - 30); }
        if (logViewerLines.empty()) { logViewerLines.push_back(L"(no log entries yet)"); }
    }
    RequestRender();
}

bool App::IsLogViewerCloseHit(float x, float y) const {
    if (!showLogViewer || !playbackSession.IsOpen() || isContextMenuOpen || activeModal != ModalType::None) return false;
    RECT bounds{};
    if (!GetClientRect(hWnd, &bounds)) return false;
    const auto layout = Presentation::LogViewer(D2D1::RectF(0, 0, static_cast<float>(bounds.right),
        static_cast<float>(bounds.bottom)), overlayRenderer.GetDpiScale());
    return layout.visible && PtInRectF(layout.close, x, y);
}

void App::ApplyAlwaysOnTop() {
    // PiP manages its own TOPMOST state (see PlaybackSession::TogglePiPPinned).
    if (!hWnd || playbackSession.IsPiP()) return;
    SetWindowPos(hWnd, settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::ToggleAlwaysOnTop() {
    settings.alwaysOnTop = !settings.alwaysOnTop;
    ApplyAlwaysOnTop();
    SaveSettings();
    ShowOSD(IconType::Pin, settings.alwaysOnTop ? L"Always on Top: On" : L"Always on Top: Off");
}

namespace {
// Classic WorkerW wallpaper host: the top-level window that hosts the shell's
// SHELLDLL_DefView (Progman before message 0x052C, a WorkerW on some shells),
// followed by the WorkerW created behind it for the wallpaper surface.
struct WallpaperWorkerLookup { HWND worker = nullptr; };
BOOL CALLBACK FindWallpaperWorkerWProc(HWND hwnd, LPARAM lParam) {
    if (!FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
    WallpaperWorkerLookup* lookup = reinterpret_cast<WallpaperWorkerLookup*>(lParam);
    lookup->worker = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
    return FALSE;
}
} // namespace

void App::SetWallpaperMode(bool enabled) {
    if (!hWnd || !hVideoWnd) return;
    if (!enabled) {
        if (wallpaperModeActive) {
            DisableWallpaperMode();
            ShowOSD(IconType::MediaFilm, L"Wallpaper Mode: Off");
        }
        return;
    }
    if (wallpaperModeActive) return;
    if (!playbackSession.IsOpen() || IsPiP() || IsFullscreen()) {
        ShowOSD(IconType::MediaFilm, L"Wallpaper mode needs normal playback");
        return;
    }
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) { ShowOSD(IconType::MediaFilm, L"Wallpaper mode unavailable"); return; }
    // Ask Progman to spawn the wallpaper WorkerW behind its shell view.
    DWORD_PTR spawnResult = 0;
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &spawnResult);
    WallpaperWorkerLookup lookup;
    EnumWindows(FindWallpaperWorkerWProc, reinterpret_cast<LPARAM>(&lookup));
    HWND workerW = lookup.worker;
    if (!workerW) { ShowOSD(IconType::MediaFilm, L"Wallpaper mode unavailable"); return; }
    // Remember the child's owner and its placement in that owner's client
    // coordinates before the reparent (SetParent keeps the screen rect).
    HWND originalParent = GetParent(hVideoWnd);
    RECT childRect{};
    if (!originalParent || !GetWindowRect(hVideoWnd, &childRect)) {
        ShowOSD(IconType::MediaFilm, L"Wallpaper mode unavailable");
        return;
    }
    MapWindowPoints(NULL, originalParent, reinterpret_cast<POINT*>(&childRect), 2);
    wallpaperOriginalParent = originalParent;
    wallpaperOriginalChildRect = childRect;
    if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
    if (!SetParent(hVideoWnd, workerW)) {
        wallpaperOriginalParent = NULL;
        wallpaperOriginalChildRect = { 0, 0, 0, 0 };
        if (playbackSession.IsOpen() && hOverlayWnd) ShowWindow(hOverlayWnd, SW_SHOW);
        ShowOSD(IconType::MediaFilm, L"Wallpaper mode unavailable");
        return;
    }
    wallpaperModeActive = true;
    // Cover the monitor the main window is on.
    HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    RECT cover = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(monitor, &monitorInfo)) cover = monitorInfo.rcMonitor;
    SetWindowPos(hVideoWnd, NULL, cover.left, cover.top,
        std::max(1L, cover.right - cover.left), std::max(1L, cover.bottom - cover.top),
        SWP_NOZORDER | SWP_SHOWWINDOW);
    player.SetVideoBounds(0, 0, (int)(cover.right - cover.left), (int)(cover.bottom - cover.top));
    // Keep the main window minimized so the taskbar stays usable.
    ShowWindow(hWnd, SW_MINIMIZE);
    ShowOSD(IconType::MediaFilm, L"Wallpaper Mode: On");
}

void App::DisableWallpaperMode() {
    if (!wallpaperModeActive || !hVideoWnd) return;
    wallpaperModeActive = false;
    const HWND originalParent = wallpaperOriginalParent;
    const RECT childRect = wallpaperOriginalChildRect;
    wallpaperOriginalParent = NULL;
    wallpaperOriginalChildRect = { 0, 0, 0, 0 };
    if (originalParent && IsWindow(originalParent) && GetParent(hVideoWnd) != originalParent) {
        SetParent(hVideoWnd, originalParent);
        // Restore the saved placement, then re-apply the current video surface
        // layout: the WM_SIZE path positions the child at the main window's
        // client size and keeps mpv's video bounds in sync with it.
        SetWindowPos(hVideoWnd, NULL, childRect.left, childRect.top,
            std::max(1L, childRect.right - childRect.left), std::max(1L, childRect.bottom - childRect.top),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT rc{};
        if (hWnd && GetClientRect(hWnd, &rc)) {
            SetWindowPos(hVideoWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
            player.SetVideoBounds(0, 0, rc.right - rc.left, rc.bottom - rc.top);
        }
    }
    // Do not fight the close path: BeginClose force-disables this mode and
    // hides the window itself right after.
    if (!closeRequested && hWnd && IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
    if (playbackSession.IsOpen() && hOverlayWnd) { ShowWindow(hOverlayWnd, SW_SHOW); SyncOverlayPosition(); }
    InvalidateRect(hWnd, NULL, FALSE);
}

// --- Jump list (recent media) publication -----------------------------------
// History is stored newest-first (MediaLibrary::AddToHistory inserts at the
// front), so the top entries are the most recently played paths.
std::vector<std::wstring> App::RecentMediaPathsForJumpList(const std::vector<HistoryRecord>& history, size_t maxCount) {
    std::vector<std::wstring> paths;
    paths.reserve(std::min(history.size(), maxCount));
    for (const auto& record : history) {
        if (paths.size() >= maxCount) break;
        if (!record.filePath.empty()) paths.push_back(record.filePath);
    }
    return paths;
}

void App::MarkJumpListDirty() {
    // Private mode leaves no traces, including OS jump list publication.
    if (!settings.jumpListEnabled || IsPrivateMode()) return;
    jumpListDirty = true;
}

void App::PumpJumpListUpdate(bool force) {
    if (IsPrivateMode()) return;
    if (!settings.jumpListEnabled) {
        if (force) UpdateJumpList({}); // remove the published category
        jumpListDirty = false;
        return;
    }
    if (!libraryReady) return;
    if (!force && GetTickCount64() - lastJumpListPublishMs < kJumpListRebuildMinIntervalMs) return;
    jumpListDirty = false;
    lastJumpListPublishMs = GetTickCount64();
    UpdateJumpList(RecentMediaPathsForJumpList(library.history, 10));
}

const char* const App::kGlobalHotkeyActions[App::kGlobalHotkeyCount] = { "togglePlay", "playNext", "playPrevious", "toggleMute" };

int App::ApplyGlobalHotkeys() {
    // Always clear all four first so changed or disabled bindings never linger.
    if (hWnd) {
        for (UINT i = 0; i < kGlobalHotkeyCount; ++i) { UnregisterHotKey(hWnd, kGlobalHotkeyBase + i); }
    }
    int registered = 0;
    if (settings.globalHotkeysEnabled && hWnd) {
        for (UINT i = 0; i < kGlobalHotkeyCount; ++i) {
            auto it = settings.shortcuts.find(kGlobalHotkeyActions[i]);
            if (it == settings.shortcuts.end()) continue;
            const ShortcutBinding& b = it->second;
            // Skip unassigned bindings, and skip chords with NO modifier: a bare
            // global key would be swallowed system-wide while other apps are focused.
            if (b.vkCode == 0 || (!b.ctrl && !b.alt && !b.shift)) continue;
            const UINT mods = (b.ctrl ? MOD_CONTROL : 0u) | (b.alt ? MOD_ALT : 0u) | (b.shift ? MOD_SHIFT : 0u);
            if (RegisterHotKey(hWnd, kGlobalHotkeyBase + i, mods, b.vkCode)) { ++registered; }
        }
    }
    registeredGlobalHotkeys = registered;
    return registered;
}

void App::UpdateTrayIcon() {
    if (!hWnd || !hInstance) return;
    if (!settings.trayIconEnabled) { RemoveTrayIcon(); return; }
    if (!hTrayIcon) {
        // Same icon resource the main window class uses (IDI_APP_ICON, see main.cpp).
        hTrayIcon = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    }
    if (!hTrayIcon) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hWnd;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = hTrayIcon;
    const bool playing = playbackSession.IsOpen() && player.IsPlaying();
    wcscpy_s(data.szTip, playing ? L"Aperture \u2014 Playing" : L"Aperture \u2014 Paused");
    trayIconData = data;
    if (trayIconAdded) {
        Shell_NotifyIconW(NIM_MODIFY, &trayIconData);
    } else {
        trayIconAdded = Shell_NotifyIconW(NIM_ADD, &trayIconData) != FALSE;
    }
}

void App::RemoveTrayIcon() {
    if (!trayIconAdded || !hWnd) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hWnd;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded = false;
}

void App::RestoreFromTray() {
    if (!hWnd) return;
    isMinimizedToTray = false;
    if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) { ShowWindow(hWnd, SW_RESTORE); }
    SetForegroundWindow(hWnd);
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::ShowTrayContextMenu(int screenX, int screenY) {
    if (!trayIconAdded || !hWnd) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    // Native popup menu: the tray lives outside the custom-drawn surface, so a
    // plain Win32 menu is the platform convention here.
    AppendMenuW(menu, MF_STRING, 1, L"Play/Pause");
    AppendMenuW(menu, MF_STRING, 2, L"Next");
    AppendMenuW(menu, MF_STRING, 3, L"Restore");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Exit Aperture");
    // TrackPopupMenu requires its owner to be foreground or it will not dismiss.
    SetForegroundWindow(hWnd);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenX, screenY, 0, hWnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    switch (cmd) {
        case 1: commandDispatcher.Execute(AppCommand::TogglePlay); break;
        case 2: commandDispatcher.Execute(AppCommand::PlayNext); break;
        case 3: RestoreFromTray(); break;
        case 4: BeginClose(); break;
        default: break;
    }
}

bool App::TryExecutePlayerMouseBinding(UINT vkCode) {
    if (!playbackSession.IsOpen()) return false;
    const bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool isAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const AppCommand cmd = commandDispatcher.FindMouseBinding(vkCode, isCtrl, isShift, isAlt, settings);
    if (cmd == AppCommand::None) return false;
    commandDispatcher.Execute(cmd);
    return true;
}

App::~App() { Shutdown(); }

bool App::IsReducedMotionEnabled() const { return settings.reduceMotionEnabled || systemVisualPreferences.Get().reduceMotion; }

// ---- Listen island playback (radio / podcast audio, hub stays visible) ----

void App::StartListenPlayback(const std::wstring& url, const std::wstring& title, const std::wstring& subtitle,
                              bool isRadio, double startSec) {
    if (url.empty()) return;
    if (!listenPlaybackActive && listenIslandFade.current <= 0.01f) {
        const float dp = std::max(0.5f, renderer.GetDpiScale());
        const float initW = std::max(220.0f, RectWidth(ListenPillBounds(dp)) / dp);
        listenIslandWidthDip.SnapTo(initW);
        listenIslandHeightDip.SnapTo(36.0f);
        listenIslandRadiusDip.SnapTo(18.0f);
        listenIslandFade.SnapTo(0.0f);
        listenIslandExpand.SnapTo(0.0f);
    }
    listenPlaybackActive = true;
    listenPlaybackIsRadio = isRadio;
    listenPlaybackPaused = false;
    listenTitle = title;
    listenSubtitle = subtitle;
    listenUrl = url;
    InvalidateRect(hWnd, NULL, FALSE);
    if (!listenPlayerInitialized) {
        if (!listenPlayer.Initialize(playerInitialized ? player.GetMpvDll() : nullptr)) {
            listenPlaybackPaused = true;
            ShowOSD(IconType::VolumeMuted, L"Could not start audio playback");
            return;
        }
        listenPlayerInitialized = true;
    }
    const double effectiveVol = settings.muted ? 0.0 : std::clamp(settings.volume, 0.0, 1.0);
    listenPlayer.SetVolume(effectiveVol);
    listenVolumeApplied = effectiveVol;
    if (!listenPlayer.Start(url, startSec, effectiveVol)) {
        listenPlaybackPaused = true;
        ShowOSD(IconType::VolumeMuted, L"Could not start audio playback");
        return;
    }
}

void App::StopListenPlayback() {
    if (!listenPlaybackActive) { listenPlayer.Stop(); return; }
    listenPlayer.Stop();
    listenPlaybackActive = false;
    listenPlaybackPaused = false;
    listenPinExpanded = false;
    listenPillHovered = false;
    listenBadgeHovered = false;
    listenMuteHovered = false;
    listenVolumeHovered = false;
    listenSeekHovered = false;
    listenXHovered = false;
    isListenVolumeDragging = false;
    isListenSeekDragging = false;
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::ToggleListenPause() {
    if (!listenPlaybackActive || !listenPlayer.IsActive()) return;
    listenPlaybackPaused = !listenPlaybackPaused;
    listenPlayer.SetPaused(listenPlaybackPaused);
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::ListenSeekBy(double deltaSec) {
    if (!listenPlaybackActive || listenPlaybackIsRadio) return;
    const double pos = listenPlayer.GetPositionSec();
    const double dur = listenPlayer.GetDurationSec();
    if (pos < 0.0) return;
    double target = std::max(0.0, pos + deltaSec);
    if (dur > 0.0) target = std::min(target, std::max(0.0, dur - 1.0));
    listenPlayer.SeekAbsolute(target);
    InvalidateRect(hWnd, NULL, FALSE);
}

// Episode finished: podcasts chain to the next episode of the same feed;
// live radio streams that end simply stop.
void App::OnListenFinished() {
    if (!listenPlaybackIsRadio) {
        for (const auto& feed : listenFeeds) {
            for (size_t i = 0; i + 1 < feed.episodes.size(); ++i) {
                if (feed.episodes[i].url == listenUrl) {
                    const auto& next = feed.episodes[i + 1];
                    ShowOSD(IconType::AutoplayNext, L"Next episode");
                    StartListenPlayback(next.url, next.title.empty() ? L"Untitled episode" : next.title,
                                        feed.title.empty() ? L"Podcast" : feed.title, false, 0.0);
                    return;
                }
            }
        }
    }
    StopListenPlayback();
    ShowOSD(IconType::Checkmark, listenPlaybackIsRadio ? L"Stream ended" : L"Episode finished");
}

D2D1_RECT_F App::ListenPillBounds(float dp) {
    RECT rc{};
    if (!hWnd || !GetClientRect(hWnd, &rc)) return D2D1::RectF(0, 0, 0, 0);
    const float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    const auto nav = Presentation::Nav(winW, dp, false, [this](const std::wstring& t) {
        return renderer.MeasureString(t, FontType::Medium).width;
    });
    const float cy = nav.tabY + nav.tabH * 0.5f;
    const float h = 36.0f * dp;
    // Positioned directly next to the History tab (nav.track.right), expanding rightward.
    const float left = nav.track.right + 12.0f * dp;
    const float rightLimit = ((importIslandPhase != 0 || importIslandFade.current > 0.001f)
        ? ImportPillBounds(dp).left
        : nav.settings.left) - 10.0f * dp;
    const float avail = std::max(220.0f * dp, rightLimit - left);
    const float w = std::clamp(340.0f * dp, 220.0f * dp, avail);
    return D2D1::RectF(left, cy - h * 0.5f, left + w, cy + h * 0.5f);
}

D2D1_RECT_F App::ImportPillBounds(float dp) {
    RECT rc{};
    if (!hWnd || !GetClientRect(hWnd, &rc)) return D2D1::RectF(0, 0, 0, 0);
    const float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    const auto nav = Presentation::Nav(winW, dp, false, [this](const std::wstring& t) {
        return renderer.MeasureString(t, FontType::Medium).width;
    });
    const float w = std::max(136.0f * dp, ImportIslandTargetWidthDip() * dp);
    const float h = 32.0f * dp;
    const float right = nav.settings.left - 10.0f * dp;
    const float cy = nav.tabY + nav.tabH * 0.5f;
    return D2D1::RectF(right - w, cy - h * 0.5f, right, cy + h * 0.5f);
}

// Content width of the island pill in DIP. Measured from the total digit width
// (both sides of the slash) and floored at 136 DIP (ai_studio_code(5).html) so
// per-file counter increments never change or jitter the pill width.
float App::ImportIslandTargetWidthDip() {
    const size_t total = importTotal.load(std::memory_order_acquire);
    const std::wstring maxLabel = total > 0
        ? (std::to_wstring(total) + L" / " + std::to_wstring(total))
        : std::wstring(L"000 / 000");
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    const float textDip = renderer.MeasureString(maxLabel, FontType::MonoSmall).width / dp;
    const float iconSz = 15.0f, gap = 10.0f, pad = 13.0f;
    return std::max(136.0f, iconSz + gap + textDip + pad * 2.0f);
}

// Expanded island card: grows down and left from the compact pill's top-right
// corner, clamped to 304x82 DIP (ai_studio_code(5).html).
D2D1_RECT_F App::IslandExpandedRect(const D2D1_RECT_F& compact, float dp) const {
    const float w = std::min(304.0f * dp, std::max(180.0f * dp, compact.right - 12.0f * dp));
    return D2D1::RectF(compact.right - w, compact.top, compact.right, compact.top + 82.0f * dp);
}

void App::AnimateVisual(AnimatedFloat& animation, float target, float duration, const CubicBezier* curve) {
    if (IsReducedMotionEnabled()) animation.SnapTo(target);
    else animation.AnimateTo(target, duration, curve);
}

void App::SetNavHoverTargets(bool media, bool playlists, bool history, bool settings, bool openFile) {
    auto animate = [&](AnimatedFloat& anim, bool on) {
        const float target = on ? 1.0f : 0.0f;
        if (std::abs(anim.to - target) > 0.001f) { AnimateVisual(anim, target, MotionTokens::kFast, MotionTokens::Standard()); }
    };
    animate(navMediaTabHover, media);
    animate(navPlaylistTabHover, playlists);
    animate(navHistoryTabHover, history);
    animate(navSettingsTabHover, settings);
    animate(navOpenFileLift, openFile);
}

void App::ClearPlaylistCardHover() {
    for (auto& pair : playlistCardExpands) {
        if (pair.second.current > 0.0f || pair.second.animating) {
            AnimateVisual(pair.second, 0.0f, MotionTokens::kResponsive, MotionTokens::Standard());
        }
    }
    playlistCardHoverIndex = -1;
    playlistCardHoverEnterTime = -1.0f;
}

void App::SnapVisualAnimations() {
    if (!IsReducedMotionEnabled()) return;
    // A preference can change while an animation is in flight. Resolve every retained visual state to its already-selected endpoint so the
    // next frame cannot paint a stale half-transition. This is O(number of persistent animations), allocation-free, and does not affect
    // input or preview playback.
    navMediaTabHover.SnapTo(navMediaTabHover.to);
    navPlaylistTabHover.SnapTo(navPlaylistTabHover.to);
    navHistoryTabHover.SnapTo(navHistoryTabHover.to);
    navSettingsTabHover.SnapTo(navSettingsTabHover.to);
    navSettingsRotation.SnapTo(navSettingsRotation.to);
    navOpenFileLift.SnapTo(navOpenFileLift.to);
    navIndicator.SnapTo(navIndicator.to);
    settingsCatIndicator.SnapTo(settingsCatIndicator.to);
    playlistRailIndicator.SnapTo(playlistRailIndicator.to);
    groupModeIndicator.SnapTo(groupModeIndicator.to);
    playlistViewModeIndicator.SnapTo(playlistViewModeIndicator.to);
    contextMenuAnim.SnapTo(contextMenuAnim.to);
    flyoutAnim.SnapTo(flyoutAnim.to);
    resumePreviewAnim.SnapTo(resumePreviewAnim.to);
    timelineHoverAnim.SnapTo(timelineHoverAnim.to);
    autoplayToggleAnim.SnapTo(autoplayToggleAnim.to);
    rememberPosToggleAnim.SnapTo(rememberPosToggleAnim.to);
    hwAccelToggleAnim.SnapTo(hwAccelToggleAnim.to);
    loopToggleAnim.SnapTo(loopToggleAnim.to);
    customMpvConfigToggleAnim.SnapTo(customMpvConfigToggleAnim.to);
    for (auto& anim : experienceToggleAnim) anim.SnapTo(anim.to);
    for (auto& anim : systemRowHoverAnim) anim.SnapTo(anim.to);
    for (auto& anim : systemRowPressAnim) anim.SnapTo(anim.to);
    jumpAheadAnim.SnapTo(jumpAheadAnim.to);
    importIslandWidthDip.SnapTo(importIslandWidthDip.to);
    importIslandHeightDip.SnapTo(importIslandHeightDip.to);
    importIslandRadiusDip.SnapTo(importIslandRadiusDip.to);
    importIslandExpand.SnapTo(importIslandExpand.to);
    importIslandFade.SnapTo(importIslandFade.to);
    listenIslandWidthDip.SnapTo(listenIslandWidthDip.to);
    listenIslandHeightDip.SnapTo(listenIslandHeightDip.to);
    listenIslandRadiusDip.SnapTo(listenIslandRadiusDip.to);
    listenIslandExpand.SnapTo(listenIslandExpand.to);
    listenIslandFade.SnapTo(listenIslandFade.to);
    for (auto& pair : playlistCardExpands) {
        pair.second.SnapTo(pair.second.to);
    }
    for (auto& row : rowAnimStates) {
        row.height.SnapTo(row.height.to);
        row.liftY.SnapTo(row.liftY.to);
        row.previewOpacity.SnapTo(row.previewOpacity.to);
    }
    if (playbackSession.IsOpen()) {
        const bool controlsForcedVisible = isScrubbing || isVolumeScrubbing || isSettingsFlyoutOpen || isContextMenuOpen;
        controlsOpacity = controlsForcedVisible ? 1.0f : (controlsIdleTimer >= (player.IsPlaying() ? 1.2f : 2.0f) ? 0.0f : 1.0f);
    }
}

void App::ApplyExperienceModeSelection(ExperienceMode mode, bool forkFromPreview) {
    const ExperienceMode oldMode = FeatureGate::ModeFromString(settings.experienceMode);
    if (mode == ExperienceMode::Custom && oldMode != ExperienceMode::Custom) {
        // Forking from a preview starts Custom from the surface the user was
        // just looking at. A plain mode switch restores their saved list; only
        // a first-ever visit (empty list) seeds it.
        if (forkFromPreview || settings.customEnabledFeatures.empty()) {
            settings.customEnabledFeatures.clear();
            for (const auto& info : FeatureGate::Catalog()) {
                if (FeatureGate::FeatureEnabled(oldMode, settings.customEnabledFeatures, info.id)) {
                    settings.customEnabledFeatures.push_back(info.key);
                }
            }
        }
    }
    settings.experienceMode = FeatureGate::ModeToString(mode);
    SaveSettings();
}

// Re-resolves the App-side tracked hover state for the Settings tab (sidebar
// category, System rows, shortcuts list). Render-time pointerIn hovers do not
// need this, but every tracked hover must be re-resolved whenever rows move
// under a stationary cursor (wheel scrolling) — not only on WM_MOUSEMOVE.
// Playlist-tab hover resolution, shared by WM_MOUSEMOVE and the wheel path so
// scrolling under a stationary cursor never drops the resume-card preview.
void App::ResolvePlaylistHover(float x, float y) {
    if (currentTab != TabView::Playlists) return;
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    const float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    const D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
    D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
    D2D1_RECT_F detailBounds = D2D1::RectF(viewBounds.left + railW + 16.0f * dp, viewBounds.top, viewBounds.right, viewBounds.bottom);
    // Rail top + button
    float addBtnSize = 26.0f * dp;
    D2D1_RECT_F addBtnRect = D2D1::RectF(railBounds.right - 14.0f * dp - addBtnSize,
        railBounds.top + (48.0f * dp - addBtnSize) * 0.5f + 4.0f * dp, railBounds.right - 14.0f * dp,
        railBounds.top + (48.0f * dp + addBtnSize) * 0.5f + 4.0f * dp);
    isPlaylistAddHovered = PtInRectF(addBtnRect, x, y);
    // Rail items
    float railClipTop = railBounds.top + 48.0f * dp - 4.0f * dp, railClipBottom = railBounds.bottom - 8.0f * dp;
    float itemH = 52.0f * dp, curY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
    hoveredPlaylistRailIndex = -1;
    for (size_t i = 0; i < library.playlists.size(); ++i) {
        if (y >= railClipTop && y <= railClipBottom && x >= railBounds.left + 8.0f * dp && x <= railBounds.right - 8.0f * dp && y >= curY && y <= curY + itemH) {
            hoveredPlaylistRailIndex = (int)i;
            break;
        }
        curY += itemH + 6.0f * dp;
    }
    // Detail pane items & controls
    Playlist* pl = library.FindPlaylist(activePlaylistId);
    hoveredPlaylistDetailItemIndex = -1;
    hoveredPlaylistDetailDeleteIndex = -1;
    isPlaylistAddFilesHovered = false;
    isPlaylistPlayAllHovered = false;
    isPlaylistDeleteHovered = false;
    isPlaylistResumeHovered = false;
    if (pl) {
        float headerH = 50.0f * dp;
        float paneRight = std::max(detailBounds.left + 120.0f * dp, detailBounds.right - 26.0f * dp);
        float playAllW = 106.0f * dp, playAllH = 32.0f * dp;
        D2D1_RECT_F playAllRect = D2D1::RectF(paneRight - playAllW - 40.0f * dp, detailBounds.top + 14.0f * dp + 6.0f * dp, paneRight - 40.0f * dp, detailBounds.top + 14.0f * dp + 6.0f * dp + playAllH);
        D2D1_RECT_F delPlRect = D2D1::RectF(paneRight - 34.0f * dp, detailBounds.top + 14.0f * dp + 6.0f * dp, paneRight, detailBounds.top + 14.0f * dp + 6.0f * dp + 32.0f * dp);
        isPlaylistDeleteHovered = PtInRectF(delPlRect, x, y);
        const std::vector<std::wstring>& plItemPaths = library.GetPlaylistPathsRef(*pl);
        if (!plItemPaths.empty()) {
            isPlaylistPlayAllHovered = PtInRectF(playAllRect, x, y);
            float resumeH = 112.0f * dp, dCurY = detailBounds.top + 14.0f * dp + headerH + 16.0f * dp;
            D2D1_RECT_F resRect = D2D1::RectF(detailBounds.left, dCurY, paneRight, dCurY + resumeH);
            isPlaylistResumeHovered = PtInRectF(resRect, x, y);
            float tbH = 32.0f * dp, listTop = dCurY + resumeH + 16.0f * dp + tbH + 12.0f * dp;
            if (y >= listTop && y <= detailBounds.bottom) {
                const auto& visiblePlaylist = GetVisiblePlaylistIndices();
                float dItemH = playlistGridView ? 166.0f * dp : 62.0f * dp;
                float dGap = playlistGridView ? 10.0f * dp : 6.0f * dp;
                size_t dColumns = playlistGridView ? (size_t)std::max(1.0f, std::floor((RectWidth(D2D1::RectF(detailBounds.left, listTop, paneRight, detailBounds.bottom)) + dGap) / (190.0f * dp))) : 1;
                float dGridW = playlistGridView ? std::max(120.0f * dp, (RectWidth(D2D1::RectF(detailBounds.left, listTop, paneRight, detailBounds.bottom)) - (dColumns - 1) * dGap) / (float)dColumns) : RectWidth(D2D1::RectF(detailBounds.left, listTop, paneRight, detailBounds.bottom));
                float dRowPitch = dItemH + dGap;
                size_t startIdx = 0;
                size_t endIdx = visiblePlaylist.size();
                float relY = (y - listTop) + playlistDetailScrollY;
                if (relY >= 0.0f) {
                    if (playlistGridView) {
                        int targetRow = static_cast<int>(relY / dRowPitch);
                        float relX = x - detailBounds.left;
                        int targetCol = (relX >= 0.0f) ? static_cast<int>(relX / (dGridW + dGap)) : 0;
                        int targetDisp = targetRow * (int)dColumns + std::max(0, std::min(targetCol, (int)dColumns - 1));
                        startIdx = static_cast<size_t>(std::max<int>(0, targetDisp - 1));
                        endIdx = std::min<size_t>(visiblePlaylist.size(), static_cast<size_t>(targetDisp + 2));
                    } else {
                        int targetDisp = static_cast<int>(relY / dRowPitch);
                        startIdx = static_cast<size_t>(std::max<int>(0, targetDisp - 1));
                        endIdx = std::min<size_t>(visiblePlaylist.size(), static_cast<size_t>(targetDisp + 2));
                    }
                }
                for (size_t displayIndex = startIdx; displayIndex < endIdx; ++displayIndex) {
                    size_t i = visiblePlaylist[displayIndex];
                    float rowY = listTop - playlistDetailScrollY + (playlistGridView ? (displayIndex / dColumns) * dRowPitch : displayIndex * dRowPitch);
                    float colX = playlistGridView ? detailBounds.left + (displayIndex % dColumns) * (dGridW + dGap) : detailBounds.left;
                    D2D1_RECT_F dItemRect = D2D1::RectF(colX, rowY, playlistGridView ? colX + dGridW : paneRight, rowY + dItemH);
                    if (PtInRectF(dItemRect, x, y)) {
                        hoveredPlaylistDetailItemIndex = (int)i;
                        if (!pl->IsSmart() && !playlistGridView) {
                            float delBtnSize = 26.0f * dp;
                            D2D1_RECT_F delBtnRect = D2D1::RectF(dItemRect.right - 32.0f * dp, dItemRect.top + (dItemH - delBtnSize) * 0.5f, dItemRect.right - 6.0f * dp, dItemRect.top + (dItemH + delBtnSize) * 0.5f);
                            if (PtInRectF(delBtnRect, x, y)) hoveredPlaylistDetailDeleteIndex = (int)i;
                        }
                        break;
                    }
                }
            }
        } else if (!pl->IsSmart()) {
            const float dropTop = detailBounds.top + 14.0f * dp + headerH + 16.0f * dp;
            const float dropCenterX = (detailBounds.left + paneRight) * 0.5f;
            const float addFilesW = 118.0f * dp, addFilesH = 34.0f * dp;
            D2D1_RECT_F addFilesRect = D2D1::RectF( dropCenterX - addFilesW * 0.5f, dropTop + 168.0f * dp,
                dropCenterX + addFilesW * 0.5f, dropTop + 168.0f * dp + addFilesH);
            isPlaylistAddFilesHovered = PtInRectF(addFilesRect, x, y);
        }
    }
}

// Listen panel hover resolution (mouse move + wheel re-resolution).
void App::ResolveListenHover(float x, float y) {
    listenHoveredFeed = -1;
    listenHoveredEpisode = -1;
    listenHoveredEpisodeFeed = -1;
    listenHoveredStation = -1;
    listenAddPodcastHovered = PtInRectF(listenAddPodcastBounds, x, y);
    for (size_t i = 0; i < listenEpisodeBounds.size(); ++i) {
        if (PtInRectF(listenEpisodeBounds[i], x, y)) {
            listenHoveredEpisode = (int)i;
            listenHoveredEpisodeFeed = listenEpisodeFeedIndex[i];
            return;
        }
    }
    for (size_t i = 0; i < listenFeedBounds.size(); ++i) {
        if (PtInRectF(listenFeedBounds[i], x, y)) { listenHoveredFeed = (int)i; return; }
    }
    for (size_t i = 0; i < listenStationBounds.size(); ++i) {
        if (PtInRectF(listenStationBounds[i], x, y)) { listenHoveredStation = (int)i; return; }
    }
}

// Shared station/episode playback: validation, recent-stream bookkeeping,
// then the island audio player — never the full-screen session.
void App::PlayListenStream(const std::wstring& url, const std::wstring& title, const std::wstring& subtitle, bool isRadio) {
    NetworkStreamUrl parsed = NetworkStreamValidator::ValidateAndParse(url);
    if (!parsed.isValid) {
        ShowOSD(IconType::VolumeMuted, NetworkStreamValidator::GetSafeErrorMessage(parsed.error, url), 4.0f);
        return;
    }
    if (playbackSession.CanPersistRecentStreams(networkStreamModel.IsRecentStreamsEnabled())) {
        networkStreamModel.AddRecentStream(parsed.rawUrl);
        networkStreamModel.SaveRecentStreams(GetExeDir());
    }
    StartListenPlayback(parsed.rawUrl, title, subtitle, isRadio);
}

void App::ResolveSettingsHover(float x, float y) {
    if (currentTab != TabView::Settings) return;
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    const float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    const D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    float sideW = SettingsLayout::kSidebarWidth * dp, catH = SettingsLayout::kSidebarItemHeight * dp;
    float curCatY = viewBounds.top + SettingsLayout::kSidebarTopInset * dp;
    hoveredSettingsCat = (SettingsCategory)-1;
    hoveredSettingsRow = -1;
    hoveredShortcutIndex = -1;
    isResetShortcutsHovered = false;
    SettingsCategory settingsHoverCats[] = {SettingsCategory::Experience, SettingsCategory::Appearance, SettingsCategory::Playback,
                                            SettingsCategory::Subtitles, SettingsCategory::Shortcuts, SettingsCategory::System};
    for (int i = 0; i < 6; ++i) {
        float cy = curCatY + i * (catH + SettingsLayout::kSidebarItemGap * dp);
        if (x >= viewBounds.left + 8.0f * dp && x <= viewBounds.left + sideW - 8.0f * dp && y >= cy && y <= cy + catH) {
            hoveredSettingsCat = settingsHoverCats[i];
            break;
        }
    }
    D2D1_RECT_F contentBounds = SettingsLayout::ContentBounds(viewBounds, dp);
    float padX = SettingsLayout::kContentPadX * dp;
    const float headerReserve = currentSettingsCat == SettingsCategory::Subtitles
        ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve;
    float contentY = SettingsLayout::ContentTop(contentBounds, dp, settingsScrollY, headerReserve);
    float contentW = RectWidth(contentBounds) - padX * 2.0f;
    if (currentSettingsCat == SettingsCategory::System) {
        hoveredSettingsRow = HitTestSystemSettingsRow(contentBounds, contentY, x, y);
    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
        float resetW = 150.0f * dp, resetH = 26.0f * dp;
        const float pageHeaderTop = contentY - SettingsLayout::kPageHeaderReserve * dp;
        D2D1_RECT_F resetRect = D2D1::RectF(contentBounds.right - padX - resetW, pageHeaderTop - 2.0f * dp, contentBounds.right - padX, pageHeaderTop - 2.0f * dp + resetH);
        isResetShortcutsHovered = PtInRectF(resetRect, x, y);
        float scTop = contentY + 32.0f * dp;
        auto scList = settings.GetShortcutActionListRef();
        size_t totalCount = scList.size();
        const bool singleColumn = SettingsLayout::ShortcutsSingleColumn(contentW, dp);
        size_t mid = singleColumn ? totalCount : (totalCount + 1) / 2;
        float colGap = 16.0f * dp;
        float colW = singleColumn ? contentW : (contentW - colGap) * 0.5f;
        float scRowH = 38.0f * dp, rowGap = 5.0f * dp;
        for (size_t rowIdx = 0; rowIdx < mid; ++rowIdx) {
            float rTop = scTop + rowIdx * (scRowH + rowGap);
            if (rowIdx < totalCount) {
                D2D1_RECT_F leftRect = D2D1::RectF(contentBounds.left + padX, rTop, contentBounds.left + padX + colW, rTop + scRowH);
                if (PtInRectF(leftRect, x, y)) { hoveredShortcutIndex = (int)rowIdx; break; }
            }
            if (!singleColumn && mid + rowIdx < totalCount) {
                D2D1_RECT_F rightRect = D2D1::RectF(contentBounds.left + padX + colW + colGap, rTop, contentBounds.right - padX, rTop + scRowH);
                if (PtInRectF(rightRect, x, y)) { hoveredShortcutIndex = (int)(mid + rowIdx); break; }
            }
        }
    }
}

void App::ToggleExperienceFeature(const std::string& key) {
    // The feature editor exists only in Custom; the guard is defensive for
    // programmatic callers. Forking seeds Custom from the surface being viewed.
    if (FeatureGate::ModeFromString(settings.experienceMode) != ExperienceMode::Custom) {
        ApplyExperienceModeSelection(ExperienceMode::Custom, /*forkFromPreview=*/true);
    }
    auto& enabled = settings.customEnabledFeatures;
    auto it = std::find(enabled.begin(), enabled.end(), key);
    if (it != enabled.end()) {
        enabled.erase(it);
    } else {
        enabled.push_back(key);
    }
    SaveSettings();
}

// Settings content width (view minus sidebar and side padding) shared by the
// shortcuts single-column mapping in focus capture/restore, hover hit-testing,
// keyboard navigation, and the scroll extent.
float App::SettingsContentWidth(float dp) const {
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const D2D1_RECT_F view = Presentation::HubView(static_cast<float>(std::max<LONG>(1, rc.right - rc.left)),
                                                   static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top)), dp);
    const D2D1_RECT_F content = SettingsLayout::ContentBounds(view, dp);
    const float padX = SettingsLayout::kContentPadX * dp;
    return RectWidth(content) - padX * 2.0f;
}

float App::GetSettingsContentHeight(float dp) const {
    float settingsCardTop = (SettingsLayout::kContentStartOffset +
        (currentSettingsCat == SettingsCategory::Subtitles ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve)) * dp;
    float settingsCardH = 164.0f * dp;
    if (currentSettingsCat == SettingsCategory::Playback) {
        const bool simpleSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Simple;
        if (simpleSettings) {
            // Behavior card + 2 simple cards (seek, frame) + speed, with one gap
            // between each of the 4 cards. The advanced tail (MPV config, global
            // hotkeys, tray, minimize, network caches) is hidden in Simple.
            settingsCardH = (SettingsLayout::kPlaybackBehaviorCardHeight + 2.0f * SettingsLayout::kPlaybackSimpleCardHeight +
                SettingsLayout::kPlaybackSpeedCardHeight +
                3.0f * SettingsLayout::kSectionCardGap) * dp;
        } else {
            // Behavior card + 7 simple cards (seek, frame, global hotkeys, tray, minimize, network forward cache, network back cache) +
            // speed + MPV, with one gap between each of the 10 cards.
            settingsCardH = (SettingsLayout::kPlaybackBehaviorCardHeight + 7.0f * SettingsLayout::kPlaybackSimpleCardHeight +
                SettingsLayout::kPlaybackSpeedCardHeight + SettingsLayout::kPlaybackMpvCardHeight +
                9.0f * SettingsLayout::kSectionCardGap) * dp;
        }
    } else if (currentSettingsCat == SettingsCategory::Subtitles) {
        settingsCardH = (SettingsLayout::kSubtitlePreviewCardHeight + 2.0f * SettingsLayout::kSubtitleTrackCardHeight +
            SettingsLayout::kSubtitleFontCardHeight + SettingsLayout::kSubtitleColorCardHeight +
            SettingsLayout::kSubtitleStyleCardHeight + SettingsLayout::kSubtitlePositionCardHeight +
            6.0f * SettingsLayout::kSubtitleCardGap + SettingsLayout::kSubtitleBottomPadding) * dp;
    } else if (currentSettingsCat == SettingsCategory::Experience) {
        RECT rc{};
        GetClientRect(hWnd, &rc);
        const D2D1_RECT_F view = Presentation::HubView(static_cast<float>(std::max<LONG>(1, rc.right - rc.left)),
                                                       static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top)), dp);
        const D2D1_RECT_F content = SettingsLayout::ContentBounds(view, dp);
        const float padX = SettingsLayout::kContentPadX * dp;
        const bool customSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Custom;
        settingsCardH = SettingsLayout::MakeExperienceLayout(content.left + padX, content.right - padX, 0.0f, dp, customSettings).bottom;
    } else if (currentSettingsCat == SettingsCategory::System) {
        const bool simpleSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Simple;
        settingsCardH = (SettingsLayout::kSystemActionsTop + SettingsLayout::SystemRowCount(simpleSettings) * 68.0f + 16.0f) * dp;
    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
        const bool single = SettingsLayout::ShortcutsSingleColumn(SettingsContentWidth(dp), dp);
        const size_t totalCount = settings.GetShortcutActionListRef().size();
        const size_t rows = single ? totalCount : (totalCount + 1) / 2;
        settingsCardH = (24.0f + 54.0f + 6.0f + static_cast<float>(rows) * 43.0f + 20.0f) * dp;
    }
    return settingsCardTop + settingsCardH;
}

float App::GetMaxScrollForPane(AutoscrollPane pane) {
    if (!hWnd || playbackSession.IsOpen()) return 0.0f;
    RECT rc{};
    if (!GetClientRect(hWnd, &rc)) return 0.0f;
    float dp = std::max(0.5f, renderer.GetDpiScale());
    float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    return GetMaxScrollForPaneInViewport(pane, winW, winH, dp);
}

// Viewport-parameterized variant so ClampScrollOffsets can resolve one client
// rect per frame instead of one GetClientRect syscall per pane.
float App::GetMaxScrollForPaneInViewport(AutoscrollPane pane, float winW, float winH, float dp) {
    float pad = 18.0f * dp;
    const D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    const float viewTop = viewBounds.top, viewBottom = viewBounds.bottom;
    float viewH = std::max(1.0f * dp, viewBottom - viewTop);
    switch (pane) {
    case AutoscrollPane::Media: {
        if (currentTab != TabView::Media) return 0.0f;
        EnsureMediaLayoutCache();
        float tbH = 32.0f * dp, mediaListTop = viewTop + tbH + 12.0f * dp;
        float mediaViewH = std::max(1.0f * dp, viewBottom - mediaListTop);
        float mediaContentH = m_mediaLayoutCache.contentHeightDIP * dp;
        return std::max(0.0f, mediaContentH - mediaViewH);
    }
    case AutoscrollPane::PlaylistRail: {
        if (currentTab != TabView::Playlists) return 0.0f;
        float railContentH = 48.0f * dp - 2.0f * dp;
        railContentH += library.playlists.size() * (52.0f * dp + 6.0f * dp);
        float railVisibleH = std::max(1.0f * dp, viewH - 34.0f * dp - 18.0f * dp);
        return std::max(0.0f, railContentH - railVisibleH);
    }
    case AutoscrollPane::PlaylistDetail: {
        if (currentTab != TabView::Playlists) return 0.0f;
        Playlist* activePlaylist = library.FindPlaylist(activePlaylistId);
        if (!activePlaylist || activePlaylist->itemPaths.empty()) return 0.0f;
        float availableW = std::max(1.0f * dp, winW - pad * 2.0f);
        float railW = std::min(260.0f * dp, std::max(120.0f * dp, availableW * 0.42f));
        float detailLeft = std::min(winW - pad, pad + railW + 16.0f * dp);
        float detailW = std::max(1.0f * dp, winW - pad - detailLeft);
        float detailContentBottom = 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp + 32.0f * dp + 12.0f * dp;
        size_t visibleCount = GetVisiblePlaylistIndices().size();
        if (playlistGridView) {
            float detailWForGrid = std::max(120.0f * dp, detailW - 26.0f * dp);
            size_t columns = (size_t)std::max(1.0f, std::floor((detailWForGrid + 10.0f * dp) / (190.0f * dp)));
            size_t rows = (visibleCount + columns - 1) / columns;
            detailContentBottom += rows * (166.0f * dp + 10.0f * dp) - (visibleCount ? 10.0f * dp : 0.0f);
        } else { detailContentBottom += visibleCount * (62.0f * dp + 6.0f * dp) - (visibleCount ? 6.0f * dp : 0.0f); }
        detailContentBottom += 40.0f * dp;
        return std::max(0.0f, detailContentBottom - viewH);
    }
    case AutoscrollPane::History: {
        if (currentTab != TabView::History) return 0.0f;
        const float historyContentBottom = library.history.empty() ? kHistoryContentInset * dp
            : HistoryContentHeight(GetHistoryProjection().groups, dp);
        return std::max(0.0f, historyContentBottom - viewH);
    }
    case AutoscrollPane::Settings: {
        if (currentTab != TabView::Settings) return 0.0f;
        return std::max(0.0f, GetSettingsContentHeight(dp) - viewH);
    }
    default: return 0.0f;
    }
}

void App::ClampScrollOffsets() {
    if (!hWnd) return;
    // While a touch pan, inertia, or rubber-band settle owns an offset, its
    // out-of-bounds excursion is intentional; the spring brings it back.
    const bool touchOwnsPane = touchPanPane != AutoscrollPane::None &&
        (touchHub.phase() == Touch::Phase::Panning || touchPanVelocity != 0.0f || touchSpringBack);
    auto clamp = [](float value, float maximum) { return std::clamp(value, 0.0f, std::max(0.0f, maximum)); };
    if (!playbackSession.IsOpen()) {
        // One client-rect syscall per frame for all five pane maxima.
        RECT rc{};
        if (GetClientRect(hWnd, &rc)) {
            const float dp = std::max(0.5f, renderer.GetDpiScale());
            const float winW = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
            const float winH = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
            if (!(touchOwnsPane && touchPanPane == AutoscrollPane::Media))
                mediaScrollY = clamp(mediaScrollY, GetMaxScrollForPaneInViewport(AutoscrollPane::Media, winW, winH, dp));
            if (!(touchOwnsPane && touchPanPane == AutoscrollPane::PlaylistRail))
                playlistRailScrollY = clamp(playlistRailScrollY, GetMaxScrollForPaneInViewport(AutoscrollPane::PlaylistRail, winW, winH, dp));
            if (!(touchOwnsPane && touchPanPane == AutoscrollPane::PlaylistDetail))
                playlistDetailScrollY = clamp(playlistDetailScrollY, GetMaxScrollForPaneInViewport(AutoscrollPane::PlaylistDetail, winW, winH, dp));
            if (!(touchOwnsPane && touchPanPane == AutoscrollPane::History))
                historyScrollY = clamp(historyScrollY, GetMaxScrollForPaneInViewport(AutoscrollPane::History, winW, winH, dp));
            if (!(touchOwnsPane && touchPanPane == AutoscrollPane::Settings))
                settingsScrollY = clamp(settingsScrollY, GetMaxScrollForPaneInViewport(AutoscrollPane::Settings, winW, winH, dp));
        }
    }
    if (autoscroll.mode != AutoscrollMode::Inactive) {
        float* pScroll = GetScrollOffsetPtr(autoscroll.pane);
        if (!pScroll) { StopAutoscroll(true); } else {
            float maxScroll = GetMaxScrollForPane(autoscroll.pane);
            if (maxScroll <= 0.5f) { StopAutoscroll(true); } else if ((*pScroll <= 0.0f && autoscroll.velocityDIPPerSecond < 0.0f) ||
                       (*pScroll >= maxScroll && autoscroll.velocityDIPPerSecond > 0.0f)) {
                StopAutoscroll(true);
            }
        }
    }
}

float* App::GetScrollOffsetPtr(AutoscrollPane pane) {
    if (pane == AutoscrollPane::Media && currentTab == TabView::Media) return &mediaScrollY;
    if (pane == AutoscrollPane::PlaylistRail && currentTab == TabView::Playlists) return &playlistRailScrollY;
    if (pane == AutoscrollPane::PlaylistDetail && currentTab == TabView::Playlists) return &playlistDetailScrollY;
    if (pane == AutoscrollPane::History && currentTab == TabView::History) return &historyScrollY;
    if (pane == AutoscrollPane::Settings && currentTab == TabView::Settings) return &settingsScrollY;
    return nullptr;
}

App::AutoscrollPane App::HitTestAutoscrollPane(float mouseX, float mouseY, float& outMaxScroll) {
    outMaxScroll = 0.0f;
    if (!hWnd || playbackSession.IsOpen()) return AutoscrollPane::None;
    RECT rc{};
    if (!GetClientRect(hWnd, &rc)) return AutoscrollPane::None;
    float dp = std::max(0.5f, renderer.GetDpiScale());
    float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    float pad = 18.0f * dp;
    D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    if (currentTab == TabView::Media) {
        if (!PtInRectF(viewBounds, mouseX, mouseY)) return AutoscrollPane::None;
        outMaxScroll = GetMaxScrollForPane(AutoscrollPane::Media);
        return (outMaxScroll > 0.5f) ? AutoscrollPane::Media : AutoscrollPane::None;
    }
    if (currentTab == TabView::Playlists) {
        float availableW = std::max(1.0f * dp, winW - pad * 2.0f);
        float railW = std::min(260.0f * dp, std::max(120.0f * dp, availableW * 0.42f));
        D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
        float detailLeft = std::min(winW - pad, pad + railW + 16.0f * dp);
        D2D1_RECT_F detailBounds = D2D1::RectF(detailLeft, viewBounds.top, viewBounds.right, viewBounds.bottom);
        if (PtInRectF(railBounds, mouseX, mouseY)) {
            outMaxScroll = GetMaxScrollForPane(AutoscrollPane::PlaylistRail);
            return (outMaxScroll > 0.5f) ? AutoscrollPane::PlaylistRail : AutoscrollPane::None;
        }
        if (PtInRectF(detailBounds, mouseX, mouseY)) {
            outMaxScroll = GetMaxScrollForPane(AutoscrollPane::PlaylistDetail);
            return (outMaxScroll > 0.5f) ? AutoscrollPane::PlaylistDetail : AutoscrollPane::None;
        }
        return AutoscrollPane::None;
    }
    if (currentTab == TabView::History) {
        if (!PtInRectF(viewBounds, mouseX, mouseY)) return AutoscrollPane::None;
        outMaxScroll = GetMaxScrollForPane(AutoscrollPane::History);
        return (outMaxScroll > 0.5f) ? AutoscrollPane::History : AutoscrollPane::None;
    }
    if (currentTab == TabView::Settings) {
        if (!PtInRectF(viewBounds, mouseX, mouseY)) return AutoscrollPane::None;
        outMaxScroll = GetMaxScrollForPane(AutoscrollPane::Settings);
        return (outMaxScroll > 0.5f) ? AutoscrollPane::Settings : AutoscrollPane::None;
    }
    return AutoscrollPane::None;
}

void App::StartAutoscroll(AutoscrollPane pane, float mouseX, float mouseY) {
    if (pane == AutoscrollPane::None) return;
    float maxScroll = GetMaxScrollForPane(pane);
    if (maxScroll <= 0.5f) return;
    if (previewPlayer.IsActive()) { previewPlayer.StopPreview(); }
    hoveredMediaIndex = -1;
    isPinnedCloseHovered = false;
    bool hasPinned = !isLastWatchedCleared && !library.mediaItems.empty() && libraryGroupMode == LibraryGroupMode::Flat &&
        (library.mediaItems[0].lastWatchedTimestamp > 0);
    for (size_t idx : m_activeAnimRowIndices) {
        if (idx < rowAnimStates.size()) {
            auto& row = rowAnimStates[idx];
            bool isPinned = (hasPinned && idx == 0);
            float restH = isPinned ? kMediaPinnedRestHeight : kMediaRestHeight;
            row.isHovering = false;
            row.hoverEnterTime = -1.0f;
            row.height.SnapTo(restH);
            row.liftY.SnapTo(0.0f);
            row.previewOpacity.SnapTo(0.0f);
            row.inActiveAnimList = false;
        }
    }
    m_activeAnimRowIndices.clear();
    InvalidateMediaLayoutCache();
    autoscroll.mode = AutoscrollMode::Pending;
    autoscroll.pane = pane;
    autoscroll.anchorPoint = { mouseX, mouseY };
    autoscroll.currentPoint = { mouseX, mouseY };
    autoscroll.velocityDIPPerSecond = 0.0f;
    autoscroll.suppressNextClick = false;
    SetCapture(hWnd);
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::StopAutoscroll(bool releaseCapture) {
    if (autoscroll.mode == AutoscrollMode::Inactive) return;
    autoscroll.mode = AutoscrollMode::Inactive;
    autoscroll.pane = AutoscrollPane::None;
    autoscroll.velocityDIPPerSecond = 0.0f;
    if (releaseCapture && GetCapture() == hWnd) { ReleaseCapture(); }
    InvalidateRect(hWnd, NULL, FALSE);
}

void App::UpdateAutoscroll(float dt) {
    if (autoscroll.mode == AutoscrollMode::Inactive || autoscroll.pane == AutoscrollPane::None) return;
    float* pScroll = GetScrollOffsetPtr(autoscroll.pane);
    if (!pScroll) { StopAutoscroll(true); return; }
    float maxScroll = GetMaxScrollForPane(autoscroll.pane);
    if (maxScroll <= 0.5f) { StopAutoscroll(true); return; }
    POINT pt{};
    if (GetCursorPos(&pt) && ScreenToClient(hWnd, &pt)) { autoscroll.currentPoint = {(float)pt.x, (float)pt.y}; }
    float dp = std::max(0.5f, renderer.GetDpiScale());
    float dyDIP = (autoscroll.currentPoint.y - autoscroll.anchorPoint.y) / dp;
    float dxDIP = (autoscroll.currentPoint.x - autoscroll.anchorPoint.x) / dp;
    constexpr float kDeadZoneDIP = 15.0f;
    if (autoscroll.mode == AutoscrollMode::Pending) {
        if (std::abs(dyDIP) > kDeadZoneDIP || std::abs(dxDIP) > kDeadZoneDIP) {
            if (GetCapture() == hWnd && (GetAsyncKeyState(VK_MBUTTON) & 0x8000)) { autoscroll.mode = AutoscrollMode::Holding; }
        }
    }
    float targetVelocity = 0.0f;
    if (std::abs(dyDIP) > kDeadZoneDIP) {
        float rawSpeed = 0.008f * std::pow(std::abs(dyDIP), 2.2f);
        targetVelocity = std::copysign(rawSpeed, dyDIP);
    }
    RECT rc{};
    GetClientRect(hWnd, &rc);
    float winW = (float)std::max<LONG>(1, rc.right - rc.left);
    float winH = (float)std::max<LONG>(1, rc.bottom - rc.top);
    float viewHDIP = std::max(1.0f, RectHeight(Presentation::HubView(winW, winH, dp)) / dp);
    float maxSpeedDIPPerSec = std::clamp(3.5f * viewHDIP, 1200.0f, 4500.0f);
    targetVelocity = std::clamp(targetVelocity, -maxSpeedDIPPerSec, maxSpeedDIPPerSec);
    float safeDt = std::clamp(dt, 0.0f, 0.05f);
    bool braking = (std::abs(targetVelocity) < std::abs(autoscroll.velocityDIPPerSecond)) ||
                   (targetVelocity * autoscroll.velocityDIPPerSecond < 0.0f) || (targetVelocity == 0.0f);
    float tau = braking ? 0.016f : 0.035f;
    float blend = 1.0f - std::exp(-safeDt / tau);
    autoscroll.velocityDIPPerSecond += (targetVelocity - autoscroll.velocityDIPPerSecond) * blend;
    if (std::abs(autoscroll.velocityDIPPerSecond) < 0.1f && targetVelocity == 0.0f) { autoscroll.velocityDIPPerSecond = 0.0f; }
    if (autoscroll.velocityDIPPerSecond != 0.0f) {
        float nextScroll = *pScroll + autoscroll.velocityDIPPerSecond * dp * safeDt;
        if (nextScroll <= 0.0f) { *pScroll = 0.0f; StopAutoscroll(true); return; } else if (nextScroll >= maxScroll) {
            *pScroll = maxScroll;
            StopAutoscroll(true);
            return;
        } else { *pScroll = nextScroll; }
    }
}

void App::BeginClose() {
    if (closeRequested || shutdownComplete) return;
    closeRequested = true;
    // The video child must never remain owned by the shell wallpaper WorkerW.
    DisableWallpaperMode();
    accessibilityActions.clear();
    accessibilityTree.SetWindow(nullptr);
    accessibilityTree.ClearNodes();
    playerCallbacksDisabled.store(true, std::memory_order_release);
    ClearPendingPlayerEvents();
    StopAutoscroll(true);
    if (playbackSession.IsOpen()) {
        player.Pause();
        PersistCurrentPlaybackProgress();
        if (playbackSession.CanPersistLocalTarget(player.GetCurrentFilePath())) PromoteLastWatchedMedia();
    }
    if (playbackSession.IsPiP()) PersistPiPGeometry(); else PersistWindowPlacement();
    SaveAll(); ReportLibrarySaveResult(SubmitPendingLibrarySave());
    // Publish the final jump list immediately, bypassing the rate limiter, so
    // the closing session's history write is not lost to the 30 s throttle.
    if (settings.jumpListEnabled && !IsPrivateMode()) {
        jumpListDirty = false;
        jumpListSeeded = true;
        lastJumpListPublishMs = GetTickCount64();
        UpdateJumpList(RecentMediaPathsForJumpList(library.history, 10));
    }
    closeSaveSerial = librarySaveWorker.RequestCheckpoint();
    libraryLoadCancelRequested.store(true, std::memory_order_release);
    librarySnapshotMailbox.Cancel();
    if (libraryLoadWorker.joinable()) CancelSynchronousIo(libraryLoadWorker.native_handle());
    deferredLibraryCommands.clear(); pendingPlaybackOpen.reset();
    CancelImportJob(); CancelBackfillJob(); CancelNetworkJobs();
    watchReconcileWorker.Cancel();
    folderRediscoveryJob.Cancel();
    folderWatcher.RequestStop();
    pathAvailability.Clear();
    renderer.CancelThumbnailWork();
    // System-integration teardown: hotkeys and the tray icon must not outlive the window.
    if (hWnd) {
        for (UINT i = 0; i < kGlobalHotkeyCount; ++i) { UnregisterHotKey(hWnd, kGlobalHotkeyBase + i); }
    }
    registeredGlobalHotkeys = 0;
    RemoveTrayIcon();
    if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
    if (hWnd) ShowWindow(hWnd, SW_HIDE);
}

bool App::PollClose() {
    PumpLibrarySaveResults();
    if (!closeRequested || shutdownComplete) return shutdownComplete;
    // Repeating cancellation closes the small race between a stop request and
    // the worker entering an otherwise interruptible synchronous directory open.
    if (!folderWatcher.IsWorkerDone()) folderWatcher.RequestStop();
    if (!libraryLoadWorkerDone.load(std::memory_order_acquire) ||
        (importWorker.joinable() && !importDone.load(std::memory_order_acquire)) ||
        (backfillWorker.joinable() && !backfillDone.load(std::memory_order_acquire)) ||
        AreNetworkJobsRunning() || watchReconcileWorker.IsRunning() || folderRediscoveryJob.IsRunning() || pathAvailability.IsRunning() || !folderWatcher.IsWorkerDone() || renderer.AreThumbnailWorkersRunning() || renderer.AreDeviceWorkersRunning() ||
        !librarySaveWorker.IsComplete(closeSaveSerial)) return false;
    Shutdown();
    if (hWnd && IsWindow(hWnd)) DestroyWindow(hWnd);
    return true;
}

void App::Shutdown() {
    if (shutdownComplete) return;
    if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
    if (hWnd) ShowWindow(hWnd, SW_HIDE);
    // Idempotent system-integration teardown (BeginClose already ran on the
    // WM_CLOSE path; the destructor path reaches Shutdown directly).
    if (hWnd) {
        for (UINT i = 0; i < kGlobalHotkeyCount; ++i) { UnregisterHotKey(hWnd, kGlobalHotkeyBase + i); }
    }
    registeredGlobalHotkeys = 0;
    RemoveTrayIcon();
    if (hTrayIcon) { DestroyIcon(hTrayIcon); hTrayIcon = nullptr; }
    StopAutoscroll(true);
    // Invalidate publication before joining. The worker owns its entire
    // snapshot, so an early close cannot post or save partial library state.
    CancelInitialLibraryLoad();
    // Fence callbacks before any UI/library state or window-owned resource
    // is torn down. MpvPlayer::Shutdown below joins its event thread; the
    // second clear after that join handles a callback that was already in flight when this fence was raised.
    playerCallbacksDisabled.store(true, std::memory_order_release);
    ClearPendingPlayerEvents();
    StopTextInputCaretTimer();
    if (hWnd) KillTimer(hWnd, kTextInputImeEchoTimer);
    activeTextInputField = TextInputField::None;
    textInputDragging = false;
    pendingImeEcho.clear();
    CancelImportJob();
    CancelBackfillJob();
    CancelNetworkJobs();
    if (importWorker.joinable()) importWorker.join();
    if (backfillWorker.joinable()) backfillWorker.join();
    updateCheckJob.StopAndJoin(); updateDownloadJob.StopAndJoin(); onlineSubtitleJobs.StopAndJoin(); radioJobs.StopAndJoin();
    podcastJobs.StopAndJoin();
    // Workers have joined above, so no probe can still hold MF. Balance only
    // when MF was ever acquired; a hub that never probed skips MFShutdown.
    MediaFoundation::ShutdownIfInitialized();
    folderWatcher.SetCallback(nullptr);
    folderWatcher.Stop();
    watchReconcileWorker.StopAndJoin();
    folderRediscoveryJob.StopAndJoin();
    pathAvailability.StopAndJoin();
    library.SetAvailabilityProvider({});
    accessibilityActions.clear();
    accessibilityTree.SetWindow(nullptr);
    accessibilityTree.ClearNodes();
    shutdownComplete = true;
    SetDisplayPowerRequest(false);
    mediaTransport.Shutdown();
    taskbarController.Shutdown();
    // Stop callbacks and background preview work before releasing renderer
    // resources. This also makes WM_CLOSE followed by the destructor safe.
    previewPlayer.StopPreview();
    if (playbackSession.IsOpen()) {
        if (!closeRequested && playbackSession.CanPersistLocalTarget(player.GetCurrentFilePath())) {
            double curTime = player.GetCurrentTime();
            if (curTime >= 0.0 && std::isfinite(curTime)) { playbackSession.SetPlaybackPosition(curTime); }
            library.UpdateProgress(player.GetCurrentFilePath(), player.GetCurrentTime(), player.GetDuration());
            PromoteLastWatchedMedia();
        }
        SaveSession();
        player.Close();
        player.SetVideoVisible(false);
        playbackSession.Close();
    }
    if (playbackSession.IsPiP()) {
        PersistPiPGeometry();
    } else {
        PersistWindowPlacement();
    }
    if (!closeRequested) SaveAll();
    ReportLibrarySaveResult(SubmitPendingLibrarySave());
    librarySaveWorker.StopAndFlush(); PumpLibrarySaveResults();
    library.SetSaveRequestHandler({});
    if (hWnd && pOleDropTarget) { RevokeDragDrop(hWnd); pOleDropTarget->Release(); pOleDropTarget = nullptr; }
    if (oleInitialized) { OleUninitialize(); oleInitialized = false; }
    // Both objects are safe to shut down repeatedly, but avoid touching the lazy libmpv handles when neither path was ever initialized.
    if (previewInitialized || previewInitAttempted) previewPlayer.Shutdown();
    hubExtractors[0].Shutdown();
    hubExtractors[1].Shutdown();
    if (playerInitialized || playerInitAttempted) player.Shutdown();
    player.ClearCallbacks();
    ClearPendingPlayerEvents();
    previewInitialized = false;
    previewInitAttempted = false;
    playerInitialized = false;
    playerInitAttempted = false;
    // Tooltip child windows must be destroyed while their owner HWNDs still
    // exist. The overlay manager is destroyed first because its owner is
    // torn down below; the hub manager then releases its main-window child.
    playerTooltips.Destroy();
    hubTooltips.Destroy();
    playerTooltipSignature = 0;
    hubTooltipSignature = 0;
    if (hOverlayWnd) {
        SetWindowLongPtr(hOverlayWnd, GWLP_USERDATA, 0);
        ShowWindow(hOverlayWnd, SW_HIDE);
        DestroyWindow(hOverlayWnd);
        hOverlayWnd = NULL;
    }
    overlayRenderer.Shutdown();
    renderer.Shutdown();
    // Restore default scheduling before exit so an EcoQoS state set while minimized cannot outlive the process' normal priority handling.
    SetEcoThrottling(false);
    if (hStartupBgBrush) { DeleteObject(hStartupBgBrush); hStartupBgBrush = nullptr; }
    overlayReady = false;
    if (hVideoWnd) { SetWindowLongPtr(hVideoWnd, GWLP_USERDATA, 0); hVideoWnd = NULL; }
    appInitialized = false;
}

bool App::HasPresentedFrame() const { return renderer.HasPresentedFrame(); }

void App::SetBootstrapColors(COLORREF bg, COLORREF text, COLORREF subtext) {
    bootstrapBg = bg;
    bootstrapText = text;
    bootstrapSubtext = subtext;
    bootstrapColorsSet = true;
}

bool App::EnsureHubTooltips() {
    if (shutdownComplete || !hWnd || !IsWindow(hWnd)) return false;
    if (hubTooltips.IsCreated()) return true;
    // Defer off the critical path: no tooltip windows until the first GPU frame has presented. Keyboard/dwell paths after startup still
    // create on demand because HasPresentedFrame is true by then.
    if (!renderer.HasPresentedFrame()) return false;
    if (!hubTooltips.Create(hWnd, 500, 360)) return false;
    ApplyTooltipTheme();
    return true;
}

bool App::EnsurePlayerTooltips() {
    if (shutdownComplete || !hOverlayWnd || !IsWindow(hOverlayWnd)) return false;
    if (playerTooltips.IsCreated()) return true;
    // Player tooltips exist only while playback is open; creation is tied to
    // first playback, never hub startup.
    if (!playbackSession.IsOpen()) return false;
    if (!playerTooltips.Create(hOverlayWnd, 500, 360)) return false;
    ApplyTooltipTheme();
    return true;
}

void App::PaintBootstrapShell(HDC hdc, const RECT& rc) {
    if (!hdc) return;
    // Same theme background the GPU frame clears with, so the handoff has no
    // color jump. GDI only: no D3D/DWrite/WIC/COM/MF. Before Initialize uses
    // the wWinMain-provided bootstrap colors; after, the live theme (same
    // values, so no flash on theme adoption).
    COLORREF bg = bootstrapColorsSet ? bootstrapBg : D2DColorToColorRef(settings.currentTheme.bg);
    COLORREF textCol = bootstrapColorsSet ? bootstrapText : RGB((BYTE)(settings.currentTheme.text.r * 255.0f),
              (BYTE)(settings.currentTheme.text.g * 255.0f), (BYTE)(settings.currentTheme.text.b * 255.0f));
    COLORREF subCol = bootstrapColorsSet ? bootstrapSubtext : RGB((BYTE)(settings.currentTheme.textMuted.r * 255.0f),
              (BYTE)(settings.currentTheme.textMuted.g * 255.0f), (BYTE)(settings.currentTheme.textMuted.b * 255.0f));
    HBRUSH bgBrush = CreateSolidBrush(bg);
    if (bgBrush) { FillRect(hdc, &rc, bgBrush); DeleteObject(bgBrush); }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textCol);
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    // Title in a stock UI font; subtitle hints loading without promising GPU.
    HFONT titleFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT prevFont = (HFONT)SelectObject(hdc, titleFont);
    const wchar_t* title = L"Aperture";
    SIZE titleSize{};
    GetTextExtentPoint32W(hdc, title, (int)wcslen(title), &titleSize);
    TextOutW(hdc, cx - titleSize.cx / 2, cy - 24, title, (int)wcslen(title));
    SetTextColor(hdc, subCol);
    const wchar_t* hint = L"Loading library…";
    SIZE hintSize{};
    GetTextExtentPoint32W(hdc, hint, (int)wcslen(hint), &hintSize);
    TextOutW(hdc, cx - hintSize.cx / 2, cy + 4, hint, (int)wcslen(hint));
    SelectObject(hdc, prevFont);
}

bool App::Initialize(HWND hwnd, HWND hVideoChild, HINSTANCE hinst, const PlaybackCommandLineOptions& startupOptions, const Settings* startupSettings) {
    shutdownComplete = false;
    // Media Foundation stays uninitialized until the first import/metadata probe acquires it (see MediaFoundation). An idle hub never pays
    // for MFStartup or the MF DLL mapping.
    playerCallbacksDisabled.store(true, std::memory_order_release);
    ClearPendingPlayerEvents();
    playerInitAttempted = false;
    playerInitialized = false;
    previewInitAttempted = false;
    previewInitialized = false;
    hWnd = hwnd;
    hVideoWnd = hVideoChild;
    // Taskbar/SMTC stay uninitialized until first playback (see Update() and
    // UpdateMediaTransport()). No COM/taskbar/SMTC cost on the hub path.
    if (hVideoWnd) { SetWindowLongPtr(hVideoWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this)); }
    hInstance = hinst;
    // Modal dialogs pump their own message loop, starving the main loop's
    // heartbeat; a window timer keeps the hang watchdog fed through any pump
    // while a genuinely stuck UI thread still stops ticking.
    SetTimer(hwnd, kHeartbeatTimer, 2000, nullptr);
    accessibilityTree.SetWindow(hWnd);
    accessibilityDirty = true;
    HRESULT oleHr = OleInitialize(NULL);
    oleInitialized = SUCCEEDED(oleHr);
    pOleDropTarget = new ApertureDropTarget(this);
    if (FAILED(RegisterDragDrop(hWnd, pOleDropTarget))) { pOleDropTarget->Release(); pOleDropTarget = nullptr; }
    hInstance = hinst;
    StorageStatus writability = CheckDataDirectoryWritability();
    if (!writability.is_ok()) {
        std::wstring dataDir = GetDataDir();
        const auto msg = Presentation::StorageGuidance(writability) + L"\n\n" + dataDir;
        MessageBoxW(hwnd, msg.c_str(), Loc::Tr("ux.storage_title").c_str(), MB_ICONWARNING | MB_OK);
    }
    // wWinMain already loaded settings.json for window placement/theme; adopt
    // that copy instead of re-reading and re-validating the file.
    if (startupSettings) settings = *startupSettings;
    else settings.Load();
    if (!settings.GetLastLoadStatus().is_ok()) ReportStorageFailure(settings.GetLastLoadStatus(), L"settings load");
    systemVisualPreferences.Refresh();
    Diagnostics::SetLoggingEnabled(settings.diagnosticsLogging);
    Diagnostics::RecordHeartbeat();
    playlistGridView = settings.playlistViewMode == "grid";
    if (settings.libraryGroupMode == "series" && settings.smartSeriesDetection) {
        libraryGroupMode = LibraryGroupMode::Series;
    } else if (settings.libraryGroupMode == "folder" || (settings.libraryGroupMode == "series" && !settings.smartSeriesDetection)) {
        libraryGroupMode = LibraryGroupMode::Folder;
    } else if (settings.libraryGroupMode == "listen") {
        libraryGroupMode = LibraryGroupMode::Listen;
    } else { libraryGroupMode = LibraryGroupMode::Flat; }
    librarySortField = ResolveLibrarySortSetting(settings, libraryGroupMode);
    librarySortDirection = settings.librarySortDirection == "descending" ? SortDirection::Descending : SortDirection::Ascending;
    // Start the worker as soon as its only UI-derived input (group mode) is known. D3D/font initialization below now overlaps all
    // portable-store I/O, parsing, indexing, promotion, and the initial projection sort.
    StartLibraryLoad(libraryGroupMode);
    if (networkStreamModel.IsRecentStreamsEnabled()) { networkStreamModel.LoadRecentStreams(GetExeDir()); }
    podcastSubscriptions.Load(GetDataDir());
    // they are shown or applied to the player.
    settings.audioDelayMs = std::clamp(settings.audioDelayMs, -5000, 5000);
    settings.subDelayMs = std::clamp(settings.subDelayMs, -10000, 10000);
    if (settings.restoreSession) {
        const auto status = playbackSession.Load();
        if (!status.is_ok()) ReportStorageFailure(status, L"session load");
    }
    // The auto update check performs a synchronous network request; running it
    // here would gate the first GPU frame on the network (connect timeout 5 s).
    // It is deferred to Update() once the first shell has been presented.
    startupUpdateCheckPending = true;
    ApplyWindowTheme();
    // The main window is already shown by wWinMain; assert the persisted
    // always-on-top preference before the first hub frame is presented.
    ApplyAlwaysOnTop();
    autoplayToggleAnim.SnapTo(settings.autoplay ? 1.0f : 0.0f);
    rememberPosToggleAnim.SnapTo(settings.rememberPos ? 1.0f : 0.0f);
    hwAccelToggleAnim.SnapTo(settings.hwAccel ? 1.0f : 0.0f);
    loopToggleAnim.SnapTo(settings.loop ? 1.0f : 0.0f);
    customMpvConfigToggleAnim.SnapTo(settings.customMpvConfigEnabled ? 1.0f : 0.0f);
    for (int i = 0; i < 40; ++i) {
        // Catalog-position order — the same order the Experience rows render in.
        const bool enabled = FeatureGate::FeatureEnabledAt(FeatureGate::ModeFromString(settings.experienceMode),
                                                           settings.customEnabledFeatures, static_cast<size_t>(i));
        experienceToggleAnim[i].SnapTo(enabled ? 1.0f : 0.0f);
    }
    player.SetCustomMpvConfigEnabled(settings.customMpvConfigEnabled);
    player.SetCustomMpvConfigPath(settings.customMpvConfigPath);
    if (!renderer.Initialize(hWnd, settings.uiFontFamily, startupOptions.mediaTargets.empty())) { return false; }
    // The D3D worker is joined on first paint; only fonts/factories are ready here.
    Diagnostics::RecordStartupMilestone(L"renderer-initialized");
    renderer.SetTextScale(systemVisualPreferences.Get().textScaleFactor);
    // Register and create Layered Transparent Overlay Window for Video Player Controls
    WNDCLASSEXW owc = { sizeof(WNDCLASSEXW) };
    owc.lpfnWndProc = OverlayWndProc;
    owc.hInstance = hInstance;
    owc.lpszClassName = L"ApertureOverlay";
    owc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    owc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&owc);
    RECT clientScreenRect{};
    GetClientScreenRect(hWnd, clientScreenRect);
    const int overlayX = clientScreenRect.left;
    const int overlayY = clientScreenRect.top;
    const int overlayW = std::max(1L, clientScreenRect.right - clientScreenRect.left);
    const int overlayH = std::max(1L, clientScreenRect.bottom - clientScreenRect.top);
    hOverlayWnd = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"ApertureOverlay", L"", WS_POPUP, overlayX, overlayY,
        overlayW, overlayH, hWnd, NULL, hInstance, NULL );
    if (hOverlayWnd) {
        SetWindowLongPtr(hOverlayWnd, GWLP_USERDATA, (LONG_PTR)this);
        DisableLegacyTouchGestures(hOverlayWnd);
        // The overlay DC target, backing DIB, and overlay fonts are created lazily by EnsureOverlayReady() when the player is first opened,
        // so hub startup does not allocate a full-window surface it never shows.
        ShowWindow(hOverlayWnd, SW_HIDE);
    }
    hubTooltipSignature = 0;
    playerTooltipSignature = 0;
    // Tooltips stay uncreated until first real use (see EnsureHubTooltips / EnsurePlayerTooltips): hub tooltips after the first GPU present
    // or on first dwell/keyboard focus, player tooltips on first playback. No common-controls cost on the critical startup path. Keep the
    // child video surface hidden until the first successful playback initialization. Main/preview libmpv resources are lazy and are created
    // by EnsurePlayerInitialized/EnsurePreviewInitialized on demand.
    if (hVideoWnd) ShowWindow(hVideoWnd, SW_HIDE);
    // SMTC stays uninitialized until first playback (see UpdateMediaTransport): no WinRT activation cost for an idle hub. Process initial
    // files through the same recursive/validated importer as picker and drag-and-drop input. This keeps multi-file CLI launches as a
    // deterministic queue and starts the first accepted item only after the background import has completed.
    if (!startupOptions.mediaTargets.empty() || startupOptions.HasOptions()) { ExecuteCommandLineOptions(startupOptions); }
    if (!startupOptions.visualCaptureScene.empty()) {
        const std::wstring& scene = startupOptions.visualCaptureScene;
        if (scene == L"media" || scene == L"hub_media") {
            SetTab(TabView::Media);
        } else if (scene == L"playlists" || scene == L"hub_playlists") {
            SetTab(TabView::Playlists);
        } else if (scene == L"history" || scene == L"hub_history") {
            SetTab(TabView::History);
        } else if (scene == L"settings" || scene == L"hub_settings" || scene == L"settings_appearance") {
            SetSettingsCategory(SettingsCategory::Appearance);
        } else if (scene == L"settings_playback") {
            SetSettingsCategory(SettingsCategory::Playback);
        } else if (scene == L"settings_subtitles") {
            SetSettingsCategory(SettingsCategory::Subtitles);
        } else if (scene == L"settings_shortcuts") {
            SetSettingsCategory(SettingsCategory::Shortcuts);
        } else if (scene == L"settings_system") { SetSettingsCategory(SettingsCategory::System); }
    }
    folderWatcher.SetCallback([this](const WatchChangeBatch& batch) {
        if (playerCallbacksDisabled.load(std::memory_order_acquire)) return;
        watchReconcileWorker.Queue(batch);
        if (!watchEventPosted.exchange(true)) { if (hWnd) PostMessageW(hWnd, kFolderWatchEventMessage, 0, 0); }
    });
    // Watched roots, Jump List publication, and metadata backfill begin only after the adopted snapshot has rendered once.
    appInitialized = true;
    // A very small/empty profile can finish while initialization is inside a nested Windows call. Ensure a completion deliberately ignored
    // by the bootstrap message guard is delivered again after the UI is ready.
    if (librarySnapshotMailbox.HasPending())
        PostMessageW(hWnd, kLibrarySnapshotReadyMessage, static_cast<WPARAM>(librarySnapshotMailbox.ActiveGeneration()), 0);
    // The accessibility tree is built by the first Render pass; an extra
    // forced build here would only be discarded and rebuilt ~150 ms later.
    // After startup settles, keep initialized resources ready in memory.
    // System-integration settings consume the loaded preferences at the end of
    // initialization: global hotkeys register here, and the tray icon appears
    // when enabled.
    ApplyGlobalHotkeys();
    UpdateTrayIcon();
    return true;
}

void App::ExecuteCommandLineOptions(const PlaybackCommandLineOptions& options) {
    if (closeRequested) return;
    if (!libraryReady || renderer.AreDeviceWorkersRunning()) {
        // Includes startup options and second-instance WM_COPYDATA commands. They are replayed in arrival order immediately after
        // publication. Keep the IPC-facing queue bounded if a large library takes a while
        // to load; the newest command is more useful than an arbitrarily old
        // one after the cap is reached.
        if (deferredLibraryCommands.size() >= kMaxDeferredLibraryCommands) { deferredLibraryCommands.pop_front(); }
        deferredLibraryCommands.push_back(options);
        return;
    }
    if (options.volume.has_value()) {
        double vol = std::clamp(*options.volume, 0.0, 100.0);
        player.SetVolume(vol / 100.0);
        settings.volume = vol / 100.0;
    }
    if (options.loop) { player.SetLoop(true); settings.loop = true; loopToggleAnim.SnapTo(1.0f); }
    if (options.mediaTargets.empty()) {
        if (options.fullscreen && !playbackSession.IsFullscreen()) { ToggleFullscreen(); }
        return;
    }
    if (options.enqueue) {
        if (playbackSession.IsOpen()) {
            playbackSession.AppendToQueue(options.mediaTargets);
            StartImportJob(options.mediaTargets, "", false);
            ShowOSD(IconType::FolderPlaylist, L"Enqueued " + std::to_wstring(options.mediaTargets.size()) + L" media");
            return;
        }
    }
    const std::wstring& firstTarget = options.mediaTargets.front();
    if (options.mediaTargets.size() > 1) {
        playbackSession.OpenQueue(options.mediaTargets, 0);
    } else { playbackSession.OpenDirect(firstTarget, 0.0); }
    double startPos = options.startPositionSec.value_or(0.0);
    OpenAndPlayMedia(firstTarget, startPos, options.mediaTargets.size() > 1);
    if (options.startPositionSec.has_value()) {
        if (player.IsLoaded() || player.IsPlaying()) { player.Seek(startPos); }
    }
    if (options.paused) { player.Pause(); playbackSession.SetPlayState(false); }
    if (options.fullscreen && !playbackSession.IsFullscreen()) { ToggleFullscreen(); }
    if (!options.subtitlePath.empty()) { player.LoadExternalSubtitle(options.subtitlePath); }
    // OpenAndPlayMedia already adds the single CLI target and arranges its metadata probe. Starting a second import for that same
    // file here created a duplicate Media Foundation source reader while playback was warming up (and needlessly doubled the media-open
    // memory spike). A multi-file CLI launch still imports the remaining queue entries.
    if (options.mediaTargets.size() > 1) { StartImportJob(options.mediaTargets, "", false); }
}

bool App::Update(float dt) {
    // Bound visual steps while keeping timers accurate when idle waits back off.
    // Clamping elapsed time in the message loop stretched autosave/sleep timers.
    const float elapsedDt = std::isfinite(dt) ? std::max(0.0f, dt) : 0.0f;
    dt = std::min(elapsedDt, 0.05f);
    Diagnostics::RecordHeartbeat();
    if (closeRequested) { PollClose(); return false; }
    bool needsRedraw = PumpLibrarySaveResults();
    pathAvailability.SetLibrarySize(library.mediaItems.size());
    if (pathAvailability.Pump()) {
        library.InvalidateAvailabilityViews();
        needsRedraw = true;
    }
    if (folderRediscoveryActive) FinishFolderRediscovery();
    if (libraryReady && (watchReconcileWorker.HasCompleted() ||
        (!watchReconcileWorker.IsRunning() && watchReconcileWorker.HasPending()))) HandleFolderWatchEvents();
    needsRedraw = PumpOnlineSubtitleJobs() || needsRedraw;
    needsRedraw = PumpRadioDirectoryJobs() || needsRedraw;
    needsRedraw = PumpPodcastJobs() || needsRedraw;
    needsRedraw = PumpUpdateJobs() || needsRedraw;
    // Jump list: seed once the async library snapshot is ready (Initialize
    // returns before the background load publishes), then pump dirty rebuilds
    // rate-limited by kJumpListRebuildMinIntervalMs.
    if (libraryReady && !jumpListSeeded) {
        jumpListSeeded = true;
        if (settings.jumpListEnabled) PumpJumpListUpdate(true);
    }
    if (jumpListDirty) PumpJumpListUpdate(false);
    // A graphics driver can hold the process loader lock while it initializes.
    // Avoid UI-thread joins or mpv export binding until that worker has finished.
    if (renderer.AreDeviceWorkersRunning()) return needsRedraw;
    const bool libraryHidden = playbackSession.IsOpen() || IsIconic(hWnd);
    if (libraryHidden) {
        const auto nowMs = GetTickCount64();
        if (!hiddenLibrarySinceMs) hiddenLibrarySinceMs = nowMs;
        if (!hiddenLibraryCachesTrimmed && nowMs - hiddenLibrarySinceMs >= 15000) {
            if (playbackSession.IsOpen()) {
                previewPlayer.BindDeviceContext(nullptr);
                renderer.TrimHiddenLibraryCaches(0, true);
            } else renderer.TrimHiddenLibraryCaches();
            hiddenLibraryCachesTrimmed = true;
        }
    } else {
        hiddenLibrarySinceMs = 0;
        hiddenLibraryCachesTrimmed = false;
    }
    // An explicit open is already committed to allocating the player. Once
    // the shell has painted, overlap its cold initialization with the catalog
    // worker; ordinary hub launches remain lazy. Preferences/queue publication
    // still wait for the authoritative library and stay on the owning thread.
    if (firstGpuShellRecorded && !libraryReady && !playerInitAttempted &&
        std::any_of(deferredLibraryCommands.begin(), deferredLibraryCommands.end(),
            [](const auto& command) { return !command.mediaTargets.empty(); })) {
        startupPlayerPrepared = EnsurePlayerInitialized();
    }
    if (libraryReady && !deferredLibraryCommands.empty()) {
        std::deque<PlaybackCommandLineOptions> commands; commands.swap(deferredLibraryCommands);
        for (const auto& command : commands) ExecuteCommandLineOptions(command);
        needsRedraw = true;
    }
    if (pendingPlaybackOpen) {
        auto request = std::move(*pendingPlaybackOpen); pendingPlaybackOpen.reset();
        OpenAndPlayMedia(request.path, request.position, request.preserveQueue); needsRedraw = true;
    }
    if (activeDragType != DragType::None) {
        const float blend = 1.0f - std::exp(-22.0f * std::min(dt, 0.05f));
        for (auto& entry : dragOffsets) {
            auto& value = entry.second;
            value.first.x += (value.second.x - value.first.x) * blend;
            value.first.y += (value.second.y - value.first.y) * blend;
        }
        const float dp = renderer.GetDpiScale();
        RECT client{}; GetClientRect(hWnd, &client);
        const float bottom = static_cast<float>(client.bottom) - 14.0f * dp;
        AutoscrollPane pane = activeDragType == DragType::PlaylistItem ? AutoscrollPane::PlaylistDetail
            : activeDragType == DragType::PlaylistRail ? AutoscrollPane::PlaylistRail : AutoscrollPane::Media;
        const float top = (pane == AutoscrollPane::PlaylistDetail ? 336.0f : pane == AutoscrollPane::PlaylistRail ? 130.0f : 128.0f) * dp;
        float* scroll = GetScrollOffsetPtr(pane);
        if (scroll && !isPlaylistsNavDropHovered) {
            *scroll = std::clamp(*scroll + Reorder::EdgeSpeed(dragCurrentY, top, pane == AutoscrollPane::PlaylistRail ? bottom - 54.0f * dp : bottom, dp) * std::min(dt, 0.05f),
                0.0f, GetMaxScrollForPane(pane));
            // Re-evaluate the destination even when the pointer is stationary.
            SendMessage(hWnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<short>(dragCurrentX), static_cast<short>(dragCurrentY)));
        }
        needsRedraw = true;
    }
    // A direct media launch can enter playback before the hub swap chain ever
    // presents. Only the hub needs this bootstrap retry; mpv and the layered
    // overlay own playback presentation. Otherwise an untouched hub flag keeps
    // repainting a fully transparent overlay at 60 Hz for the entire video.
    if (!playbackSession.IsOpen() && !renderer.HasPresentedFrame()) { needsRedraw = true; }
    if (IsAutoscrolling()) { UpdateAutoscroll(dt); needsRedraw = true; }
    if (UpdateTouchGestures(dt, NowSeconds())) { needsRedraw = true; }
    if (importJobActive) {
        PumpImportFirstPlayback();
        if (importDone.load(std::memory_order_acquire)) { FinishImportJob(); }
        needsRedraw = true;
    }
    // Import island lifecycle (reference design): Active -> Complete (check
    // mark, 1.5 s) -> Closing (28x8 collapse, 0.42 s) -> idle. A dismiss (X)
    // during the job closes immediately; the import itself keeps running.
    if (importIslandPhase == 0 && importJobActive && !importIslandDismissed) {
        importIslandPhase = 1;
        importIslandPhaseT = 0.0;
        importPinExpanded = false;
        importIslandWidthDip.SnapTo(28.0f);
        importIslandHeightDip.SnapTo(8.0f);
        importIslandRadiusDip.SnapTo(4.0f);
        importIslandFade.SnapTo(0.0f);
        importIslandExpand.SnapTo(0.0f);
    }
    if (importIslandPhase == 1 && importIslandDismissed) {
        importIslandPhase = 3;
        importIslandPhaseT = 0.0;
        importPinExpanded = false;
        importPillHovered = false;
        importCancelHovered = false;
    }
    if (importIslandPhase == 1 && !importJobActive) {
        importIslandPhase = 2;
        importIslandPhaseT = 0.0;
        importPinExpanded = false;
    }
    if (importIslandPhase == 2) {
        importIslandPhaseT += dt;
        if (importIslandPhaseT >= 1.5) { importIslandPhase = 3; importIslandPhaseT = 0.0; }
    }
    if (importIslandPhase == 3) {
        importIslandPhaseT += dt;
        if (importIslandPhaseT >= 0.45 && importIslandFade.current <= 0.02f) {
            importIslandPhase = 0;
            importIslandDismissed = false;
        } else if (importIslandPhaseT >= 0.65) {
            importIslandPhase = 0;
            importIslandDismissed = false;
        }
    }
    if (importIslandPhase != 0) {
        const bool expandedNow = importIslandPhase == 1 && !importIslandDismissed && (importPillHovered || importPinExpanded);
        AnimateVisual(importIslandExpand, expandedNow ? 1.0f : 0.0f, 0.48f, &kEaseIslandShell);
        const float dpIp = std::max(0.5f, renderer.GetDpiScale());
        float wT = 136.0f, hT = 32.0f, rT = 16.0f, fadeT = 1.0f;
        const CubicBezier* sizeCurve = &kEaseIslandShell;
        float sizeDur = 0.48f;
        if (importIslandPhase == 3) {
            wT = 28.0f; hT = 8.0f; rT = 4.0f;
            fadeT = 0.0f;
            sizeCurve = &kEaseIslandCollapse; sizeDur = 0.42f;
        } else if (importIslandPhase == 2) {
            wT = 144.0f;
        } else if (expandedNow) {
            const D2D1_RECT_F comp = ImportPillBounds(dpIp);
            wT = RectWidth(IslandExpandedRect(comp, dpIp)) / dpIp;
            hT = 82.0f; rT = 20.0f;
        } else {
            wT = std::max(136.0f, ImportIslandTargetWidthDip());
        }
        AnimateVisual(importIslandWidthDip, wT, sizeDur, sizeCurve);
        AnimateVisual(importIslandHeightDip, hT, sizeDur, sizeCurve);
        AnimateVisual(importIslandRadiusDip, rT, sizeDur, sizeCurve);
        AnimateVisual(importIslandFade, fadeT, 0.30f, MotionTokens::Standard());
        needsRedraw = true;
    } else {
        importIslandExpand.SnapTo(0.0f);
    }
    if (importIslandWidthDip.Tick()) needsRedraw = true;
    if (importIslandHeightDip.Tick()) needsRedraw = true;
    if (importIslandRadiusDip.Tick()) needsRedraw = true;
    if (importIslandExpand.Tick()) needsRedraw = true;
    if (importIslandFade.Tick()) needsRedraw = true;
    if (importIslandPhase == 1 || importIslandPhase == 2) {
        importSpinAngle = std::fmod(importSpinAngle + dt * (360.0 / 1.85), 360.0);
        needsRedraw = true;
    }
    // Listen island: poll the audio player, mirror master volume/mute, and
    // transition smoothly between compact and click-expanded states.
    if (listenPlaybackActive) {
        if (listenPlayer.PollFinished()) OnListenFinished();
        const double effectiveVol = settings.muted ? 0.0 : std::clamp(settings.volume, 0.0, 1.0);
        if (listenPlaybackActive && std::abs(effectiveVol - listenVolumeApplied) > 0.0005) {
            listenVolumeApplied = effectiveVol;
            listenPlayer.SetVolume(effectiveVol);
        }
        listenEqPhase += dt;
        needsRedraw = true; // EQ bars animate while audio plays
    }
    if (listenPlaybackActive || listenIslandFade.current > 0.001f) {
        const bool expandedNow = listenPlaybackActive && listenPinExpanded;
        AnimateVisual(listenIslandExpand, expandedNow ? 1.0f : 0.0f, 0.18f, MotionTokens::Standard());
        const float dpLs = std::max(0.5f, renderer.GetDpiScale());
        const D2D1_RECT_F comp = ListenPillBounds(dpLs);
        const float compW = std::max(220.0f, RectWidth(comp) / dpLs);
        float wT = compW, hT = 36.0f, rT = 18.0f, fadeT = 1.0f;
        if (!listenPlaybackActive) {
            fadeT = 0.0f;
        } else if (expandedNow) {
            wT = std::max(compW, std::min(380.0f, compW + 40.0f));
            hT = 88.0f;
            rT = 14.0f;
        }
        AnimateVisual(listenIslandWidthDip, wT, 0.18f, MotionTokens::Standard());
        AnimateVisual(listenIslandHeightDip, hT, 0.18f, MotionTokens::Standard());
        AnimateVisual(listenIslandRadiusDip, rT, 0.18f, MotionTokens::Standard());
        AnimateVisual(listenIslandFade, fadeT, 0.16f, MotionTokens::Standard());
        needsRedraw = true;
    }
    if (listenIslandWidthDip.Tick()) needsRedraw = true;
    if (listenIslandHeightDip.Tick()) needsRedraw = true;
    if (listenIslandRadiusDip.Tick()) needsRedraw = true;
    if (listenIslandExpand.Tick()) needsRedraw = true;
    if (listenIslandFade.Tick()) needsRedraw = true;
    if (backfillJobActive) {
        if (backfillDone.load(std::memory_order_acquire)) { FinishBackfillJob(); }
    }
    RunDeferredLibraryReadyWork();
    if (startupUpdateCheckPending && firstGpuShellRecorded && !shutdownComplete) {
        startupUpdateCheckPending = false;
        if (settings.autoUpdateCheckEnabled && !playbackSession.IsPrivateMode()) { CheckForUpdates(false); }
    }
    if (sleepTimerRemainingSec > 0.0f) {
        sleepTimerRemainingSec = std::max(0.0f, sleepTimerRemainingSec - elapsedDt);
        if (sleepTimerRemainingSec == 0.0f) { ClosePlayer(); ShowOSD(IconType::HistoryClock, L"Sleep timer finished"); }
    }
    ClampScrollOffsets();
    if (playbackSession.IsOpen()) {
        taskbarController.EnsureInitialized(hWnd);
        taskbarController.Update(TaskbarController::ProjectState(
            playbackSession, library, settings, player.GetCurrentTime(), player.GetDuration()));
    } else { taskbarController.Clear(); }
    // Controls idle fade & cursor hiding
    if (playbackSession.IsOpen()) {
        float targetControlsOpacity = 1.0f;
        if (!isScrubbing && !isVolumeScrubbing && !isSettingsFlyoutOpen && !isContextMenuOpen) {
            controlsIdleTimer += elapsedDt;
            const float idleLimit = player.IsPlaying() ? 1.2f : 2.0f;
            if (controlsIdleTimer >= idleLimit) { targetControlsOpacity = 0.0f; }
        } else { controlsIdleTimer = 0.0f; targetControlsOpacity = 1.0f; }
        if (IsReducedMotionEnabled()) {
            if (controlsOpacity != targetControlsOpacity) { controlsOpacity = targetControlsOpacity; needsRedraw = true; }
        } else if (std::abs(controlsOpacity - targetControlsOpacity) > 0.005f) {
            controlsOpacity += (targetControlsOpacity - controlsOpacity) * std::min(1.0f, dt * 6.0f);
            needsRedraw = true;
        } else if (controlsOpacity != targetControlsOpacity) { controlsOpacity = targetControlsOpacity; needsRedraw = true; }
        // Hide mouse cursor when controls fade out completely
        if (controlsOpacity <= 0.02f && !isSettingsFlyoutOpen && !isContextMenuOpen && !isScrubbing && !isVolumeScrubbing) {
            if (!isCursorHidden) { isCursorHidden = true; SetCursor(NULL); }
            hoveredPlayerButtonId = 0;
            isBackHovered = false;
            isVolumeHovered = false;
        } else {
            if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        }
        if (player.IsPlaying()) {
            // mpv presents video independently. Once the controls disappear,
            // an empty layered overlay needs no per-frame clear or upload.
            // Subtitle timing, diagnostics and visible controls stay live;
            // transient animations request their own redraws below.
            if (overlayRenderer.ShouldRefreshPlayback(controlsOpacity > 0.0f || showPlaybackDiagnostics ||
                isSettingsFlyoutOpen || isContextMenuOpen, subtitles.GetActiveTrack() ?
                subtitles.GetCurrentSubtitleText(player.GetCurrentTime(), settings.subDelayMs) : std::wstring{})) needsRedraw = true;
            progressSaveTimerSec += elapsedDt;
            if (progressSaveTimerSec >= 5.0f) { progressSaveTimerSec = 0.0f; QueuePlaybackProgressSave(); SaveSession(); }
        }
        // Jump ahead chapter detection
        if (player.IsLoaded()) {
            int curChap = player.GetCurrentChapter();
            const std::vector<MpvChapterInfo>& chapters = GetCachedChapters();
            double curTime = player.GetCurrentTime();
            double duration = player.GetDuration();
            if (curChap < 0 && !chapters.empty() && duration > 0.0) {
                for (int i = (int)chapters.size() - 1; i >= 0; --i) {
                    if (chapters[i].time <= curTime + 0.25) { curChap = i; break; }
                }
            }
            if (curChap != jumpAheadActiveChapterIndex) {
                jumpAheadActiveChapterIndex = curChap;
                double targetTime = 0.0;
                if (curChap >= 0 && IsQualifyingJumpAheadChapter((size_t)curChap, targetTime)) {
                    jumpAheadTargetTime = targetTime;
                    isJumpAheadVisible = true;
                    jumpAheadTimer = 3.0f;
                    AnimateVisual(jumpAheadAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
                } else {
                    isJumpAheadVisible = false;
                    jumpAheadTimer = 0.0f;
                    AnimateVisual(jumpAheadAnim, 0.0f, MotionTokens::kFast, MotionTokens::Standard());
                }
            } else if (isJumpAheadVisible) {
                if (!isJumpAheadHovered && !isJumpAheadPressed) {
                    jumpAheadTimer -= elapsedDt;
                    if (jumpAheadTimer <= 0.0f) {
                        isJumpAheadVisible = false;
                        jumpAheadTimer = 0.0f;
                        AnimateVisual(jumpAheadAnim, 0.0f, MotionTokens::kFast, MotionTokens::Standard());
                    }
                }
            }
        }
    } else {
        EnsureMediaLayoutCache();
        if (!m_activeAnimRowIndices.empty()) {
            size_t remainingCount = 0;
            const auto& visibleIndices = GetVisibleMediaIndices();
            for (size_t idx : m_activeAnimRowIndices) {
                if (idx >= rowAnimStates.size()) continue;
                auto& row = rowAnimStates[idx];
                bool isPinned = (!isLastWatchedCleared && idx == 0 && libraryGroupMode == LibraryGroupMode::Flat &&
                    (!visibleIndices.empty() && visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0));
                if (!IsAutoscrolling() && activeDragType == DragType::None && !touchPressSuppressesHoverExpand &&
                    row.isHovering && row.hoverEnterTime >= 0.0f) {
                    if ((NowSeconds() - row.hoverEnterTime) >= MotionTokens::kHoverRevealDelay) {
                        const float expandedH = isPinned ? kMediaPinnedRestHeight : 180.0f;
                        if (!isPinned && std::abs(row.height.to - expandedH) > 0.01f) {
                            AnimateVisual(row.height, expandedH, MotionTokens::kPreview, MotionTokens::Standard());
                            InvalidateMediaLayoutCache(idx);
                        }
                        if (!row.previewOpacity.animating && row.previewOpacity.to < 1.0f) {
                            AnimateVisual(row.liftY, 2.0f, MotionTokens::kReveal, MotionTokens::Spring());
                            AnimateVisual(row.previewOpacity, 1.0f, MotionTokens::kPreview, MotionTokens::Spring());
                        }
                        if (settings.hoverPreviewEnabled && idx < visibleIndices.size()) {
                            size_t origIdx = visibleIndices[idx];
                            if (origIdx < library.mediaItems.size()) {
                                const auto& item = library.mediaItems[origIdx];
                                const bool canPreview = previewInitialized || (!previewInitAttempted && EnsurePreviewInitialized());
                                if (canPreview && previewPlayer.GetCurrentPreviewPath() != item.filePath) {
                                    // The fixed row uses the live frame as a subtle backdrop. Cap its buffer to keep preview work modest
                                    // while retaining a clean image.
                                    RECT prc;
                                    if (GetClientRect(hWnd, &prc) && prc.right > prc.left) {
                                        const float pdp = std::max(0.5f, renderer.GetDpiScale());
                                        const float rowPxW = std::max(0.0f, (float)(prc.right - prc.left) - 24.0f * pdp);
                                        int wantW = (int)std::clamp(rowPxW, 640.0f, 960.0f);
                                        int wantH = wantW * 9 / 16;
                                        previewPlayer.SetPreviewOutputSize(wantW, wantH);
                                    }
                                    double startSec = 0.0;
                                    if (item.lastPosSec > 0.0) {
                                        startSec = item.lastPosSec;
                                    } else if (item.durationSec > 5.0) { startSec = std::min(item.durationSec * 0.1, 10.0); }
                                    previewPlayer.StartPreview(item.filePath, startSec);
                                }
                            }
                        }
                    }
                } else {
                    if (idx < visibleIndices.size()) {
                        size_t origIdx = visibleIndices[idx];
                        if (origIdx < library.mediaItems.size()) {
                            if (previewPlayer.GetCurrentPreviewPath() == library.mediaItems[origIdx].filePath)
                                previewPlayer.StopPreview();
                        }
                    }
                }
                if (row.height.Tick()) { needsRedraw = true; InvalidateMediaLayoutCache(idx); }
                if (row.liftY.Tick()) needsRedraw = true;
                if (row.previewOpacity.Tick()) needsRedraw = true;
                bool stillActive = row.height.animating || row.liftY.animating || row.previewOpacity.animating ||
                                   (!IsAutoscrolling() && row.isHovering && row.hoverEnterTime >= 0.0f);
                if (stillActive) { m_activeAnimRowIndices[remainingCount++] = idx; } else { row.inActiveAnimList = false; }
            }
            m_activeAnimRowIndices.resize(remainingCount);
        }
        if (previewPlayer.IsActive() && (currentTab == TabView::Media || currentTab == TabView::Playlists)) {
            previewLastActivity = NowSeconds();
            if (previewPlayer.UpdateFrame(renderer.GetD2DContext())) { needsRedraw = true; }
        }
        // Playlist "Continue Watching" card hover preview (same behavior as the media-row expanded preview).
        if (currentTab == TabView::Playlists && isPlaylistResumeHovered && !playbackSession.IsOpen() &&
            !touchPressSuppressesHoverExpand && !resumePreviewPath.empty()) {
            if (resumeHoverEnter < 0.0) resumeHoverEnter = NowSeconds();
            if (!resumePreviewStarted && (NowSeconds() - resumeHoverEnter) >= MotionTokens::kHoverRevealDelay) {
                const bool canPreview = previewInitialized || (!previewInitAttempted && EnsurePreviewInitialized());
                if (canPreview) {
                    RECT prc;
                    if (GetClientRect(hWnd, &prc) && prc.right > prc.left) {
                        const float pdp = std::max(0.5f, renderer.GetDpiScale());
                        const float rowPxW = std::max(0.0f, (float)(prc.right - prc.left) - 24.0f * pdp);
                        previewPlayer.SetPreviewOutputSize((int)std::clamp(rowPxW, 640.0f, 1280.0f), (int)std::clamp(rowPxW, 640.0f, 1280.0f) * 9 / 16);
                    }
                    if (previewPlayer.GetCurrentPreviewPath() != resumePreviewPath)
                        previewPlayer.StartPreview(resumePreviewPath, resumeResumePos);
                    resumePreviewStarted = true;
                }
            }
            if (resumePreviewStarted && !resumePreviewAnim.animating && resumePreviewAnim.to < 1.0f) {
                AnimateVisual(resumePreviewAnim, 1.0f, MotionTokens::kFast, MotionTokens::Standard());
            }
        } else {
            if (resumeHoverEnter >= 0.0) resumeHoverEnter = -1.0;
            if (resumePreviewStarted) {
                resumePreviewStarted = false;
                if (previewPlayer.GetCurrentPreviewPath() == resumePreviewPath) { previewPlayer.StopPreview(); }
            }
            if (resumePreviewAnim.current > 0.0f) {
                AnimateVisual(resumePreviewAnim, 0.0f, MotionTokens::kResponsive, MotionTokens::Standard());
            }
        }
        // Playlist episode card hover expansion (1-second hover intent in grid view).
        if (currentTab == TabView::Playlists && playlistGridView && !playbackSession.IsOpen() &&
            playlistCardHoverIndex >= 0 && playlistCardHoverEnterTime >= 0.0f) {
            if ((NowSeconds() - playlistCardHoverEnterTime) >= kPlaylistCardHoverDelay) {
                auto& anim = playlistCardExpands[static_cast<size_t>(playlistCardHoverIndex)];
                if (!anim.animating && anim.to < 1.0f) {
                    AnimateVisual(anim, 1.0f, MotionTokens::kReveal, MotionTokens::Standard());
                }
            }
        }
        for (auto it = playlistCardExpands.begin(); it != playlistCardExpands.end(); ) {
            if (it->second.Tick()) needsRedraw = true;
            if (!it->second.animating && it->second.current <= 0.0001f &&
                (playlistCardHoverIndex < 0 || it->first != static_cast<size_t>(playlistCardHoverIndex))) {
                it = playlistCardExpands.erase(it);
            } else {
                ++it;
            }
        }
        if (DriveHubExtraction()) needsRedraw = true;
        if (renderer.HasDeferredThumbnailUploads()) needsRedraw = true;
        MaybePrefetchVisibleThumbnails();
        // No unconditional idle prewarm: an idle hub must not load libmpv or create main/preview mpv instances merely to make the first
        // hover faster. The preview initializes on first hover-intent (see hover path calling EnsurePreviewInitialized), where it loads its
        // own DLL mapping independently of the main player.
        const auto updateIndicator = [&](AnimatedFloat& indicator, bool& ready, float target) {
            if (!ready) { indicator.SnapTo(target); ready = true; }
            else if (std::abs(indicator.to - target) > 0.001f) {
                AnimateVisual(indicator, target, 0.34f, &kEaseSpring);
                needsRedraw = true;
            }
        };
        if (currentTab != TabView::Settings)
            updateIndicator(navIndicator, navIndicatorReady, static_cast<float>(currentTab));
        if (currentTab == TabView::Settings)
            updateIndicator(settingsCatIndicator, settingsCatIndicatorReady, static_cast<float>(currentSettingsCat));
        if (currentTab == TabView::Media)
            updateIndicator(groupModeIndicator, groupModeIndicatorReady,
                libraryGroupMode == LibraryGroupMode::Flat ? 0.0f
                : (libraryGroupMode == LibraryGroupMode::Listen ? 1.0f : 0.5f));
        if (currentTab == TabView::Playlists) {
            size_t selected = static_cast<size_t>(playlistRailIndicator.to);
            if (selected >= library.playlists.size() || library.playlists[selected].id != activePlaylistId) {
                const auto active = std::find_if(library.playlists.begin(), library.playlists.end(),
                    [&](const Playlist& playlist) { return playlist.id == activePlaylistId; });
                selected = active == library.playlists.end() ? 0 : static_cast<size_t>(active - library.playlists.begin());
            }
            updateIndicator(playlistRailIndicator, playlistRailIndicatorReady, static_cast<float>(selected));
            updateIndicator(playlistViewModeIndicator, playlistViewModeIndicatorReady, playlistGridView ? 1.0f : 0.0f);
        }
        for (auto* indicator : {&navIndicator, &settingsCatIndicator, &playlistRailIndicator, &groupModeIndicator, &playlistViewModeIndicator})
            if (indicator->Tick()) needsRedraw = true;
        if (navMediaTabHover.Tick()) needsRedraw = true;
        if (navPlaylistTabHover.Tick()) needsRedraw = true;
        if (navHistoryTabHover.Tick()) needsRedraw = true;
        if (navSettingsTabHover.Tick()) needsRedraw = true;
        if (navSettingsRotation.Tick()) needsRedraw = true;
        if (navOpenFileLift.Tick()) needsRedraw = true;
        if (autoplayToggleAnim.Tick()) needsRedraw = true;
        if (rememberPosToggleAnim.Tick()) needsRedraw = true;
        if (hwAccelToggleAnim.Tick()) needsRedraw = true;
        if (loopToggleAnim.Tick()) needsRedraw = true;
        if (customMpvConfigToggleAnim.Tick()) needsRedraw = true;
        // Experience feature toggles ease toward the enabled state of their
        // catalog feature; the target re-resolves whenever the mode changes.
        // Index by catalog position (render order), never by FeatureId.
        for (int i = 0; i < 40; ++i) {
            const bool enabled = FeatureGate::FeatureEnabledAt(FeatureGate::ModeFromString(settings.experienceMode),
                                                               settings.customEnabledFeatures, static_cast<size_t>(i));
            const float toggleTarget = enabled ? 1.0f : 0.0f;
            if (std::abs(experienceToggleAnim[i].to - toggleTarget) > 0.001f) {
                AnimateVisual(experienceToggleAnim[i], toggleTarget, MotionTokens::kToggle, MotionTokens::Standard());
            }
            if (experienceToggleAnim[i].Tick()) needsRedraw = true;
        }
        // System & Storage rows ease toward their hover/press targets here; the
        // pressed flag itself flips inside the mouse handlers and renders
        // instantly so press feedback is immediate, like the nav bar.
        for (int i = 0; i < SettingsLayout::kSystemRowCount; ++i) {
            const float hoverTarget = (currentTab == TabView::Settings &&
                                       currentSettingsCat == SettingsCategory::System &&
                                       hoveredSettingsRow == i) ? 1.0f : 0.0f;
            const float prevHover = systemRowHoverAnim[i].current;
            if (std::abs(systemRowHoverAnim[i].to - hoverTarget) > 0.001f) {
                AnimateVisual(systemRowHoverAnim[i], hoverTarget, MotionTokens::kFast, MotionTokens::Standard());
            }
            if (systemRowHoverAnim[i].Tick()) needsRedraw = true;
            // Reduce Motion snaps the target without an animating frame. The
            // WM_PAINT dispatched alongside the pointer message has already
            // presented the previous state, so a snapped current still owes
            // the screen a repaint or the last hover tint freezes in place.
            if (systemRowHoverAnim[i].current != prevHover) needsRedraw = true;
            const float prevPress = systemRowPressAnim[i].current;
            const float pressTarget = settingsPressState.IsSystemActionPressed(i) ? 1.0f : 0.0f;
            if (std::abs(systemRowPressAnim[i].to - pressTarget) > 0.001f) {
                AnimateVisual(systemRowPressAnim[i], pressTarget, MotionTokens::kFast, MotionTokens::Standard());
            }
            if (systemRowPressAnim[i].Tick()) needsRedraw = true;
            if (systemRowPressAnim[i].current != prevPress) needsRedraw = true;
        }
    }
    if (previewInitialized && !previewPlayer.IsActive() && NowSeconds() - previewLastActivity > 12.0) {
        // Apply the idle policy during playback too: opening a movie stops
        // hub previews, but their decoder and frame cache otherwise stay warm.
        previewPlayer.Shutdown();
        previewInitialized = false;
        previewInitAttempted = false;
    }
    // Non-blocking notification timer. Notifications from the same channel
    // coalesce in ShowOSD; distinct notifications are presented in order.
    if (osdTimer > 0.0f) { osdTimer = std::max(0.0f, osdTimer - elapsedDt); needsRedraw = true; }
    if (!pendingCoalescedAnnouncement.empty() && (GetTickCount64() - lastVolumeOrSeekAnnounceMs >= 150)) {
        std::wstring msg = std::move(pendingCoalescedAnnouncement);
        pendingCoalescedAnnouncement.clear();
        AnnounceStatus(msg, true);
    }
    if (osdTimer == 0.0f && !notificationQueue.empty()) {
        PendingNotification next = std::move(notificationQueue.front());
        notificationQueue.pop_front();
        osdIcon = next.icon;
        osdText = std::move(next.text);
        osdTimer = osdDuration = next.durationSec;
        needsRedraw = true;
    }
    // Double-tap animation state. Each press owns an independent chevron slide, while sign and number pops restart from zero just like the
    // HTML Web Animations calls.
    auto advanceChevronSlides = [dt](std::vector<float>& slides) {
        for (float& progress : slides) { progress = std::min(1.0f, progress + dt / kSeekOsdChevronSec); }
        slides.erase(std::remove_if(slides.begin(), slides.end(), [](float progress) { return progress >= 1.0f; }), slides.end());
    };
    if (doubleTapTimer > 0.0f) {
        doubleTapTimer = std::max(0.0f, doubleTapTimer - elapsedDt);
        if (IsReducedMotionEnabled()) {
            doubleTapSignPop = 1.0f;
            doubleTapNumberPop = 1.0f;
            doubleTapChevronSlides.clear();
            doubleTapAppear = 1.0f;
            doubleTapFadeValid = false;
        } else {
            doubleTapSignPop = std::min(1.0f, doubleTapSignPop + dt / kSeekOsdSignSec);
            doubleTapNumberPop = std::min(1.0f, doubleTapNumberPop + dt / kSeekOsdNumberSec);
            advanceChevronSlides(doubleTapChevronSlides);
            doubleTapAppear = std::min(1.0f, doubleTapAppear + dt / kSeekOsdAppearSec);
        }
        if (doubleTapTimer <= 0.0f) { doubleTapAccumSec = 0.0; doubleTapPresses = 0; doubleTapChevronSlides.clear(); }
        needsRedraw = true;
    }
    if (doubleTapFadeValid) {
        if (IsReducedMotionEnabled()) { doubleTapFadeValid = false; } else {
            doubleTapFadeProgress = std::min( 1.0f, doubleTapFadeProgress + dt / kSeekOsdFastFadeSec);
            doubleTapFadeSignPop = std::min(1.0f, doubleTapFadeSignPop + dt / kSeekOsdSignSec);
            doubleTapFadeNumberPop = std::min(1.0f, doubleTapFadeNumberPop + dt / kSeekOsdNumberSec);
            doubleTapFadeAppear = std::min(1.0f, doubleTapFadeAppear + dt / kSeekOsdAppearSec);
            advanceChevronSlides(doubleTapFadeChevronSlides);
            if (doubleTapFadeProgress >= 1.0f) { doubleTapFadeValid = false; doubleTapFadeChevronSlides.clear(); }
            needsRedraw = true;
        }
    }
    // Play/Pause badge timer
    if (playPauseAnimTimer > 0.0f) { playPauseAnimTimer -= dt; needsRedraw = true; }
    if (contextMenuAnim.Tick()) needsRedraw = true;
    if (TickSplitIrisAnimation(dt)) needsRedraw = true;
    if (flyoutAnim.Tick()) needsRedraw = true;
    if (pipVolumeAnim.Tick()) needsRedraw = true;
    if (resumePreviewAnim.Tick()) needsRedraw = true;
    // Retire menu close fades once they land (also covers reduced-motion snaps, where the anim never reports animating).
    if (contextMenuClosing && !contextMenuAnim.animating) { contextMenuClosing = false; needsRedraw = true; }
    if (flyoutClosing && !flyoutAnim.animating) { flyoutClosing = false; needsRedraw = true; }
    // Timeline hover & knob expansion animation
    const bool timelineActive = (isTimelineHovered || isScrubbing);
    const float targetHover = timelineActive ? 1.0f : 0.0f;
    if (IsReducedMotionEnabled()) {
        timelineHoverAnim.SnapTo(targetHover);
    } else if (std::abs(timelineHoverAnim.to - targetHover) > 0.001f) {
        AnimateVisual(timelineHoverAnim, targetHover, MotionTokens::kFast, MotionTokens::Standard());
    }
    if (timelineHoverAnim.Tick()) needsRedraw = true;
    if (jumpAheadAnim.Tick()) needsRedraw = true;
    if (playbackSession.IsOpen()) {
        if (needsRedraw) overlayRenderer.RequestRedraw();
        return overlayRenderer.IsFrameDue();
    }
    return needsRedraw;
}

bool App::RenderHubFrame() {
    if (renderer.AreDeviceWorkersRunning()) return false;
    renderer.BeginDraw();
    if (!renderer.GetD2DContext()) return false;
    previewPlayer.BindDeviceContext(renderer.GetD2DContext());
    HubRenderViewModel vm = BuildHubRenderViewModel();
    for (int i = 0; i < 40; ++i) vm.settingsTab.experienceToggleAnim[i] = experienceToggleAnim[i].current;
    UIComponents::DrawHub(renderer, settings, vm);
    settingsDescTools = std::move(vm.settingsTab.descTools);
    return renderer.EndDraw() && !renderer.IsOccluded();
}

void App::Render() {
    if (renderer.AreDeviceWorkersRunning()) return;
    if (playbackSession.IsOpen()) {
        if (hubTooltips.IsActive()) hubTooltips.SetActive(false);
        if (!EnsureOverlayReady()) return;
        RECT clientScreenRect{};
        if (!GetClientScreenRect(hWnd, clientScreenRect)) return;
        if (!overlayRenderer.BeginFrame(hOverlayWnd)) return;
        // Rejected input-driven frames retain a pending timer wake. Refresh
        // derived UI state once for the accepted frame using the latest input.
        RefreshAccessibilityTree(false);
        RefreshPlayerTooltips();
        PlayerRenderViewModel vm = BuildPlayerRenderViewModel();
        UIComponents::DrawPlayerOverlay(overlayRenderer, settings, vm);
        const bool presented = overlayRenderer.EndDrawAndPresent(hOverlayWnd, clientScreenRect.left, clientScreenRect.top);
        if (presented && controlsOpacity <= 0.0f && (!vm.subtitle.visible || vm.subtitle.text.empty()))
            overlayRenderer.ReleaseEmptySurface();
        return;
    }
    RefreshAccessibilityTree(false);
    playerTooltips.SetActive(false);
    playerTooltips.HideForKeyboard();
    RefreshHubTooltips();
    // --- Hub Library View (Rendered directly on main window) ---
    if (!RenderHubFrame()) return;
    // The first refresh above predates this frame's render; run it again so
    // tools laid out during the frame (e.g. compact Settings descriptions)
    // are registered at their final positions.
    RefreshHubTooltips();
    // Reveal a complete hub frame before removing playback surfaces.
    if (hVideoWnd && IsWindowVisible(hVideoWnd)) ShowWindow(hVideoWnd, SW_HIDE);
    if (hOverlayWnd && IsWindowVisible(hOverlayWnd)) ShowWindow(hOverlayWnd, SW_HIDE);
    if (!firstGpuShellRecorded) { firstGpuShellRecorded = true; Diagnostics::RecordStartupMilestone(L"first-gpu-shell", false); }
    if (libraryReady && !librarySnapshotFrameRendered) {
        librarySnapshotFrameRendered = true;
        Diagnostics::RecordStartupMilestone(L"first-populated-frame", false);
    }
}

void App::PrepareVisualCaptureScene(const std::wstring& scene) {
    // Interaction fixtures are opt-in and must use an isolated executable/profile.
    if (scene.rfind(L"reorder_", 0) == 0 && FileExists(GetExeDir() + L"\\reorder.fixture")) {
        const bool playlist = scene.find(L"grid") != std::wstring::npos || scene.find(L"list") != std::wstring::npos;
        SetTab(playlist ? TabView::Playlists : TabView::Media);
        libraryGroupMode = LibraryGroupMode::Flat;
        isLastWatchedCleared = true;
        playlistGridView = scene.find(L"grid") != std::wstring::npos;
        if (playlist && !library.playlists.empty()) activePlaylistId = library.playlists[0].id;
        auto vm = BuildHubRenderViewModel();
        D2D1_RECT_F source{}, target{};
        if (playlist) {
            if (vm.playlistsTab.detail.visibleEpisodes.size() < 4) return;
            source = vm.playlistsTab.detail.visibleEpisodes[0].itemRect;
            target = vm.playlistsTab.detail.visibleEpisodes[3].itemRect;
        } else {
            if (vm.mediaTab.visibleCards.size() < 3) return;
            source = vm.mediaTab.visibleCards[0].itemRect;
            target = vm.mediaTab.visibleCards[2].itemRect;
        }
        const float dp = renderer.GetDpiScale();
        auto position = [dp](D2D1_RECT_F rect) { return MAKELPARAM(static_cast<short>(rect.left + 70 * dp), static_cast<short>(rect.top + 20 * dp)); };
        SendMessage(hWnd, WM_MOUSEMOVE, 0, position(source));
        SendMessage(hWnd, WM_LBUTTONDOWN, MK_LBUTTON, position(source));
        if (scene.find(L"click") == std::wstring::npos) { SendMessage(hWnd, WM_MOUSEMOVE, MK_LBUTTON, position(target)); }
        for (int frame = 0; frame < 24; ++frame) { Update(1.0f / 60); BuildHubRenderViewModel(); }
        if (scene.find(L"cancel") != std::wstring::npos) SendMessage(hWnd, WM_KEYDOWN, VK_ESCAPE, 0);
        else if (scene.find(L"capturelost") != std::wstring::npos) SendMessage(hWnd, WM_CAPTURECHANGED, 0, 0);
        else if (scene.find(L"drag") == std::wstring::npos) { SendMessage(hWnd, WM_LBUTTONUP, 0, position(target)); }
        return;
    }
    if (scene == L"modal_recovery") {
        ShowConfirmationModal(Loc::Tr("ux.storage_title"), Loc::Tr("ux.storage_permission"), Loc::Tr("ux.open_folder"), nullptr, nullptr, false);
    } else if (scene == L"modal_playback") {
        ShowConfirmationModal(Loc::Tr("ux.playback_title"), Loc::Tr("ux.playback_network"), Loc::Tr("ux.retry"), nullptr, nullptr, false);
    } else if (scene == L"modal_confirm") {
        ShowConfirmationModal(L"Remove from library", L"Remove these items from your library? Your media files stay on disk.", L"Remove", nullptr);
    } else if (scene == L"modal_input") { ShowNewPlaylistModal(); }
}

LRESULT App::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (closeRequested && uMsg != WM_CLOSE && uMsg != WM_DESTROY && uMsg != WM_NCDESTROY &&
        uMsg != WM_PAINT && uMsg != WM_ERASEBKGND) return DefWindowProc(hwnd, uMsg, wParam, lParam);
    float dp = renderer.GetDpiScale();
    // Bootstrap shell (window visible before Initialize): no taskbar, tooltip,
    // playback, or worker dispatch until appInitialized. WM_PAINT/WM_ERASEBKGND
    // take the GDI bootstrap branch below; everything else defers to the
    // guarded switch (paint/close/size) or DefWindowProc.
    const bool bootstrapping = !appInitialized;
    if (!bootstrapping && HandlePlaybackSpeedDragMessage(uMsg, lParam)) return 0;
    if (!bootstrapping && taskbarController.HandleWindowMessage(hwnd, uMsg, wParam, lParam, commandDispatcher)) { return 0; }
    const bool relayTooltipMouse = !bootstrapping && (uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSELEAVE ||
        uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK ||
        uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP);
    if (relayTooltipMouse) {
        HideTooltipKeyboardFocus();
        if (!playbackSession.IsOpen() && !IsAutoscrolling()) hubTooltips.RelayMouseMessage(uMsg, wParam, lParam);
    }
    switch (uMsg) {
        case WM_NCCALCSIZE:
            // PiP is a borderless card: keep WS_THICKFRAME for the native sizing services,
            // but give the client the full window rect so the video reaches every edge.
            // Leaving the default inset would show the window background as a grey band
            // around the video (top) and a gap on the right.
            if (wParam && playbackSession.IsPiP()) return 0;
            break;
        case WM_NCHITTEST:
            // Rebuild the resize borders that WM_NCCALCSIZE removed above.
            if (playbackSession.IsPiP()) {
                const POINT hitPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT rc{};
                if (GetWindowRect(hwnd, &rc)) {
                    const LONG zone = (LONG)(8.0f * dp);
                    const bool left = hitPt.x < rc.left + zone;
                    const bool right = hitPt.x >= rc.right - zone;
                    const bool top = hitPt.y < rc.top + zone;
                    const bool bottom = hitPt.y >= rc.bottom - zone;
                    if (left && top) return HTTOPLEFT;
                    if (right && top) return HTTOPRIGHT;
                    if (left && bottom) return HTBOTTOMLEFT;
                    if (right && bottom) return HTBOTTOMRIGHT;
                    if (left) return HTLEFT;
                    if (right) return HTRIGHT;
                    if (top) return HTTOP;
                    if (bottom) return HTBOTTOM;
                }
                return HTCLIENT;
            }
            break;
        case WM_GETOBJECT:
            if (bootstrapping) break;
            if (static_cast<LONG>(lParam) == OBJID_CLIENT || static_cast<LONG>(lParam) == static_cast<LONG>(UiaRootObjectId)) {
                RefreshAccessibilityTree(true);
                return accessibilityTree.HandleGetObject(wParam, lParam);
            }
            break;
        case kAccessibilityActionMessage:
            if (bootstrapping) return 0;
            InvokeAccessibilityAction(static_cast<LONG>(wParam));
            return 0;
        case kAccessibilityAnnouncementMessage:
            if (bootstrapping) return 0;
            ProcessPendingAnnouncements();
            return 0;
        case kPlayerEventMessage:
            if (bootstrapping) return 0;
            HandlePlayerEvents();
            return 0;
        case kLibrarySnapshotReadyMessage:
            if (bootstrapping) return 0;
            FinishInitialLibraryLoad(static_cast<uint64_t>(wParam));
            return 0;
        case Renderer::kThumbnailReadyMessage:
            if (bootstrapping) return 0;
            renderer.AcknowledgeThumbnailNotification();
            if (!thumbnailRefreshTimerPending) { thumbnailRefreshTimerPending = true; SetTimer(hWnd, kThumbnailRefreshTimer, 20, nullptr); }
            return 0;
        case kFolderWatchEventMessage:
            if (bootstrapping) return 0;
            watchEventPosted.store(false);
            HandleFolderWatchEvents();
            return 0;
        case kBackfillEventMessage:
            if (bootstrapping) return 0;
            FinishBackfillJob();
            return 0;
        case MediaTransport::kMediaTransportEventMessage:
            if (bootstrapping) return 0;
            mediaTransport.ProcessPendingCommands(commandDispatcher);
            return 0;
        case WM_HOTKEY: {
            if (bootstrapping) return 0;
            // id-0x9000 -> fixed action index (see kGlobalHotkeyActions).
            const UINT hotkeyIndex = static_cast<UINT>(wParam) - kGlobalHotkeyBase;
            if (hotkeyIndex < kGlobalHotkeyCount) {
                commandDispatcher.Execute(CommandDispatcher::CommandIdFromString(kGlobalHotkeyActions[hotkeyIndex]));
            }
            return 0;
        }
        case kTrayCallbackMessage: {
            if (bootstrapping) return 0;
            const UINT trayEvent = LOWORD(lParam);
            if (trayEvent == WM_LBUTTONUP || trayEvent == WM_LBUTTONDBLCLK) { RestoreFromTray(); }
            else if (trayEvent == WM_RBUTTONUP) { ShowTrayContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); }
            return 0;
        }
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            // Deferred until after Initialize: settings are not loaded yet and
            // ApplyWindowTheme would overwrite them. Initialize refreshes once.
            if (bootstrapping) return 0;
            // SystemParametersInfo/GetSysColor are cheap, but only rebuild the palette/layout when the normalized snapshot changed. This
            // also handles external high-contrast entry/exit without touching the persisted Settings theme or accent values.
            const uint32_t changes = systemVisualPreferences.Refresh();
            if (changes & (SystemVisualChangeAccessibility | SystemVisualChangeColors)) {
                ApplyWindowTheme();
                const float textScale = systemVisualPreferences.Get().textScaleFactor;
                renderer.SetTextScale(textScale);
                if (overlayReady) overlayRenderer.SetTextScale(textScale);
                InvalidateMediaLayoutCache();
            }
            if (changes & SystemVisualChangeMotion) { SnapVisualAnimations(); }
            if (changes != SystemVisualChangeNone) {
                accessibilityDirty = true;
                InvalidateRect(hwnd, NULL, FALSE);
                if (playbackSession.IsOpen()) Render();
            }
            return 0;
        }
        case WM_ERASEBKGND:
            // The flip-model swap chain owns the whole client area after the first present. Before that, paint the theme background so the
            // very first visible pixels are never an uninitialized white flash.
            if (!renderer.HasPresentedFrame() && hStartupBgBrush) {
                RECT rcErase = { 0, 0, 0, 0 };
                GetClientRect(hwnd, &rcErase);
                if (rcErase.right > rcErase.left && rcErase.bottom > rcErase.top) { FillRect((HDC)wParam, &rcErase, hStartupBgBrush); }
            }
            return 1;
        case WM_ENTERSIZEMOVE: {
            isUserSizingMoving = true;
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            isUserSizingMoving = false;
            if (playbackSession.IsPiP() && !inPresentationTransition) {
                PersistPiPGeometry();
            } else if (!playbackSession.IsFullscreen() && !playbackSession.IsPiP() && !inPresentationTransition) {
                PersistWindowPlacement();
            }
            return 0;
        }
        case WM_SIZE: {
            if (activeDragType != DragType::None || pendingDragType != DragType::None) CancelCardDrag();
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            // During bootstrap the window exists but settings/library are not
            // loaded: resize the renderer/video surface only. Never persist
            // placement (settings would be overwritten with defaults).
            if (bootstrapping) {
                renderer.Resize(w, h);
                if (hVideoWnd) { SetWindowPos(hVideoWnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE); }
                return 0;
            }
            if (wParam == SIZE_MINIMIZED) {
                // While minimized and not playing there is nothing time-
                // critical to do; let the scheduler run the process in
                // EcoQoS until the window comes back.
                isMinimized = true;
                if (hOverlayWnd) ShowWindow(hOverlayWnd, SW_HIDE);
                SetEcoThrottling(!player.IsPlaying());
                UpdateDisplayPowerRequest();
                // Minimize-to-tray: hide after the minimize so the taskbar
                // button disappears; RestoreFromTray undoes it with SW_RESTORE,
                // which returns the pre-minimize placement.
                if (settings.trayIconEnabled && settings.minimizeToTray && !closeRequested) {
                    isMinimizedToTray = true;
                    ShowWindow(hWnd, SW_HIDE);
                }
            } else if (isMinimized) {
                isMinimized = false;
                SetEcoThrottling(false);
                UpdateDisplayPowerRequest();
                if (playbackSession.IsOpen() && hOverlayWnd) { ShowWindow(hOverlayWnd, SW_SHOW); SyncOverlayPosition(); }
            }
            renderer.Resize(w, h);
            if (hVideoWnd) { SetWindowPos(hVideoWnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE); player.SetVideoBounds(0, 0, w, h); }
            if (playbackSession.IsOpen()) { SyncOverlayPosition(); }
            if (!inPresentationTransition) {
                if (playbackSession.IsPiP()) {
                    if (isUserSizingMoving) {
                        PersistPiPGeometry();
                    }
                } else if (!playbackSession.IsFullscreen() && w > 0 && h > 0) {
                    PersistWindowPlacement();
                }
            }
            ClampScrollOffsets();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_MOVE: {
            if (bootstrapping) return 0;
            if (playbackSession.IsOpen()) { SyncOverlayPosition(); }
            if (!inPresentationTransition) {
                if (playbackSession.IsPiP()) {
                    if (isUserSizingMoving || isPiPDraggingWindow) {
                        PersistPiPGeometry();
                    }
                } else if (!playbackSession.IsFullscreen()) {
                    PersistWindowPlacement();
                }
            }
            return 0;
        }
        case WM_DPICHANGED: {
            float newDpi = (float)LOWORD(wParam);
            renderer.SetDpi(newDpi);
            if (bootstrapping) {
                RECT* prcNewWindow = (RECT*)lParam;
                if (prcNewWindow) {
                    SetWindowPos(hWnd, NULL, prcNewWindow->left, prcNewWindow->top, std::max(1L, prcNewWindow->right - prcNewWindow->left),
                        std::max(1L, prcNewWindow->bottom - prcNewWindow->top), SWP_NOZORDER | SWP_NOACTIVATE);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            overlayRenderer.SetDpi(newDpi / 96.0f);
            RECT* prcNewWindow = (RECT*)lParam;
            if (prcNewWindow && !playbackSession.IsFullscreen() && !playbackSession.IsPiP()) {
                SetWindowPos(hWnd, NULL, prcNewWindow->left, prcNewWindow->top, std::max(1L, prcNewWindow->right - prcNewWindow->left),
                    std::max(1L, prcNewWindow->bottom - prcNewWindow->top), SWP_NOZORDER | SWP_NOACTIVATE);
            }
            SyncOverlayPosition();
            ClampScrollOffsets();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                UINT dpi = 96;
                using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
                HMODULE user32 = GetModuleHandleW(L"user32.dll");
                auto getDpi = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                if (getDpi && hWnd) dpi = getDpi(hWnd);
                if (playbackSession.IsPiP()) {
                    // PiP is a compact playback surface. The normal hub minimum made the nominal 480x270 PiP snap larger as soon as the
                    // user attempted to resize it.
                    mmi->ptMinTrackSize.x = MulDiv(300, (int)dpi, 96);
                    mmi->ptMinTrackSize.y = MulDiv(169, (int)dpi, 96);
                } else { mmi->ptMinTrackSize.x = MulDiv(640, (int)dpi, 96); mmi->ptMinTrackSize.y = MulDiv(420, (int)dpi, 96); }
            }
            return 0;
        }
        case WM_DROPFILES: { HDROP hDrop = (HDROP)wParam; isDragActive = false; HandleDroppedFiles(hDrop); return 0; }
        case WM_COPYDATA: {
            PCOPYDATASTRUCT pCDS = (PCOPYDATASTRUCT)lParam;
            if (pCDS && pCDS->dwData == 0x41505452 /* 'APTR' */ && pCDS->lpData && pCDS->cbData > 0) {
                const wchar_t* pText = (const wchar_t*)pCDS->lpData;
                size_t numChars = pCDS->cbData / sizeof(wchar_t);
                std::wstring payload(pText, numChars);
                std::vector<std::wstring> paths;
                size_t start = 0;
                while (start < payload.size()) {
                    size_t end = payload.find(L'\n', start);
                    if (end == std::wstring::npos) end = payload.size();
                    std::wstring line = payload.substr(start, end - start);
                    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\0')) line.pop_back();
                    if (!line.empty()) {
                        std::wstring fullPath = Ipc::ResolveFullPathLong(line);
                        if (!fullPath.empty()) { paths.push_back(fullPath); }
                    }
                    start = end + 1;
                }
                if (!paths.empty()) {
                    if (importJobActive) { CancelImportJob(); }
                    StartImportJob(paths, "", true);
                }
                if (IsIconic(hWnd)) { ShowWindow(hWnd, SW_RESTORE); }
                SetForegroundWindow(hWnd);
                return TRUE;
            }
            return FALSE;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // Keep the GDI shell while initialization or an explicit startup
            // open is pending. Playback can then use mpv without allocating the
            // library device; a normal hub paint creates its own context later.
            const bool awaitingStartupMedia = !playbackSession.IsOpen() &&
                std::any_of(deferredLibraryCommands.begin(), deferredLibraryCommands.end(),
                    [](const auto& command) { return !command.mediaTargets.empty(); });
            if ((!appInitialized || renderer.AreDeviceWorkersRunning() || awaitingStartupMedia) && !renderer.HasPresentedFrame()) {
                PaintBootstrapShell(hdc, ps.rcPaint);
                EndPaint(hWnd, &ps);
                if (!bootstrapShellPainted) { bootstrapShellPainted = true; Diagnostics::RecordStartupMilestone(L"bootstrap-shell"); }
                return 0;
            }
            Render();
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if ((UINT_PTR)wParam == kHeartbeatTimer) { Diagnostics::RecordHeartbeat(); return 0; }
            if ((UINT_PTR)wParam == kThumbnailRefreshTimer) {
                KillTimer(hWnd, kThumbnailRefreshTimer);
                thumbnailRefreshTimerPending = false;
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if ((UINT_PTR)wParam == kTextInputCaretTimer) {
                if (activeTextInputField != TextInputField::None) {
                    textInputCaretVisible = !textInputCaretVisible;
                    UpdateImeCandidateWindow();
                    InvalidateRect(hWnd, NULL, FALSE);
                } else { KillTimer(hWnd, kTextInputCaretTimer); }
                return 0;
            }
            if ((UINT_PTR)wParam == kTextInputImeEchoTimer) { KillTimer(hWnd, kTextInputImeEchoTimer); pendingImeEcho.clear(); return 0; }
            // Only repaint when the tick actually changed something (caret blink
            // already invalidates above): a fixed-rate forced repaint here would
            // repaint the whole surface every 530 ms while a text field is focused.
            if (Update(0.016f)) InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            // Alt+Space is a native Windows system-menu chord. Preserve the WM_SYSKEYDOWN Alt-context bit and bypass Aperture shortcuts.
            if (uMsg == WM_SYSKEYDOWN && wParam == VK_SPACE && (lParam & (1LL << 29)) != 0) { break; }
            if (activeDragType != DragType::None || pendingDragType != DragType::None) {
                if (wParam == VK_ESCAPE) CancelCardDrag();
                return 0;
            }
            if (IsAutoscrolling()) { StopAutoscroll(true); return 0; }
            const bool handled = HandleGlobalKeyDown(wParam, lParam);
            if (handled) {
                if (playbackSession.IsOpen()) {
                    playerTooltips.HideForKeyboard();
                    Render();
                } else {
                    ShowHubTooltipForKeyboardFocus();
                    InvalidateRect(hWnd, NULL, FALSE);
                }
                return 0;
            }
            break;
        }
        case WM_CHAR: {
            if (activeTextInputField != TextInputField::None) { HandleTextInputChar(static_cast<wchar_t>(wParam)); return 0; }
            break;
        }
        case WM_UNICHAR: {
            if (wParam == UNICODE_NOCHAR) return TRUE;
            if (activeTextInputField != TextInputField::None) {
                const unsigned int codePoint = static_cast<unsigned int>(wParam);
                if (codePoint <= 0xFFFFu) { HandleTextInputChar(static_cast<wchar_t>(codePoint)); } else if (codePoint <= 0x10FFFFu) {
                    const unsigned int scalar = codePoint - 0x10000u;
                    HandleTextInputChar(static_cast<wchar_t>(0xD800u + (scalar >> 10)));
                    HandleTextInputChar(static_cast<wchar_t>(0xDC00u + (scalar & 0x3FFu)));
                }
                return 0;
            }
            break;
        }
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
            if (HandleTextInputIme(uMsg, wParam, lParam)) return 0;
            break;
        case WM_ACTIVATEAPP: {
            if (!wParam && !playbackSession.IsPiP() && hOverlayWnd) {
                SetWindowPos(hOverlayWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else if (wParam && playbackSession.IsOpen() && hOverlayWnd) { SyncOverlayPosition(); }
            return 0;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (IsAutoscrolling()) { StopAutoscroll(true); }
                isOverlayMouseTracking = false;
                isMouseTracking = false;
                lastPlayerMouseX = -1.0f;
                lastPlayerMouseY = -1.0f;
                if (!playbackSession.IsPiP() && hOverlayWnd)
                    SetWindowPos(hOverlayWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                lastPlayerMouseX = -1.0f;
                lastPlayerMouseY = -1.0f;
                if (playbackSession.IsOpen()) {
                    controlsIdleTimer = 0.0f;
                    controlsOpacity = 1.0f;
                    SyncOverlayPosition();
                    if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_CANCELMODE:
            CancelTouchGestures();
            // fall through: the shared press-state cleanup below still runs
        case WM_CAPTURECHANGED:
            if (activeDragType != DragType::None || pendingDragType != DragType::None) CancelCardDrag();
            settingsPressState = {};
            subtitleSizeDragging = false;
            isClearHistoryPressed = false;
            isModalOkPressed = false;
            isModalCancelPressed = false;
            pressedNavIndex = -1;
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                isPiPPotentialWindowDrag = false;
                isPiPDraggingWindow = false;
            }
            if (isScrubbing || isVolumeScrubbing) {
                isScrubbing = false;
                scrubTargetTime = -1.0;
                isVolumeScrubbing = false;
                isTimelineHovered = false;
            }
            if (IsAutoscrolling() && (HWND)lParam != hWnd) { StopAutoscroll(false); }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        case WM_KILLFOCUS:
            if (activeDragType != DragType::None || pendingDragType != DragType::None) CancelCardDrag();
            settingsPressState = {};
            isClearHistoryPressed = false;
            isModalOkPressed = false;
            isModalCancelPressed = false;
            pressedNavIndex = -1;
            if (IsAutoscrolling()) { StopAutoscroll(true); }
            HideTooltipKeyboardFocus();
            if (activeTextInputField != TextInputField::None) BlurTextInput();
            isOverlayMouseTracking = false;
            isVideoMouseTracking = false;
            isMouseTracking = false;
            lastPlayerMouseX = -1.0f;
            lastPlayerMouseY = -1.0f;
            break;
        case WM_SETFOCUS:
            lastPlayerMouseX = -1.0f;
            lastPlayerMouseY = -1.0f;
            if (!playbackSession.IsOpen()) { ShowHubTooltipForKeyboardFocus(); } else {
                controlsIdleTimer = 0.0f;
                if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
                InvalidateRect(hWnd, NULL, FALSE);
            }
            break;
        case WM_MOUSEWHEEL: {
            // Active shortcut capture: a wheel notch completes the binding
            // instead of scrolling the hub pane.
            if (HandleShortcutCaptureMouse(GET_WHEEL_DELTA_WPARAM(wParam) >= 0 ? kMouseWheelUp : kMouseWheelDown)) return 0;
            if (IsAutoscrolling()) { StopAutoscroll(true); return 0; }
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool isPhysicalCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            float scrollDelta = (float)delta * 0.5f;
            if (playbackSession.IsOpen()) {
                if (HandleContextMenuWheel(delta)) {
                    InvalidateRect(hWnd, NULL, FALSE);
                    Render();
                    return 0;
                }
                if (isSettingsFlyoutOpen && currentFlyoutMenu == "queue") {
                    const size_t queueSize = playbackSession.GetQueueSize();
                    const size_t maxFirst = queueSize > 7 ? queueSize - 7 : 0;
                    if (delta > 0 && queueFlyoutFirstVisible > 0) {
                        --queueFlyoutFirstVisible;
                    } else if (delta < 0 && queueFlyoutFirstVisible < maxFirst) { ++queueFlyoutFirstVisible; }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                if (isPhysicalCtrl) {
                    int step = (delta > 0) ? 50 : -50;
                    settings.subDelayMs += step;
                    player.SetSubDelay((double)settings.subDelayMs / 1000.0);
                    SaveSettings();
                    wchar_t buf[64];
                    swprintf_s(buf, L"Subtitle Delay: %+d ms", settings.subDelayMs);
                    ShowOSD(IconType::Subtitles, buf);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else {
                    double newVol = std::clamp(player.GetVolume() + (delta > 0 ? 0.05 : -0.05), 0.0, kMaxVolumeRatio);
                    player.SetVolume(newVol);
                    settings.volume = newVol;
                    settings.muted = false;
                    int pct = (int)std::round(newVol * 100.0);
                    ShowOSD(newVol <= 0.01 ? IconType::VolumeMuted : (newVol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow), std::to_wstring(pct) + L"%");
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            } else {
                if (globalSearchModel.IsOpen()) {
                    if (delta > 0) globalSearchModel.MoveSelectionUp();
                    else if (delta < 0) globalSearchModel.MoveSelectionDown();
                    InvalidateRect(hWnd, NULL, FALSE);
                    MarkAccessibilityDirty();
                    return 0;
                }
                if (listenPlaybackActive) {
                    POINT wheelPtLi{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &wheelPtLi);
                    if (PtInRectF(listenIslandHitRect, static_cast<float>(wheelPtLi.x), static_cast<float>(wheelPtLi.y))) {
                        const double curVol = settings.muted ? 0.0 : std::clamp(settings.volume, 0.0, 1.0);
                        const double newVol = std::clamp(curVol + (delta > 0 ? 0.05 : -0.05), 0.0, 1.0);
                        settings.volume = newVol;
                        settings.muted = (newVol <= 0.001);
                        const double eff = settings.muted ? 0.0 : settings.volume;
                        listenVolumeApplied = eff;
                        listenPlayer.SetVolume(eff);
                        SaveSettings();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                }
                if (currentTab == TabView::Media) {
                    POINT wheelPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &wheelPt);
                    if (PtInRectF(mediaSortBtnBounds, static_cast<float>(wheelPt.x), static_cast<float>(wheelPt.y))) {
                        std::vector<LibrarySortField> fields;
                        if (libraryGroupMode == LibraryGroupMode::Flat && settings.manualMediaOrder)
                            fields.push_back(LibrarySortField::Manual);
                        fields.push_back(LibrarySortField::Title);
                        fields.push_back(LibrarySortField::DateAdded);
                        fields.push_back(LibrarySortField::LastPlayed);
                        fields.push_back(LibrarySortField::Duration);
                        size_t current = 0;
                        for (size_t i = 0; i < fields.size(); ++i) {
                            if (fields[i] == librarySortField) { current = i; break; }
                        }
                        if (!fields.empty()) {
                            const int dir = delta > 0 ? -1 : 1;
                            const int count = static_cast<int>(fields.size());
                            const int next = (static_cast<int>(current) + dir + count) % count;
                            SetLibrarySort(fields[static_cast<size_t>(next)], librarySortDirection);
                        }
                        return 0;
                    }
                    if (libraryGroupMode == LibraryGroupMode::Listen) {
                        const float maxListenScroll = std::max(0.0f, listenContentHeight - listenViewportHeight);
                        listenScrollY = std::clamp(listenScrollY - scrollDelta, 0.0f, maxListenScroll);
                        POINT wheelPtLs{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                        ScreenToClient(hWnd, &wheelPtLs);
                        ResolveListenHover((float)wheelPtLs.x, (float)wheelPtLs.y);
                    } else {
                    mediaScrollY = std::max(0.0f, mediaScrollY - scrollDelta);
                    POINT wheelPtMd{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &wheelPtMd);
                    UpdateHoverStates((float)wheelPtMd.x, (float)wheelPtMd.y);
                    }
                } else if (currentTab == TabView::Playlists) {
                    playlistDetailScrollY = std::max(0.0f, playlistDetailScrollY - scrollDelta);
                    // Content moves under a stationary cursor while scrolling;
                    // re-resolve hover so the resume preview keeps running.
                    POINT wheelPtPl{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &wheelPtPl);
                    ResolvePlaylistHover((float)wheelPtPl.x, (float)wheelPtPl.y);
                }
                else if (currentTab == TabView::History) {
                    historyScrollY = std::max(0.0f, historyScrollY - scrollDelta);
                } else if (currentTab == TabView::Settings) {
                    settingsScrollY = std::max(0.0f, settingsScrollY - scrollDelta);
                    // Rows move under a stationary cursor while scrolling; re-resolve
                    // every tracked hover (sidebar, System rows, shortcuts) so the
                    // highlight never trails the list.
                    POINT wheelPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    ScreenToClient(hWnd, &wheelPt);
                    ResolveSettingsHover((float)wheelPt.x, (float)wheelPt.y);
                }
                ClampScrollOffsets();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (playbackSession.IsOpen() && controlsOpacity <= 0.02f && !isSettingsFlyoutOpen && !isContextMenuOpen && !isScrubbing && !isVolumeScrubbing) {
                if (IsPlaybackDiagnosticsCloseHit(lastPlayerMouseX, lastPlayerMouseY)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
                if (IsLogViewerCloseHit(lastPlayerMouseX, lastPlayerMouseY)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
                SetCursor(NULL);
                return TRUE;
            }
            if (!playbackSession.IsOpen()) {
                if (currentTab == TabView::Media && hoveredMediaGroupHeader >= 0) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
                if (currentTab == TabView::Settings) {
                    if (currentSettingsCat == SettingsCategory::System && hoveredSettingsRow >= 0) {
                        SetCursor(LoadCursor(NULL, IDC_HAND));
                        return TRUE;
                    }
                    if (hoveredSettingsCat != (SettingsCategory)-1) {
                        SetCursor(LoadCursor(NULL, IDC_HAND));
                        return TRUE;
                    }
                }
            }
            break;
        }
        case WM_MOUSEMOVE: {
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            hubMouseX = x;
            hubMouseY = y;
            if (IsAutoscrolling()) {
                autoscroll.currentPoint = { x, y };
                float autoscrollDp = std::max(0.5f, renderer.GetDpiScale());
                float dyDIP = (autoscroll.currentPoint.y - autoscroll.anchorPoint.y) / autoscrollDp;
                float dxDIP = (autoscroll.currentPoint.x - autoscroll.anchorPoint.x) / autoscrollDp;
                const float kDeadZone = 15.0f;
                if (autoscroll.mode == AutoscrollMode::Pending) {
                    if (std::abs(dyDIP) > kDeadZone || std::abs(dxDIP) > kDeadZone) { autoscroll.mode = AutoscrollMode::Holding; }
                }
                return 0;
            }
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float)(rc.right - rc.left);
            float winH = (float)(rc.bottom - rc.top);
            bool hoverChanged = false;
            const bool oldPlaylistAddHovered = isPlaylistAddHovered, oldPlaylistAddFilesHovered = isPlaylistAddFilesHovered;
            const int oldHoveredPlaylistRailIndex = hoveredPlaylistRailIndex;
            const int oldHoveredPlaylistDetailItemIndex = hoveredPlaylistDetailItemIndex;
            const int oldHoveredPlaylistDetailDeleteIndex = hoveredPlaylistDetailDeleteIndex;
            const bool oldPlaylistPlayAllHovered = isPlaylistPlayAllHovered, oldPlaylistDeleteHovered = isPlaylistDeleteHovered;
            const bool oldPlaylistResumeHovered = isPlaylistResumeHovered, oldClearHistoryHovered = isClearHistoryHovered;
            const int oldHoveredHistoryIndex = hoveredHistoryIndex, oldHoveredHistoryDeleteIndex = hoveredHistoryDeleteIndex;
            const int oldHoveredHistoryEmptyBtn = hoveredHistoryEmptyBtn;
            const SettingsCategory oldHoveredSettingsCat = hoveredSettingsCat;
            const int oldHoveredSettingsRow = hoveredSettingsRow;
            const int oldHoveredShortcutIndex = hoveredShortcutIndex;
            const bool oldResetShortcutsHovered = isResetShortcutsHovered;
            const int oldHoveredContextMenuItem = hoveredContextMenuItem;
            const int oldHoveredIrisShoulder = hoveredIrisShoulder;
            const int oldActiveIrisDialIndex = activeIrisDialIndex;
            const int oldListenHoveredFeed = listenHoveredFeed;
            const int oldListenHoveredEpisode = listenHoveredEpisode;
            const int oldListenHoveredEpisodeFeed = listenHoveredEpisodeFeed;
            const int oldListenHoveredStation = listenHoveredStation;
            const bool oldListenAddPodcastHovered = listenAddPodcastHovered;
            if (playbackSession.IsOpen() && activeModal == ModalType::None) {
                if (activeTextInputField == TextInputField::PlaybackSpeed && textInputDragging) {
                    SetTextInputCaretFromMouse(x, y, true);
                    Render();
                    return 0;
                }
                if (!isMouseTracking) {
                    TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
                    TrackMouseEvent(&tme);
                    isMouseTracking = true;
                }
                bool mouseMoved = (lastPlayerMouseX < 0.0f || std::abs(x - lastPlayerMouseX) >= 1.0f || std::abs(y - lastPlayerMouseY) >= 1.0f);
                if (mouseMoved) {
                    lastPlayerMouseX = x;
                    lastPlayerMouseY = y;
                    controlsIdleTimer = 0.0f;
                    controlsOpacity = 1.0f;
                    isKeyboardNavActive = false;
                    if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
                }
                if (playbackSession.IsPiP()) {
                    if (isPiPPotentialWindowDrag || isPiPDraggingWindow) {
                        POINT curPt;
                        GetCursorPos(&curPt);
                        int dx = curPt.x - pipWindowDragStartPoint.x;
                        int dy = curPt.y - pipWindowDragStartPoint.y;
                        if (!isPiPDraggingWindow && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
                            isPiPDraggingWindow = true;
                            isPiPDragOccurred = true;
                        }
                        if (isPiPDraggingWindow) {
                            isPiPDragOccurred = true;
                            int newX = pipWindowDragStartWindowPos.x + dx;
                            int newY = pipWindowDragStartWindowPos.y + dy;
                            SetWindowPos(hWnd, NULL, newX, newY, 0, 0,
                                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                            // Drag coordinates are the top-level window's
                            // outer bounds. Recompute the overlay from the
                            // owner's client origin so the PiP frame border
                            // never becomes a visible control offset.
                            SyncOverlayPosition();
                            return 0;
                        }
                    }
                    const auto pip = Presentation::PiP(winW, winH, dp);
                    RefreshOverlayHitTargets(x, y);
                    if (isScrubbing) {
                        ScrubTimelineBetween(x, pip.timeline.left, std::max(1.0f, RectWidth(pip.timeline)));
                    }
                    if (isPiPVolumeDragging) {
                        float trackH = pip.volumeTrack.bottom - pip.volumeTrack.top;
                        if (trackH > 1.0f) {
                            float ratio = (pip.volumeTrack.bottom - y) / trackH;
                            double newVol = std::clamp((double)ratio, 0.0, 1.0) * kMaxVolumeRatio;
                            commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ newVol });
                        }
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                if (mouseMoved) InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            } else {
                bool mouseMoved = (lastPlayerMouseX < 0.0f || std::abs(x - lastPlayerMouseX) >= 1.0f || std::abs(y - lastPlayerMouseY) >= 1.0f);
                if (mouseMoved) {
                    lastPlayerMouseX = x;
                    lastPlayerMouseY = y;
                    if (isKeyboardNavActive) { isKeyboardNavActive = false; hoverChanged = true; }
                }
                if (activeTextInputField != TextInputField::None && textInputDragging) { SetTextInputCaretFromMouse(x, y, true); return 0; }
                if (importIslandPhase != 0 || importIslandFade.current > 0.001f) {
                    const bool newPillHovered = importIslandPhase != 0 && PtInRectF(importIslandHitRect, x, y);
                    const bool newXHovered = newPillHovered && importIslandExpand.current > 0.5f &&
                        PtInRectF(importIslandXBounds, x, y);
                    if (importPillHovered != newPillHovered || importCancelHovered != newXHovered) {
                        importPillHovered = newPillHovered;
                        importCancelHovered = newXHovered;
                        hoverChanged = true;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                } else if (importPillHovered || importCancelHovered) {
                    importPillHovered = false;
                    importCancelHovered = false;
                    hoverChanged = true;
                }
                if (isListenVolumeDragging) {
                    if (!(wParam & MK_LBUTTON)) {
                        isListenVolumeDragging = false;
                        if (GetCapture() == hWnd) ReleaseCapture();
                        SaveSettings();
                    } else {
                        const double ratio = std::clamp((double)(x - listenIslandVolumeBounds.left) / (double)std::max(1.0f, RectWidth(listenIslandVolumeBounds)), 0.0, 1.0);
                        settings.volume = ratio;
                        settings.muted = (ratio <= 0.001);
                        const double eff = settings.muted ? 0.0 : settings.volume;
                        listenVolumeApplied = eff;
                        listenPlayer.SetVolume(eff);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                }
                if (isListenSeekDragging) {
                    if (!(wParam & MK_LBUTTON)) {
                        isListenSeekDragging = false;
                        if (GetCapture() == hWnd) ReleaseCapture();
                    } else if (!listenPlaybackIsRadio) {
                        const double dur = listenPlayer.GetDurationSec();
                        if (dur > 0.0) {
                            const double ratio = std::clamp((double)(x - listenIslandSeekBounds.left) / (double)std::max(1.0f, RectWidth(listenIslandSeekBounds)), 0.0, 1.0);
                            listenPlayer.SeekAbsolute(ratio * dur);
                            InvalidateRect(hWnd, NULL, FALSE);
                        }
                        return 0;
                    }
                }
                if (listenPlaybackActive || listenIslandFade.current > 0.001f) {
                    const bool newPillHovered = listenPlaybackActive && PtInRectF(listenIslandHitRect, x, y);
                    const bool newBadge = newPillHovered && PtInRectF(listenIslandBadgeBounds, x, y);
                    const bool newMute = newPillHovered && PtInRectF(listenIslandMuteBounds, x, y);
                    const bool newVol = newPillHovered && PtInRectF(listenIslandVolumeBounds, x, y);
                    const bool newSeek = newPillHovered && (listenIslandExpand.current > 0.25f) && !listenPlaybackIsRadio && PtInRectF(listenIslandSeekBounds, x, y);
                    const bool newX = newPillHovered && PtInRectF(listenIslandXBounds, x, y);
                    if (listenPillHovered != newPillHovered || listenBadgeHovered != newBadge ||
                        listenMuteHovered != newMute || listenVolumeHovered != newVol ||
                        listenSeekHovered != newSeek || listenXHovered != newX) {
                        listenPillHovered = newPillHovered;
                        listenBadgeHovered = newBadge;
                        listenMuteHovered = newMute;
                        listenVolumeHovered = newVol;
                        listenSeekHovered = newSeek;
                        listenXHovered = newX;
                        hoverChanged = true;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                } else if (listenPillHovered || listenBadgeHovered || listenMuteHovered || listenVolumeHovered || listenSeekHovered || listenXHovered) {
                    listenPillHovered = false;
                    listenBadgeHovered = false;
                    listenMuteHovered = false;
                    listenVolumeHovered = false;
                    listenSeekHovered = false;
                    listenXHovered = false;
                    hoverChanged = true;
                }
                if (activeModal != ModalType::None) {
                    const bool isInputModal = (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist || activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput ||
                        activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput || activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides ||
                        activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile || activeModal == ModalType::JumpToTime || activeModal == ModalType::CustomAspectInput || activeModal == ModalType::CustomRotationInput);
                    const float maxBoxW = isInputModal ? 460.0f * dp : 500.0f * dp;
                    const float boxW = std::min(maxBoxW, std::max(320.0f * dp, winW - 48.0f * dp));
                    const float boxH = (isInputModal ? 204.0f : Presentation::ConfirmationHeight) * dp;
                    const float boxLeft = (winW - boxW) * 0.5f;
                    const float boxTop = (winH - boxH) * 0.5f;
                    const float confirmW = isInputModal ? 92.0f * dp :
                        std::max(110.0f * dp, renderer.MeasureString(modalConfirmText, FontType::Medium).width + 32.0f * dp);
                    const float cancelW = isInputModal ? 92.0f * dp : 100.0f * dp, buttonY = boxTop + boxH - 56.0f * dp;
                    const float confirmRight = boxLeft + boxW - 24.0f * dp;
                    const D2D1_RECT_F confirmRect = D2D1::RectF(confirmRight - confirmW, buttonY, confirmRight, buttonY + 36.0f * dp);
                    const D2D1_RECT_F cancelRect = D2D1::RectF(confirmRect.left - 10.0f * dp - cancelW, buttonY,
                        confirmRect.left - 10.0f * dp, buttonY + 36.0f * dp);
                    const bool newModalOkHovered = PtInRectF(confirmRect, x, y);
                    const bool newModalCancelHovered = PtInRectF(cancelRect, x, y);
                    if (isModalOkHovered != newModalOkHovered || isModalCancelHovered != newModalCancelHovered) {
                        isModalOkHovered = newModalOkHovered;
                        isModalCancelHovered = newModalCancelHovered;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    return 0;
                }
                if (subtitleSizeDragging && GetCapture() == hWnd && currentTab == TabView::Settings &&
                    currentSettingsCat == SettingsCategory::Subtitles) {
                    const D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
                    const float sideW = SettingsLayout::kSidebarWidth * dp;
                    const D2D1_RECT_F contentBounds = SettingsLayout::ContentBounds(viewBounds, dp);
                    const float padX = SettingsLayout::kContentPadX * dp;
                    const float contentW = RectWidth(contentBounds) - padX * 2.0f;
                    const float subLeft = contentBounds.left + padX, cardRight = subLeft + contentW + 10.0f * dp;
                    const float sliderLeft = subLeft + 22.0f * dp + 64.0f * dp + 18.0f * dp, sliderRight = cardRight - 22.0f * dp;
                    const int pixels = SubtitleSizeFromSliderX(x, sliderLeft, sliderRight);
                    settingsPressState.subIndex = pixels - 14;
                    keyboardFocusedIndex = 2;
                    activeFocusPane = FocusPane::SettingsContent;
                    const std::string value = SubtitleSizeToString(pixels);
                    if (settings.subFontSize != value) { settings.subFontSize = value; ApplyPlayerPreferences(); SaveSettings(); }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                if (pendingDragType != DragType::None) {
                    if (std::abs(x - dragStartX) < std::max(5.0f * dp, static_cast<float>(GetSystemMetrics(SM_CXDRAG))) &&
                        std::abs(y - dragStartY) < std::max(5.0f * dp, static_cast<float>(GetSystemMetrics(SM_CYDRAG)))) return 0;
                    activeDragType = pendingDragType;
                    pendingDragType = DragType::None;
                    previewPlayer.StopPreview();
                    if (activeDragType == DragType::MediaList) {
                        for (size_t i = 0; i < rowAnimStates.size(); ++i) {
                            auto& row = rowAnimStates[i];
                            const auto& visible = GetVisibleMediaIndices();
                            const bool pinned = !isLastWatchedCleared && i == 0 && !visible.empty() && library.mediaItems[visible[0]].lastWatchedTimestamp > 0;
                            row.isHovering = false;
                            row.hoverEnterTime = -1.0f;
                            row.height.SnapTo(pinned ? kMediaPinnedRestHeight : kMediaRestHeight);
                            row.liftY.SnapTo(0);
                            row.previewOpacity.SnapTo(0);
                        }
                        InvalidateMediaLayoutCache();
                    }
                }
                if (activeDragType != DragType::None) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    dragCurrentY = y;
                    dragCurrentX = x;
                    const float dragCenterX = (dragSourceRect.left + dragSourceRect.right) * 0.5f + (dragCurrentX - dragStartX);
                    const float dragCenterY = (dragSourceRect.top + dragSourceRect.bottom) * 0.5f + (dragCurrentY - dragStartY);
                    float pad = 18.0f * dp, padY = 14.0f * dp, navH = 48.0f * dp;
                    D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
                    if (activeDragType == DragType::MediaList) {
                        const auto navLayout = Presentation::Nav(winW, dp, Localization::Instance().IsRtl(), [&](const std::wstring& s) { return renderer.MeasureString(s, FontType::Medium).width; });
                        const D2D1_RECT_F plTabRect = navLayout.tabs[1];
                        isPlaylistsNavDropHovered = PtInRectF(plTabRect, x, y);
                        const auto& visible = GetVisibleMediaIndices();
                        int targetIdx = dragFromIndex;
                        if (!visible.empty()) {
                            const float centerYDIP = (dragCenterY - (viewBounds.top + 44.0f * dp) + mediaScrollY) / dp;
                            EnsureMediaLayoutCache();
                            const auto& offsets = m_mediaLayoutCache.rowOffsetsDIP;
                            const int count = static_cast<int>(std::min(visible.size(), offsets.empty() ? size_t(0) : offsets.size() - 1));
                            if (count > 0) {
                                auto slotCenter = [&](int index) {
                                    return offsets[static_cast<size_t>(index)] + GetMediaRowHeightDIP(static_cast<size_t>(index)) * 0.5f;
                                };
                                if (centerYDIP <= slotCenter(0)) { targetIdx = 0; } else if (centerYDIP >= slotCenter(count - 1)) {
                                    targetIdx = count - 1;
                                } else {
                                    int lo = 1, hi = count - 1;
                                    while (lo < hi) {
                                        const int mid = lo + (hi - lo) / 2;
                                        if (slotCenter(mid) < centerYDIP) lo = mid + 1;
                                        else hi = mid;
                                    }
                                    const float midpoint = (slotCenter(lo - 1) + slotCenter(lo)) * 0.5f;
                                    targetIdx = centerYDIP < midpoint ? lo - 1 : lo;
                                }
                            }
                        }
                        // The Continue Watching card remains pinned; its content may still be added to a playlist.
                        int minIdx = !isLastWatchedCleared && visible.size() > 1 && library.mediaItems[visible[0]].lastWatchedTimestamp > 0 ? 1 : 0;
                        dragToIndex = dragFromIndex == 0 && minIdx == 1 ? 0
                            : std::clamp(targetIdx, minIdx, std::max(minIdx, static_cast<int>(visible.size()) - 1));
                    } else if (activeDragType == DragType::MediaFolder) {
                        const auto& proj = GetCurrentMediaProjection();
                        if (!proj.groups.empty()) {
                            float tbH = 32.0f * dp, listTop = viewBounds.top + tbH + 12.0f * dp;
                            float yDIP = (y - listTop + mediaScrollY) / dp;
                            int targetIdx = HitTestMediaGroupHeader(yDIP, proj.groups.size());
                            if (targetIdx >= 0) { dragToIndex = targetIdx; }
                        }
                    } else if (activeDragType == DragType::PlaylistRail) {
                        float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
                        D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
                        float itemH = 52.0f * dp, curY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
                        dragToIndex = Reorder::LinearTargetFromCenter( dragCenterY, curY + itemH * 0.5f, itemH + 6.0f * dp,
                            static_cast<int>(library.playlists.size()));
                    } else if (activeDragType == DragType::PlaylistItem && playlistSearchInput.GetText().empty()) {
                        Playlist* pl = library.FindPlaylist(activePlaylistId);
                        if (pl && !pl->itemPaths.empty()) {
                            float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
                            D2D1_RECT_F detailBounds = D2D1::RectF(viewBounds.left + railW + 16.0f * dp, viewBounds.top, viewBounds.right, viewBounds.bottom);
                            const float paneRight = std::max(detailBounds.left + 120.0f * dp, detailBounds.right - 26.0f * dp);
                            const float width = paneRight - detailBounds.left;
                            const float gap = (playlistGridView ? 10.0f : 6.0f) * dp;
                            const int columns = playlistGridView ? static_cast<int>(std::max(1.0f, std::floor((width + gap) / (190.0f * dp)))) : 1;
                            const float cellW = playlistGridView ? std::max(120.0f * dp, (width - (columns - 1) * gap) / columns) : width;
                            const float itemH = (playlistGridView ? 166.0f : 62.0f) * dp;
                            const float firstCenterX = detailBounds.left + cellW * 0.5f;
                            const float firstCenterY = detailBounds.top + 252.0f * dp - playlistDetailScrollY + itemH * 0.5f;
                            dragToIndex = Reorder::GridTargetFromCenter( dragCenterX, dragCenterY, firstCenterX, firstCenterY,
                                cellW + gap, (playlistGridView ? 176.0f : 68.0f) * dp, columns, static_cast<int>(pl->itemPaths.size()));
                        }
                    }
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                if (!isMouseTracking) {
                    TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
                    TrackMouseEvent(&tme);
                    isMouseTracking = true;
                }
                // An open context menu is solid: while the cursor is inside it,
                // hub hover states stay as they are (previews persist) but new
                // hover effects must not trigger from content behind the menu.
                const bool cursorInsideOpenMenu = isContextMenuOpen && PtInRectF(ContextMenuBounds(dp), x, y);
                if (!cursorInsideOpenMenu) {
                hoverChanged = hoverChanged || UpdateHoverStates(x, y);
                D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
                if (currentTab == TabView::Media && libraryGroupMode == LibraryGroupMode::Listen) {
                    ResolveListenHover(x, y);
                } else {
                    listenHoveredFeed = -1;
                    listenHoveredEpisode = -1;
                    listenHoveredEpisodeFeed = -1;
                    listenHoveredStation = -1;
                    listenAddPodcastHovered = false;
                }
                if (currentTab == TabView::Playlists) {
                    ResolvePlaylistHover(x, y);
                } else if (currentTab == TabView::History) {
                    D2D1_SIZE_F clearSize = renderer.MeasureString(L"Clear History", FontType::Medium);
                    float clearW = clearSize.width + 24.0f * dp, clearH = 32.0f * dp;
                    float clearTop = viewBounds.top + (46.0f * dp - clearH) * 0.5f;
                    D2D1_RECT_F clearBtnRect = D2D1::RectF(viewBounds.right - clearW - 6.0f * dp, clearTop,
                        viewBounds.right - 6.0f * dp, clearTop + clearH);
                    isClearHistoryHovered = PtInRectF(clearBtnRect, x, y);
                    float headerY = viewBounds.top + kHistoryContentInset * dp - historyScrollY;
                    hoveredHistoryIndex = -1;
                    hoveredHistoryDeleteIndex = -1;
                    hoveredHistoryEmptyBtn = -1;
                    if (library.history.empty()) {
                        for (size_t i = 0; i < historyEmptyBtnBounds.size(); ++i) {
                            if (PtInRectF(historyEmptyBtnBounds[i], x, y)) { hoveredHistoryEmptyBtn = (int)i; break; }
                        }
                    } else {
                        const HistoryTimelineProjection& proj = GetHistoryProjection();
                        const float delSize = 28.0f * dp;
                        for (size_t g = 0; g < proj.groups.size(); ++g) {
                            const HistoryGroupLayout layout(headerY, proj.groups[g].count, dp);
                            headerY = layout.NextHeaderTop();
                            const auto [firstRow, lastRow] = layout.VisibleRows(y, y);
                            for (size_t row = firstRow; row < lastRow; ++row) {
                                const size_t idx = proj.groups[g].startIndex + row;
                                size_t origIdx = proj.itemIndices[idx];
                                const float rowY = layout.RowTop(row);
                                D2D1_RECT_F itemRect = D2D1::RectF(viewBounds.left + 12.0f * dp, rowY,
                                    viewBounds.right - 24.0f * dp, rowY + layout.rowHeight);
                                if (PtInRectF(itemRect, x, y)) {
                                    hoveredHistoryIndex = (int)origIdx;
                                    D2D1_RECT_F delRect = D2D1::RectF(itemRect.right - delSize - 10.0f * dp,
                                        itemRect.top + (layout.rowHeight - delSize) * 0.5f, itemRect.right - 10.0f * dp,
                                        itemRect.top + (layout.rowHeight + delSize) * 0.5f);
                                    if (PtInRectF(delRect, x, y)) hoveredHistoryDeleteIndex = (int)origIdx;
                                    break;
                                }
                            }
                            if (hoveredHistoryIndex >= 0) break;
                        }
                    }
                } else if (currentTab == TabView::Settings) {
                    ResolveSettingsHover(x, y);
                }
                } // cursor-inside-open-menu guard
            }
            if (isContextMenuOpen) {
                UpdateContextMenuHitTarget(x, y, dp);
            }
            if (currentTab == TabView::Playlists && playlistGridView) {
                if (hoveredPlaylistDetailItemIndex != playlistCardHoverIndex) {
                    if (playlistCardHoverIndex >= 0) {
                        auto it = playlistCardExpands.find(static_cast<size_t>(playlistCardHoverIndex));
                        if (it != playlistCardExpands.end() && (it->second.current > 0.0f || it->second.animating)) {
                            AnimateVisual(it->second, 0.0f, MotionTokens::kResponsive, MotionTokens::Standard());
                        }
                    }
                    playlistCardHoverIndex = hoveredPlaylistDetailItemIndex;
                    playlistCardHoverEnterTime = (playlistCardHoverIndex >= 0) ? NowSeconds() : -1.0f;
                }
            } else if (playlistCardHoverIndex >= 0) {
                ClearPlaylistCardHover();
            }
            hoverChanged = hoverChanged || oldPlaylistAddHovered != isPlaylistAddHovered ||
                oldPlaylistAddFilesHovered != isPlaylistAddFilesHovered || oldHoveredPlaylistRailIndex != hoveredPlaylistRailIndex ||
                oldHoveredPlaylistDetailItemIndex != hoveredPlaylistDetailItemIndex ||
                oldHoveredPlaylistDetailDeleteIndex != hoveredPlaylistDetailDeleteIndex ||
                oldPlaylistPlayAllHovered != isPlaylistPlayAllHovered || oldPlaylistDeleteHovered != isPlaylistDeleteHovered ||
                oldPlaylistResumeHovered != isPlaylistResumeHovered || oldClearHistoryHovered != isClearHistoryHovered ||
                oldHoveredHistoryIndex != hoveredHistoryIndex || oldHoveredHistoryDeleteIndex != hoveredHistoryDeleteIndex ||
                oldHoveredHistoryEmptyBtn != hoveredHistoryEmptyBtn || oldHoveredSettingsCat != hoveredSettingsCat ||
                oldHoveredSettingsRow != hoveredSettingsRow ||
                oldHoveredShortcutIndex != hoveredShortcutIndex || oldResetShortcutsHovered != isResetShortcutsHovered ||
                oldHoveredContextMenuItem != hoveredContextMenuItem ||
                oldHoveredIrisShoulder != hoveredIrisShoulder ||
                oldActiveIrisDialIndex != activeIrisDialIndex ||
                oldListenHoveredFeed != listenHoveredFeed ||
                oldListenHoveredEpisode != listenHoveredEpisode ||
                oldListenHoveredEpisodeFeed != listenHoveredEpisodeFeed ||
                oldListenHoveredStation != listenHoveredStation ||
                oldListenAddPodcastHovered != listenAddPodcastHovered;
            // Settings rows resolve hover at draw time (pointerIn), so any
            // mousemove over the tab must repaint even when tracked state is
            // unchanged — otherwise highlights lag behind the cursor.
            if (hoverChanged || currentTab == TabView::Settings) InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE: { OnMouseLeave(); InvalidateRect(hWnd, NULL, FALSE); return 0; }
        case WM_MBUTTONDOWN: {
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            // Active shortcut capture: a middle click completes the binding
            // instead of starting autoscroll.
            if (HandleShortcutCaptureMouse(kMouseMiddle)) return 0;
            if (IsAutoscrolling()) { StopAutoscroll(true); autoscroll.suppressNextClick = true; return 0; }
            if (!playbackSession.IsOpen() && activeModal == ModalType::None &&
                activeDragType == DragType::None && !textInputDragging && !isContextMenuOpen && !isSettingsFlyoutOpen) {
                float maxScroll = 0.0f;
                AutoscrollPane pane = HitTestAutoscrollPane(x, y, maxScroll);
                if (pane != AutoscrollPane::None && maxScroll > 0.5f) { StartAutoscroll(pane, x, y); return 0; }
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            if (!rebindingActionId.empty()) return 0;
            if (autoscroll.suppressNextClick) { autoscroll.suppressNextClick = false; return 0; }
            if (IsAutoscrolling()) {
                if (autoscroll.mode == AutoscrollMode::Pending) {
                    autoscroll.mode = AutoscrollMode::Toggle;
                    return 0;
                } else if (autoscroll.mode == AutoscrollMode::Holding) { StopAutoscroll(true); return 0; }
            }
            return 0;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            if (!playbackSession.IsOpen() && HandleHubTouchPointer(hWnd, uMsg, wParam, lParam)) return 0;
            UINT mouseMessage = 0;
            WPARAM mouseWParam = 0;
            LPARAM mouseLParam = 0;
            if (TranslatePointerMessage(hWnd, uMsg, wParam, mouseMessage, mouseWParam, mouseLParam))
                return HandleMessage(hWnd, mouseMessage, mouseWParam, mouseLParam);
            return 0;
        }
        case WM_POINTERCAPTURECHANGED:
            // The system took the pointer away (palm rejection); end cleanly.
            CancelTouchGestures();
            return 0;
        case WM_LBUTTONDOWN: {
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            if (IsAutoscrolling()) { StopAutoscroll(true); autoscroll.suppressNextClick = true; return 0; }
            RECT rc;
            GetClientRect(hWnd, &rc);
            float winW = (float)(rc.right - rc.left);
            float winH = (float)(rc.bottom - rc.top);
            // Resolve the target from this event's coordinates before any
            // modal/menu dispatch; cached hover is only a visual aid.
            if (playbackSession.IsOpen()) RefreshOverlayHitTargets(x, y);
            else RefreshHubHitTargets(x, y);
            if (IsPlaybackDiagnosticsCloseHit(x, y)) { TogglePlaybackDiagnostics(); return 0; }
            if (IsLogViewerCloseHit(x, y)) { ToggleLogViewer(); return 0; }
            if (!playbackSession.IsOpen() && globalSearchModel.IsOpen()) {
                const float searchW = std::min(440.0f * dp, winW - 36.0f * dp);
                const float searchH = std::min(390.0f * dp, winH - 106.0f * dp);
                const float searchLeft = 18.0f * dp, searchTop = 76.0f * dp;
                const D2D1_RECT_F searchRect = D2D1::RectF(searchLeft, searchTop, searchLeft + searchW, searchTop + searchH);
                const D2D1_RECT_F closeRect = D2D1::RectF(searchRect.right - 42.0f * dp,
                    searchTop + 10.0f * dp, searchRect.right - 10.0f * dp, searchTop + 42.0f * dp);
                const D2D1_RECT_F inputRect = D2D1::RectF(searchLeft + 12.0f * dp,
                    searchTop + 10.0f * dp, closeRect.left - 8.0f * dp, searchTop + 42.0f * dp);
                if (PtInRectF(closeRect, x, y)) { CloseGlobalSearch(); InvalidateRect(hWnd, NULL, FALSE); return 0; }
                if (PtInRectF(inputRect, x, y)) {
                    FocusTextInput(TextInputField::GlobalSearch);
                    const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    SetTextInputCaretFromMouse(x, y, extendSelection);
                    textInputDragging = true;
                    textInputDragAnchor = extendSelection ? globalSearchModel.GetTextInputModel().GetAnchor()
                                                          : globalSearchModel.GetTextInputModel().GetCaret();
                    SetCapture(hWnd);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                const auto searchProjection = globalSearchModel.GetProjection();
                float itemY = searchTop + 52.0f * dp;
                for (size_t i = 0; i < searchProjection.results.size(); ++i) {
                    const D2D1_RECT_F itemRect = D2D1::RectF(searchLeft + 8.0f * dp, itemY, searchRect.right - 8.0f * dp, itemY + 44.0f * dp);
                    if (itemRect.bottom <= searchRect.bottom - 26.0f * dp && PtInRectF(itemRect, x, y)) {
                        globalSearchModel.SetSelectedIndex(static_cast<int>(i));
                        auto action = globalSearchModel.ActivateSelection(library);
                        ExecuteGlobalSearchAction(action);
                        return 0;
                    }
                    itemY += 46.0f * dp;
                }
                if (!PtInRectF(searchRect, x, y)) { CloseGlobalSearch(); }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (!playbackSession.IsOpen() && listenPlaybackActive) {
                if (PtInRectF(listenIslandHitRect, x, y)) {
                    if (PtInRectF(listenIslandXBounds, x, y)) {
                        StopListenPlayback();
                        ShowOSD(IconType::Checkmark, L"Listening stopped");
                        return 0;
                    }
                    if (PtInRectF(listenIslandBadgeBounds, x, y)) {
                        ToggleListenPause();
                        return 0;
                    }
                    if (PtInRectF(listenIslandMuteBounds, x, y)) {
                        settings.muted = !settings.muted;
                        if (!settings.muted && settings.volume <= 0.01) settings.volume = 0.5;
                        const double eff = settings.muted ? 0.0 : std::clamp(settings.volume, 0.0, 1.0);
                        listenVolumeApplied = eff;
                        listenPlayer.SetVolume(eff);
                        SaveSettings();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (PtInRectF(listenIslandVolumeBounds, x, y)) {
                        isListenVolumeDragging = true;
                        SetCapture(hWnd);
                        const double ratio = std::clamp((double)(x - listenIslandVolumeBounds.left) / (double)std::max(1.0f, RectWidth(listenIslandVolumeBounds)), 0.0, 1.0);
                        settings.volume = ratio;
                        settings.muted = (ratio <= 0.001);
                        const double eff = settings.muted ? 0.0 : settings.volume;
                        listenVolumeApplied = eff;
                        listenPlayer.SetVolume(eff);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (listenIslandExpand.current > 0.25f && !listenPlaybackIsRadio && PtInRectF(listenIslandSeekBounds, x, y)) {
                        const double dur = listenPlayer.GetDurationSec();
                        if (dur > 0.0) {
                            isListenSeekDragging = true;
                            SetCapture(hWnd);
                            const double ratio = std::clamp((double)(x - listenIslandSeekBounds.left) / (double)std::max(1.0f, RectWidth(listenIslandSeekBounds)), 0.0, 1.0);
                            listenPlayer.SeekAbsolute(ratio * dur);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                    listenPinExpanded = !listenPinExpanded;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }
            if (!playbackSession.IsOpen() && importIslandPhase != 0) {
                if (PtInRectF(importIslandHitRect, x, y)) {
                    if (importIslandExpand.current > 0.5f && PtInRectF(importIslandXBounds, x, y)) {
                        // Dismiss the island; the import itself keeps running.
                        importIslandDismissed = true;
                        importPinExpanded = false;
                        importPillHovered = false;
                        importCancelHovered = false;
                        ShowOSD(IconType::Checkmark, L"Import continues in the background (Esc cancels)");
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    importPinExpanded = !importPinExpanded;
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
            }
            if (isContextMenuOpen) {
                HandleContextMenuClick(x, y, dp);
                InvalidateRect(hWnd, NULL, FALSE);
                if (playbackSession.IsOpen()) Render();
                return 0;
            }
            if (activeModal != ModalType::None) {
                // Check ok/cancel buttons
                float cx = winW * 0.5f, cy = winH * 0.5f;
                bool isInputModal = (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist || activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput || activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput || activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides || activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile || activeModal == ModalType::JumpToTime || activeModal == ModalType::CustomAspectInput || activeModal == ModalType::CustomRotationInput);
                float maxBoxW = isInputModal ? 460.0f * dp : 500.0f * dp;
                float boxW = std::min(maxBoxW, std::max(320.0f * dp, winW - 48.0f * dp));
                float boxH = (isInputModal ? 204.0f : Presentation::ConfirmationHeight) * dp;
                D2D1_RECT_F boxRect = D2D1::RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
                if (isInputModal) {
                    const D2D1_RECT_F inputRect = D2D1::RectF(boxRect.left + 24.0f * dp, boxRect.top + 64.0f * dp,
                        boxRect.right - 24.0f * dp, boxRect.top + 106.0f * dp);
                    if (PtInRectF(inputRect, x, y)) {
                        FocusTextInput(TextInputField::Modal);
                        const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        SetTextInputCaretFromMouse(x, y, extendSelection);
                        textInputDragging = true;
                        textInputDragAnchor = extendSelection ? modalInput.GetAnchor() : modalInput.GetCaret();
                        SetCapture(hWnd);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                }
                float btnW = isInputModal ? 92.0f * dp : std::max(110.0f * dp, renderer.MeasureString(modalConfirmText, FontType::Medium).width + 32.0f * dp);
                float cancelBtnW = isInputModal ? 92.0f * dp : 100.0f * dp, btnH = 36.0f * dp, btnY = boxRect.bottom - 20.0f * dp - btnH;
                D2D1_RECT_F okBtn = D2D1::RectF(boxRect.right - 24.0f * dp - btnW, btnY, boxRect.right - 24.0f * dp, btnY + btnH);
                D2D1_RECT_F cancelBtn = D2D1::RectF(okBtn.left - 10.0f * dp - cancelBtnW, btnY, okBtn.left - 10.0f * dp, btnY + btnH);
                if (PtInRectF(okBtn, x, y)) {
                    isModalOkPressed = true;
                    SetCapture(hWnd);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (PtInRectF(cancelBtn, x, y)) {
                    isModalCancelPressed = true;
                    SetCapture(hWnd);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                } else if (!PtInRectF(boxRect, x, y)) { DismissActiveModal(false); return 0; }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (playbackSession.IsOpen()) {
                if (playbackSession.IsPiP()) {
                    const auto pip = Presentation::PiP(winW, winH, dp);
                    isKeyboardNavActive = false;
                    playerKeyboardFocusId = 0;
                    if (isContextMenuOpen) {
                        HandleContextMenuClick(x, y, dp);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (isSettingsFlyoutOpen) {
                        float flyoutW = 280.0f * dp;
                        float flyoutH = GetFlyoutHeight();
                        D2D1_RECT_F flyoutBounds = D2D1::RectF( winW - flyoutW - 20.0f * dp, winH - 76.0f * dp - flyoutH - 10.0f * dp,
                            winW - 20.0f * dp, winH - 76.0f * dp - 10.0f * dp);
                        if (PtInRectF(flyoutBounds, x, y)) {
                            HandleFlyoutClick(x - flyoutBounds.left, y - flyoutBounds.top);
                        } else { CloseSettingsFlyout(); }
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (isPiPVolumeSliderOpen) {
                        if (PtInRectF(pip.volumeSlider, x, y)) {
                            isPiPVolumeDragging = true;
                            SetCapture(hWnd);
                            float trackH = pip.volumeTrack.bottom - pip.volumeTrack.top;
                            double newVol = std::clamp((double)(pip.volumeTrack.bottom - y) / (double)std::max(1.0f, trackH), 0.0, 1.0) * kMaxVolumeRatio;
                            commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ newVol });
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (!PtInRectF(pip.audio, x, y)) {
                            isPiPVolumeSliderOpen = false;
                            AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                            InvalidateRect(hWnd, NULL, FALSE);
                            const bool overOtherPiPControl = PtInRectF(pip.restore, x, y) || PtInRectF(pip.pin, x, y) ||
                                PtInRectF(pip.close, x, y) || PtInRectF(pip.next, x, y) || PtInRectF(pip.previous, x, y) ||
                                PtInRectF(pip.playPause, x, y) || PtInRectF(pip.timelineHit, x, y);
                            if (!overOtherPiPControl) return 0;
                        }
                    }
                    if (PtInRectF(pip.restore, x, y)) { commandDispatcher.Execute(AppCommand::TogglePiP); return 0; }
                    if (PtInRectF(pip.close, x, y)) { SendMessage(hWnd, WM_CLOSE, 0, 0); return 0; }
                    if (PtInRectF(pip.pin, x, y)) {
                        playbackSession.TogglePiPPinned(hWnd);
                        SyncOverlayPosition();
                        MarkAccessibilityDirty();
                        RefreshAccessibilityTree(true);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (PtInRectF(pip.audio, x, y)) {
                        isPiPVolumeSliderOpen = !isPiPVolumeSliderOpen;
                        if (isPiPVolumeSliderOpen) {
                            pipVolumeAnim.SnapTo(0.0f);
                            AnimateVisual(pipVolumeAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                        } else {
                            AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                        }
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (PtInRectF(pip.next, x, y)) {
                        if (playbackSession.IsQueuePlayback() && playbackSession.GetQueueSize() > 1)
                            commandDispatcher.Execute(AppCommand::PlayNext);
                        return 0;
                    }
                    if (PtInRectF(pip.previous, x, y)) {
                        if (playbackSession.IsQueuePlayback() && (playbackSession.GetQueueIndex() > 0 || settings.loop))
                            commandDispatcher.Execute(AppCommand::PlayPrevious);
                        return 0;
                    }
                    if (PtInRectF(pip.playPause, x, y)) {
                        commandDispatcher.Execute(AppCommand::TogglePlay);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (PtInRectF(pip.timelineHit, x, y) && player.GetDuration() > 0.0) {
                        isScrubbing = true;
                        SetCapture(hWnd);
                        double ratio = std::clamp((double)(x - pip.timeline.left) / (double)std::max(1.0f, RectWidth(pip.timeline)), 0.0, 1.0);
                        double seekTarget = ratio * player.GetDuration();
                        scrubTargetTime = seekTarget;
                        player.Seek(seekTarget, false);
                        lastMpvSeekTime = seekTarget;
                        lastMpvSeekTimestamp = std::chrono::steady_clock::now();
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    GetCursorPos(&pipWindowDragStartPoint);
                    RECT winRect;
                    GetWindowRect(hWnd, &winRect);
                    pipWindowDragStartWindowPos = { winRect.left, winRect.top };
                    isPiPPotentialWindowDrag = true;
                    isPiPDraggingWindow = false;
                    isPiPDragOccurred = false;
                    SetCapture(hWnd);
                    return 0;
                }
                // Jump ahead button
                if (isJumpAheadHovered && jumpAheadAnim.current > 0.1f) {
                    isJumpAheadPressed = true;
                    SetCapture(hWnd);
                    InvalidateRect(hWnd, NULL, FALSE);
                    return 0;
                }
                float bottomBarH = 76.0f * dp, padX = Presentation::PlayerPadding * dp;
                // Back button
                float topBarH = 64.0f * dp, btnSize = 36.0f * dp;
                D2D1_RECT_F backBtn = Presentation::Player(winW, winH, dp).back;
                if (PtInRectF(backBtn, x, y)) { ClosePlayer(); return 0; }
                // Timeline Scrubber
                float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
                float trackCenterY = winH - bottomBarH + 16.0f * dp;
                if (x >= barLeft && x <= barRight && y >= trackCenterY - 12.0f * dp && y <= trackCenterY + 12.0f * dp) {
                    isScrubbing = true;
                    SetCapture(hWnd);
                    double ratio = std::clamp((double)(x - barLeft) / (double)barW, 0.0, 1.0);
                    std::wstring snappedChapter;
                    double seekTarget = TimelineClickTarget(x, barLeft, barW, ratio * player.GetDuration(), snappedChapter, (wParam & MK_CONTROL) != 0);
                    if (!snappedChapter.empty()) ShowOSD(IconType::MediaFilm, snappedChapter);
                    scrubTargetTime = seekTarget;
                    player.Seek(seekTarget, false);
                    lastMpvSeekTime = seekTarget;
                    lastMpvSeekTimestamp = std::chrono::steady_clock::now();
                    return 0;
                }
                // Button clicks
                if (hoveredPlayerButtonId == 1) TriggerShortcut("togglePlay");
                else if (hoveredPlayerButtonId == 2) TriggerShortcut("playNext");
                else if (hoveredPlayerButtonId == 3) TriggerShortcut("toggleMute");
                else if (hoveredPlayerButtonId == 4) commandDispatcher.Execute(AppCommand::ToggleSubtitles);
                else if (hoveredPlayerButtonId == 5) { isSettingsFlyoutOpen = !isSettingsFlyoutOpen; currentFlyoutMenu = "main"; }
                else if (hoveredPlayerButtonId == 6) TogglePiP();
                else if (hoveredPlayerButtonId == 7) ToggleFullscreen();
                else if (hoveredPlayerButtonId == 8) {
                    if (playbackSession.GetQueueIndex() > 0 || settings.loop) TriggerShortcut("playPrevious");
                } else {
                    if (y < winH - bottomBarH && y > topBarH) { TriggerShortcut("togglePlay"); }
                }
            } else {
                // Hub view clicks. Nav targets dispatch on mouse-up so the
                // press state is visible; index matches NavBar drawing.
                auto navIndexForHover = [&]() -> int {
                    if (isNavOpenHovered) return 4;
                    if (isNavSettingsHovered) return 3;
                    if (hoveredTab == TabView::Media) return 0;
                    if (hoveredTab == TabView::Playlists) return 1;
                    if (hoveredTab == TabView::History) return 2;
                    return -1;
                };
                int navIdx = navIndexForHover();
                if (navIdx >= 0) { pressedNavIndex = navIdx; SetCapture(hWnd); InvalidateRect(hWnd, NULL, FALSE); return 0; }
                D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
                if (currentTab == TabView::Media) {
                    if (!mediaSearchInput.GetText().empty() && PtInRectF(mediaSearchClearBtnBounds, x, y)) { ClearMediaSearch(); return 0; }
                    if (PtInRectF(mediaSearchBtnBounds, x, y)) {
                        FocusTextInput(TextInputField::MediaSearch);
                        activeFocusPane = FocusPane::MediaList;
                        const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        SetTextInputCaretFromMouse(x, y, extendSelection);
                        textInputDragging = true;
                        textInputDragAnchor = extendSelection ? mediaSearchInput.GetAnchor() : mediaSearchInput.GetCaret();
                        SetCapture(hWnd);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (activeTextInputField == TextInputField::MediaSearch) { BlurTextInput(); }
                    if (PtInRectF(mediaSortBtnBounds, x, y)) {
                        OpenMediaSortContextMenu(mediaSortBtnBounds.left, mediaSortBtnBounds.bottom + 6.0f * dp);
                        return 0;
                    }
                    if (PtInRectF(mediaFlatBtnBounds, x, y)) { SetLibraryGroupMode(LibraryGroupMode::Flat); return 0; }
                    if (PtInRectF(mediaSeriesBtnBounds, x, y)) {
                        SetLibraryGroupMode(settings.smartSeriesDetection ? LibraryGroupMode::Series : LibraryGroupMode::Folder);
                        return 0;
                    }
                    if (PtInRectF(mediaListenBtnBounds, x, y)) { SetLibraryGroupMode(LibraryGroupMode::Listen); return 0; }
                    if (libraryGroupMode == LibraryGroupMode::Listen) {
                        if (PtInRectF(listenAddPodcastBounds, x, y)) { ShowAddPodcastModal(); return 0; }
                        auto resolveFreshFeed = [&](size_t subIdx) -> const PodcastFeed* {
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
                        for (size_t i = 0; i < listenEpisodeBounds.size(); ++i) {
                            if (!PtInRectF(listenEpisodeBounds[i], x, y)) continue;
                            const int feedIdx = (i < listenEpisodeFeedIndex.size()) ? listenEpisodeFeedIndex[i] : -1;
                            if (feedIdx >= 0) {
                                size_t epIdx = 0;
                                for (size_t k = 0; k < i && k < listenEpisodeFeedIndex.size(); ++k) {
                                    if (listenEpisodeFeedIndex[k] == feedIdx) ++epIdx;
                                }
                                const PodcastFeed* fresh = resolveFreshFeed((size_t)feedIdx);
                                if (fresh && epIdx < fresh->episodes.size()) {
                                    if (listenPlaybackActive && listenUrl == fresh->episodes[epIdx].url) {
                                        ToggleListenPause();
                                    } else {
                                        PlayListenStream(fresh->episodes[epIdx].url,
                                                         fresh->episodes[epIdx].title.empty() ? L"Untitled episode" : fresh->episodes[epIdx].title,
                                                         fresh->title.empty() ? L"Podcast" : fresh->title, false);
                                    }
                                    return 0;
                                }
                            }
                            return 0;
                        }
                        for (size_t i = 0; i < listenFeedBounds.size(); ++i) {
                            if (!PtInRectF(listenFeedBounds[i], x, y)) continue;
                            if (podcastSubscriptions.GetFeeds().empty()) { ShowAddPodcastModal(); return 0; }
                            const PodcastFeed* fresh = resolveFreshFeed(i);
                            if (fresh && !fresh->episodes.empty() && x <= listenFeedBounds[i].left + 44.0f * dp) {
                                listenExpandedFeed = (int)i;
                                PlayListenStream(fresh->episodes[0].url,
                                                 fresh->episodes[0].title.empty() ? L"Untitled episode" : fresh->episodes[0].title,
                                                 fresh->title.empty() ? L"Podcast" : fresh->title, false);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            listenExpandedFeed = ((int)i == listenExpandedFeed) ? -1 : (int)i;
                            listenScrollY = 0.0f;
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        for (size_t i = 0; i < listenStationBounds.size(); ++i) {
                            if (!PtInRectF(listenStationBounds[i], x, y)) continue;
                            if (i < listenStations.size() && !listenStations[i].url.empty()) {
                                if (listenPlaybackActive && listenUrl == listenStations[i].url) {
                                    ToggleListenPause();
                                } else {
                                    std::wstring meta = listenStations[i].bitrateKbps > 0
                                        ? (std::to_wstring(listenStations[i].bitrateKbps) + L" kbps")
                                        : L"";
                                    if (!listenStations[i].codec.empty()) {
                                        if (!meta.empty()) meta += L" \x00B7 ";
                                        meta += listenStations[i].codec;
                                    }
                                    if (!listenStations[i].country.empty()) {
                                        if (!meta.empty()) meta += L" \x00B7 ";
                                        meta += listenStations[i].country;
                                    }
                                    if (meta.empty()) meta = L"Live radio stream";
                                    PlayListenStream(listenStations[i].url, listenStations[i].name, meta, true);
                                }
                            }
                            return 0;
                        }
                        return 0;
                    }
                    if (libraryGroupMode != LibraryGroupMode::Flat) {
                        const auto& proj = GetCurrentMediaProjection();
                        float tbH = 32.0f * dp, listTop = viewBounds.top + tbH + 12.0f * dp, cardLeft = viewBounds.left + 1.0f * dp;
                        float cardRight = viewBounds.right - 2.0f * dp;
                        int headerIdx = hoveredMediaGroupHeader;
                        if (headerIdx < 0 && x >= cardLeft && x <= cardRight && y >= listTop && y <= viewBounds.bottom) {
                            float yDIP = (y - listTop + mediaScrollY) / dp;
                            headerIdx = HitTestMediaGroupHeader(yDIP, proj.groups.size());
                        }
                        if (headerIdx >= 0 && headerIdx < (int)proj.groups.size()) {
                            const std::wstring& key = proj.groups[headerIdx].key;
                            // 1. Delete button click
                            float delRight = cardRight - 8.0f * dp, delLeft = cardRight - 28.0f * dp;
                            if (x >= delLeft && x <= delRight) {
                                if (selectedFolderKeys.count(key) == 0) { selectedFolderKeys.clear(); selectedFolderKeys.insert(key); }
                                RemoveSelectedFolders();
                                return 0;
                            }
                            // 2. Drag handle click
                            float dragHandleLeft = cardLeft, dragHandleRight = cardLeft + 22.0f * dp;
                            if (libraryGroupMode == LibraryGroupMode::Folder && x >= dragHandleLeft && x <= dragHandleRight) {
                                activeDragType = DragType::MediaFolder;
                                dragFromIndex = headerIdx;
                                dragToIndex = headerIdx;
                                dragStartY = y;
                                dragCurrentY = y;
                                dragCurrentX = x;
                                SetCapture(hWnd);
                                selectedFolderKeys.clear();
                                selectedFolderKeys.insert(key);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            // 3. Selection & Collapse handling
                            selectedMediaIndices.clear();
                            bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            if (isCtrl) {
                                if (selectedFolderKeys.count(key) > 0) {
                                    selectedFolderKeys.erase(key);
                                } else { selectedFolderKeys.insert(key); }
                                selectionAnchorIndex = headerIdx;
                            } else if (isShift) {
                                selectedFolderKeys.clear();
                                int anchor = (selectionAnchorIndex >= 0 && selectionAnchorIndex < (int)proj.groups.size()) ? selectionAnchorIndex : 0;
                                int startG = std::min(anchor, headerIdx);
                                int endG = std::max(anchor, headerIdx);
                                for (int g = startG; g <= endG; ++g) { selectedFolderKeys.insert(proj.groups[g].key); }
                            } else {
                                selectedFolderKeys.clear();
                                selectedFolderKeys.insert(key);
                                selectionAnchorIndex = headerIdx;
                                if (m_collapsedGroupKeys.count(key) > 0) {
                                    m_collapsedGroupKeys.erase(key);
                                } else { m_collapsedGroupKeys.insert(key); }
                                ++m_collapsedGroupsGeneration;
                                hoveredMediaIndex = -1;
                                previewPlayer.StopPreview();
                                InvalidateMediaLayoutCache();
                                ClampScrollOffsets();
                            }
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                    const auto& visibleIndices = GetVisibleMediaIndices();
                    int pressedMediaIndex = -1;
                    bool pressedPinnedClose = false;
                    if (!visibleIndices.empty()) {
                        const auto mediaVm = BuildHubRenderViewModel();
                        for (const auto& card : mediaVm.mediaTab.visibleCards) {
                            if (card.isPlaceholder) continue;
                            D2D1_RECT_F visualRect = card.itemRect;
                            visualRect.top -= card.liftY;
                            visualRect.bottom -= card.liftY;
                            if (!PtInRectF(visualRect, x, y)) continue;
                            auto it = std::find(visibleIndices.begin(), visibleIndices.end(), card.originalIndex);
                            if (it == visibleIndices.end()) break;
                            pressedMediaIndex = static_cast<int>(std::distance(visibleIndices.begin(), it));
                            if (card.isPinned) {
                                const float closeSize = 28.0f * dp, closeX = visualRect.right - 14.0f * dp - closeSize;
                                const float closeY = visualRect.top + (RectHeight(visualRect) - closeSize) * 0.5f;
                                const D2D1_RECT_F closeRect = D2D1::RectF( closeX, closeY, closeX + closeSize, closeY + closeSize);
                                pressedPinnedClose = PtInRectF(closeRect, x, y);
                            }
                            break;
                        }
                    }
                    if (visibleIndices.empty()) {
                        if (hoveredMediaEmptyBtn >= 0 && hoveredMediaEmptyBtn < (int)mediaEmptyBtnBounds.size()) {
                            pressedMediaEmptyBtn = hoveredMediaEmptyBtn;
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    } else if (pressedMediaIndex >= 0 && pressedMediaIndex < (int)visibleIndices.size()) {
                        if (pressedPinnedClose) {
                            isLastWatchedCleared = true;
                            isPinnedCloseHovered = false;
                            ShowOSD(IconType::Trash, L"Last Watched Cleared");
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (libraryGroupMode == LibraryGroupMode::Flat && (GetKeyState(VK_CONTROL) & 0x8000) == 0 && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                            HandleItemSelection(FocusPane::MediaList, (size_t)pressedMediaIndex, false, false, visibleIndices.size());
                            ArmCardDrag(DragType::MediaList, pressedMediaIndex, x, y);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        HandleItemSelection(FocusPane::MediaList, (size_t)pressedMediaIndex, isShift, isCtrl, visibleIndices.size());
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                }
                if (currentTab == TabView::Playlists) {
                    float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
                    D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
                    D2D1_RECT_F detailBounds = D2D1::RectF(viewBounds.left + railW + 16.0f * dp, viewBounds.top, viewBounds.right, viewBounds.bottom);
                    if (isInlineRenaming) {
                        int plIdx = -1;
                        for (size_t i = 0; i < library.playlists.size(); ++i) {
                            if (library.playlists[i].id == inlineRenamePlaylistId) { plIdx = (int)i; break; }
                        }
                        bool clickedInsideRename = false;
                        if (plIdx >= 0) {
                            float rItemH = 52.0f * dp, rGap = 6.0f * dp;
                            float rCurY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
                            float rowTop = rCurY + plIdx * (rItemH + rGap);
                            D2D1_RECT_F rItemRect = D2D1::RectF(railBounds.left + 8.0f * dp, rowTop, railBounds.right - 8.0f * dp, rowTop + rItemH);
                            float ringCX = rItemRect.left + 32.0f * dp, ringRadius = 8.5f * dp;
                            D2D1_RECT_F titleRect = D2D1::RectF(ringCX + ringRadius + 12.0f * dp, rItemRect.top + 7.0f * dp, rItemRect.right - 10.0f * dp, rItemRect.top + 27.0f * dp);
                            D2D1_RECT_F renameBox = D2D1::RectF(titleRect.left - 4.0f * dp, rItemRect.top + 4.0f * dp, rItemRect.right - 8.0f * dp, rItemRect.top + 30.0f * dp);
                            if (PtInRectF(renameBox, x, y)) {
                                clickedInsideRename = true;
                                FocusTextInput(TextInputField::InlineRename);
                                const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                SetTextInputCaretFromMouse(x, y, extendSelection);
                                textInputDragging = true;
                                textInputDragAnchor = extendSelection ? inlineRenameInput.GetAnchor() : inlineRenameInput.GetCaret();
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                        if (!clickedInsideRename) { BlurTextInput(); }
                    }
                    Playlist* activePlForToolbar = library.FindPlaylist(activePlaylistId);
                    if (activePlForToolbar && !activePlForToolbar->itemPaths.empty()) {
                        const float toolbarY = detailBounds.top + 14.0f * dp + 50.0f * dp + 16.0f * dp + 112.0f * dp + 16.0f * dp;
                        const float toolbarH = 32.0f * dp;
                        const float paneRight = std::max(detailBounds.left + 120.0f * dp, detailBounds.right - 26.0f * dp);
                        D2D1_RECT_F searchRect = D2D1::RectF(detailBounds.left, toolbarY, detailBounds.left + 230.0f * dp, toolbarY + toolbarH);
                        D2D1_RECT_F toggleRect = D2D1::RectF(paneRight - 64.0f * dp, toolbarY, paneRight, toolbarY + toolbarH);
                        if (PtInRectF(searchRect, x, y)) {
                            FocusTextInput(TextInputField::PlaylistSearch);
                            activeFocusPane = FocusPane::PlaylistDetail;
                            const bool extendSelection = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            SetTextInputCaretFromMouse(x, y, extendSelection);
                            textInputDragging = true;
                            textInputDragAnchor = extendSelection ? playlistSearchInput.GetAnchor() : playlistSearchInput.GetCaret();
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (PtInRectF(toggleRect, x, y)) {
                            playlistGridView = x >= (toggleRect.left + RectWidth(toggleRect) * 0.5f);
                            settings.playlistViewMode = playlistGridView ? "grid" : "list";
                            SaveSettings();
                            BlurTextInput();
                            ClearPlaylistCardHover();
                            playlistDetailScrollY = 0.0f;
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                    if (activeTextInputField == TextInputField::PlaylistSearch) BlurTextInput();
                    if (isPlaylistAddHovered) { ShowNewPlaylistModal(); return 0; }
                    if (hoveredPlaylistRailIndex >= 0 && hoveredPlaylistRailIndex < (int)library.playlists.size()) {
                        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0 && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                            activeFocusPane = FocusPane::PlaylistRail;
                            HandleItemSelection(FocusPane::PlaylistRail, (size_t)hoveredPlaylistRailIndex, false, false, library.playlists.size());
                            activePlaylistId = library.playlists[hoveredPlaylistRailIndex].id;
                            ClearPlaylistCardHover();
                            playlistCardExpands.clear();
                            playlistDetailScrollY = 0;
                            playlistSearchInput.SetText(u"");
                            ArmCardDrag(DragType::PlaylistRail, hoveredPlaylistRailIndex, x, y);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        activeFocusPane = FocusPane::PlaylistRail;
                        HandleItemSelection(FocusPane::PlaylistRail, (size_t)hoveredPlaylistRailIndex, isShift, isCtrl, library.playlists.size());
                        activePlaylistId = library.playlists[hoveredPlaylistRailIndex].id;
                        playlistDetailScrollY = 0.0f;
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (isPlaylistAddFilesHovered) { OpenFileDialogForPlaylist(activePlaylistId); return 0; }
                    // Play All button
                    if (isPlaylistPlayAllHovered) { PlayPlaylist(activePlaylistId, 0); return 0; }
                    // Delete Playlist button
                    if (isPlaylistDeleteHovered) {
                        Playlist* pl = library.FindPlaylist(activePlaylistId);
                        if (pl) {
                            std::string toDelId = pl->id;
                            library.DeletePlaylist(toDelId);
                            if (!library.playlists.empty()) activePlaylistId = library.playlists[0].id;
                            else activePlaylistId = "";
                            ShowOSD(IconType::Trash, L"Playlist Deleted");
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                    // Resume / Spotlight card click
                    if (isPlaylistResumeHovered) {
                        Playlist* pl = library.FindPlaylist(activePlaylistId);
                        if (pl) {
                            std::vector<std::wstring> plItemPaths = library.GetPlaylistPaths(*pl);
                            if (!plItemPaths.empty()) {
                                size_t resIdx = library.GetPlaylistResumeIndex(pl);
                                PlayPlaylist(activePlaylistId, resIdx);
                                return 0;
                            }
                        }
                    }
                    Playlist* pl = library.FindPlaylist(activePlaylistId);
                    if (pl) {
                        std::vector<std::wstring> plItemPaths = library.GetPlaylistPaths(*pl);
                        if (!pl->IsSmart() && hoveredPlaylistDetailDeleteIndex >= 0 && hoveredPlaylistDetailDeleteIndex < (int)plItemPaths.size()) {
                            library.RemoveFromPlaylist(activePlaylistId, (size_t)hoveredPlaylistDetailDeleteIndex);
                            ShowOSD(IconType::Trash, L"Removed from playlist");
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (hoveredPlaylistDetailItemIndex >= 0 && hoveredPlaylistDetailItemIndex < (int)plItemPaths.size()) {
                            if (!pl->IsSmart() && playlistSearchInput.GetText().empty() && (GetKeyState(VK_CONTROL) & 0x8000) == 0 && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                                activeFocusPane = FocusPane::PlaylistDetail;
                                HandleItemSelection(FocusPane::PlaylistDetail, (size_t)hoveredPlaylistDetailItemIndex, false, false, plItemPaths.size());
                                ArmCardDrag(DragType::PlaylistItem, hoveredPlaylistDetailItemIndex, x, y);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                            bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            activeFocusPane = FocusPane::PlaylistDetail;
                            HandleItemSelection(FocusPane::PlaylistDetail, (size_t)hoveredPlaylistDetailItemIndex, isShift, isCtrl, plItemPaths.size());
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                }
                if (currentTab == TabView::History) {
                    D2D1_SIZE_F clearSize = renderer.MeasureString(L"Clear History", FontType::Medium);
                    float clearW = clearSize.width + 24.0f * dp, clearH = 32.0f * dp;
                    float clearTop = viewBounds.top + (46.0f * dp - clearH) * 0.5f;
                    D2D1_RECT_F clearBtnRect = D2D1::RectF(viewBounds.right - clearW - 6.0f * dp, clearTop,
                        viewBounds.right - 6.0f * dp, clearTop + clearH);
                    if (PtInRectF(clearBtnRect, x, y)) {
                        isClearHistoryPressed = true;
                        SetCapture(hWnd);
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                    if (library.history.empty()) {
                        if (hoveredHistoryEmptyBtn >= 0 && hoveredHistoryEmptyBtn < (int)historyEmptyBtnBounds.size()) {
                            pressedHistoryEmptyBtn = hoveredHistoryEmptyBtn;
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    } else if (hoveredHistoryIndex >= 0 && hoveredHistoryIndex < (int)library.history.size()) {
                        if (hoveredHistoryDeleteIndex == hoveredHistoryIndex) {
                            library.RemoveFromHistory((size_t)hoveredHistoryIndex);
                            MarkJumpListDirty();
                            selectedHistoryIndices.clear();
                            keyboardFocusedIndex = library.history.empty() ? -1 : std::min(keyboardFocusedIndex, (int)library.history.size() - 1);
                            ShowOSD(IconType::Trash, L"Removed from History");
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        bool isCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        HandleItemSelection(FocusPane::HistoryList, (size_t)hoveredHistoryIndex, isShift, isCtrl, library.history.size());
                        InvalidateRect(hWnd, NULL, FALSE);
                        return 0;
                    }
                }
                if (currentTab == TabView::Settings) {
                    float sideW = SettingsLayout::kSidebarWidth * dp, catH = SettingsLayout::kSidebarItemHeight * dp;
                    float curCatY = viewBounds.top + SettingsLayout::kSidebarTopInset * dp;
                    SettingsCategory cats[] = {SettingsCategory::Experience, SettingsCategory::Appearance, SettingsCategory::Playback,
                                               SettingsCategory::Subtitles, SettingsCategory::Shortcuts, SettingsCategory::System};
                    // Check sidebar category click
                    for (int i = 0; i < 6; ++i) {
                        float cy = curCatY + i * (catH + SettingsLayout::kSidebarItemGap * dp);
                        if (x >= viewBounds.left + 8.0f * dp && x <= viewBounds.left + sideW - 8.0f * dp && y >= cy && y <= cy + catH) {
                            settingsPressState = { SettingsPressState::Target::Category, i, -1 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                    // Content Pane Hits
                    D2D1_RECT_F contentBounds = SettingsLayout::ContentBounds(viewBounds, dp);
                    float padX = SettingsLayout::kContentPadX * dp;
                    const float headerReserve = currentSettingsCat == SettingsCategory::Subtitles
                        ? SettingsLayout::kSubtitlePageHeaderReserve : SettingsLayout::kPageHeaderReserve;
                    float contentY = SettingsLayout::ContentTop(contentBounds, dp, settingsScrollY, headerReserve);
                    float contentW = RectWidth(contentBounds) - padX * 2.0f;
                    if (currentSettingsCat == SettingsCategory::Experience) {
                        // Mode tiles are row 0 (sub 0..2); features are rows 1..N in
                        // catalog order, present only while Custom is selected.
                        const bool customSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Custom;
                        const auto layout = SettingsLayout::MakeExperienceLayout(contentBounds.left + padX, contentBounds.right - padX, contentY, dp, customSettings);
                        for (int i = 0; i < 3; ++i) {
                            if (PtInRectF(layout.modeTiles[i], x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 0, i };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                        if (customSettings) {
                            for (size_t i = 0; i < FeatureGate::Catalog().size(); ++i) {
                                if (PtInRectF(layout.rows[i], x, y)) {
                                    settingsPressState = { SettingsPressState::Target::OptionRow, static_cast<int>(i) + 1, 0 };
                                    SetCapture(hWnd);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    return 0;
                                }
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Appearance) {
                        // Section 1: Themes (Row 0)
                        contentY += 28.0f * dp;
                        const auto& allThemes = settings.GetAllThemes();
                        float cardW = (contentW - 12.0f * dp) * 0.5f;
                        float cardH = 46.0f * dp;
                        for (size_t i = 0; i < allThemes.size(); ++i) {
                            float col = (float)(i % 2);
                            float row = (float)(i / 2);
                            float tx = contentBounds.left + padX + col * (cardW + 12.0f * dp);
                            float ty = contentY + row * (cardH + 8.0f * dp);
                            if (x >= tx && x <= tx + cardW && y >= ty && y <= ty + cardH) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 0, (int)i };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                        contentY += (((allThemes.size() + 1) / 2) * (cardH + 8.0f * dp)) + 16.0f * dp;
                        // Section 2: Display Language (Row 1)
                        contentY += 28.0f * dp;
                        auto languages = settings.GetAvailableLanguages();
                        float langW = (contentW - 16.0f * dp) / 3.0f;
                        float langH = 34.0f * dp;
                        for (size_t i = 0; i < languages.size(); ++i) {
                            float col = (float)(i % 3);
                            float row = (float)(i / 3);
                            float lx = contentBounds.left + padX + col * (langW + 8.0f * dp);
                            float ly = contentY + row * (langH + 8.0f * dp);
                            if (x >= lx && x <= lx + langW && y >= ly && y <= ly + langH) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 1, (int)i };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Playback) {
                        // Playback uses card geometry shared with the native renderer.
                        const bool simpleSettings = FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Simple;
                        const float cardGap = SettingsLayout::kSectionCardGap * dp, titleH = SettingsLayout::kPlaybackCardTitleHeight * dp;
                        const float rowH = SettingsLayout::kPlaybackBehaviorRowHeight * dp;
                        const float simpleCardH = SettingsLayout::kPlaybackSimpleCardHeight * dp, contentLeft = contentBounds.left + padX;
                        const float contentRight = contentBounds.right - padX;
                        // Section 1: Behavior Switches (Rows 0..3)
                        const float behaviorTop = contentY;
                        contentY = behaviorTop + titleH;
                        for (int i = 0; i < 4; ++i) {
                            D2D1_RECT_F rRect = D2D1::RectF(contentLeft, contentY + i * rowH, contentRight, contentY + (i + 1) * rowH);
                            if (PtInRectF(rRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, i, 0 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                        contentY = behaviorTop + SettingsLayout::kPlaybackBehaviorCardHeight * dp + cardGap;
                        // Section 2: Arrow Key Seek Step (Row 4)
                        const float seekCardTop = contentY;
                        D2D1_RECT_F seekRowRect = D2D1::RectF(contentLeft, seekCardTop + 8.0f * dp,
                                                              contentRight, seekCardTop + simpleCardH - 8.0f * dp);
                        D2D1_RECT_F dropRect = D2D1::RectF(seekRowRect.right - 220.0f * dp, seekRowRect.top + 8.0f * dp, seekRowRect.right - 100.0f * dp, seekRowRect.bottom - 8.0f * dp);
                        D2D1_RECT_F custBtnRect = D2D1::RectF(seekRowRect.right - 92.0f * dp, seekRowRect.top + 8.0f * dp, seekRowRect.right - 10.0f * dp, seekRowRect.bottom - 8.0f * dp);
                        if (PtInRectF(dropRect, x, y)) {
                            settingsPressState = { SettingsPressState::Target::OptionRow, 4, 0 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (PtInRectF(custBtnRect, x, y)) {
                            settingsPressState = { SettingsPressState::Target::OptionRow, 4, 1 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        // Section 3: Frame Seek Step (Row 5)
                        contentY = seekCardTop + simpleCardH + cardGap;
                        const float frameCardTop = contentY;
                        D2D1_RECT_F frameRowRect = D2D1::RectF(contentLeft, frameCardTop + 8.0f * dp,
                                                               contentRight, frameCardTop + simpleCardH - 8.0f * dp);
                        D2D1_RECT_F fDropRect = D2D1::RectF(frameRowRect.right - 220.0f * dp, frameRowRect.top + 8.0f * dp, frameRowRect.right - 100.0f * dp, frameRowRect.bottom - 8.0f * dp);
                        D2D1_RECT_F fCustBtnRect = D2D1::RectF(frameRowRect.right - 92.0f * dp, frameRowRect.top + 8.0f * dp, frameRowRect.right - 10.0f * dp, frameRowRect.bottom - 8.0f * dp);
                        if (PtInRectF(fDropRect, x, y)) {
                            settingsPressState = { SettingsPressState::Target::OptionRow, 5, 0 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        if (PtInRectF(fCustBtnRect, x, y)) {
                            settingsPressState = { SettingsPressState::Target::OptionRow, 5, 1 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        // Section 4: Default Playback Speed (Row 6)
                        contentY = frameCardTop + simpleCardH + cardGap;
                        const float speedCardTop = contentY;
                        double speeds[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
                        float spdW = (contentW - 25.0f * dp) / 6.0f;
                        float pH = 34.0f * dp;
                        contentY = speedCardTop + 42.0f * dp;
                        for (int i = 0; i < 6; ++i) {
                            D2D1_RECT_F spdRect = D2D1::RectF(contentLeft + i * (spdW + 5.0f * dp), contentY,
                                                             contentLeft + i * (spdW + 5.0f * dp) + spdW, contentY + pH);
                            if (PtInRectF(spdRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 6, i };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                        // Sections 5-8 hide in Simple mode; the press targets must hide with them.
                        if (!simpleSettings) {
                            // Section 5: Custom MPV Configuration (Rows 7 & 8)
                            const float mpvCardTop = speedCardTop + SettingsLayout::kPlaybackSpeedCardHeight * dp + cardGap;
                            contentY = mpvCardTop + titleH;
                            D2D1_RECT_F mpvSwitchRect = D2D1::RectF(contentLeft, contentY, contentRight,
                                                                   contentY + SettingsLayout::kPlaybackMpvSwitchHeight * dp);
                            if (PtInRectF(mpvSwitchRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 7, 0 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            contentY = mpvCardTop + SettingsLayout::kPlaybackMpvActionTop * dp;
                            const float mpvActCardH = SettingsLayout::kPlaybackMpvActionHeight * dp;
                            const float mpvActGap = SettingsLayout::kPlaybackMpvActionGap * dp;
                            for (int i = 0; i < 3; ++i) {
                                D2D1_RECT_F actRowRect = D2D1::RectF(contentLeft, contentY + i * (mpvActCardH + mpvActGap),
                                                                     contentRight, contentY + i * (mpvActCardH + mpvActGap) + mpvActCardH);
                                if (PtInRectF(actRowRect, x, y)) {
                                    settingsPressState = { SettingsPressState::Target::OptionRow, 8, i };
                                    SetCapture(hWnd);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    return 0;
                                }
                            }
                            // Section 6: System Integration (Rows 9, 10 & 11)
                            contentY = mpvCardTop + SettingsLayout::kPlaybackMpvCardHeight * dp + cardGap;
                            for (int i = 9; i <= 11; ++i) {
                                D2D1_RECT_F sysCardRect = D2D1::RectF(contentLeft, contentY, contentRight, contentY + simpleCardH);
                                if (PtInRectF(sysCardRect, x, y)) {
                                    settingsPressState = { SettingsPressState::Target::OptionRow, i, 0 };
                                    SetCapture(hWnd);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    return 0;
                                }
                                contentY += simpleCardH + cardGap;
                            }
                            // Section 7: Network Cache Forward (Row 12)
                            D2D1_RECT_F fwdCacheRowRect = D2D1::RectF(contentLeft, contentY + 8.0f * dp,
                                                                      contentRight, contentY + simpleCardH - 8.0f * dp);
                            D2D1_RECT_F fwdDropRect = D2D1::RectF(fwdCacheRowRect.right - 220.0f * dp, fwdCacheRowRect.top + 8.0f * dp, fwdCacheRowRect.right - 100.0f * dp, fwdCacheRowRect.bottom - 8.0f * dp);
                            D2D1_RECT_F fwdCustBtnRect = D2D1::RectF(fwdCacheRowRect.right - 92.0f * dp, fwdCacheRowRect.top + 8.0f * dp, fwdCacheRowRect.right - 10.0f * dp, fwdCacheRowRect.bottom - 8.0f * dp);
                            if (PtInRectF(fwdDropRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 12, 0 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            if (PtInRectF(fwdCustBtnRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 12, 1 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            // Section 8: Network Cache Back (Row 13)
                            contentY += simpleCardH + cardGap;
                            D2D1_RECT_F backCacheRowRect = D2D1::RectF(contentLeft, contentY + 8.0f * dp,
                                                                       contentRight, contentY + simpleCardH - 8.0f * dp);
                            D2D1_RECT_F backDropRect = D2D1::RectF(backCacheRowRect.right - 220.0f * dp, backCacheRowRect.top + 8.0f * dp, backCacheRowRect.right - 100.0f * dp, backCacheRowRect.bottom - 8.0f * dp);
                            D2D1_RECT_F backCustBtnRect = D2D1::RectF(backCacheRowRect.right - 92.0f * dp, backCacheRowRect.top + 8.0f * dp, backCacheRowRect.right - 10.0f * dp, backCacheRowRect.bottom - 8.0f * dp);
                            if (PtInRectF(backDropRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 13, 0 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                            if (PtInRectF(backCustBtnRect, x, y)) {
                                settingsPressState = { SettingsPressState::Target::OptionRow, 13, 1 };
                                SetCapture(hWnd);
                                InvalidateRect(hWnd, NULL, FALSE);
                                return 0;
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                        const float usableW = contentW, subLeft = contentBounds.left + padX, controlH = SettingsLayout::kControlHeight * dp;
                        const float controlGap = 8.0f * dp, trackCardH = SettingsLayout::kSubtitleTrackCardHeight * dp;
                        const float cardGap = SettingsLayout::kSubtitleCardGap * dp;
                        const float trackW = (usableW - 6.0f * controlGap) / 7.0f;
                        const float cardRight = subLeft + usableW + 10.0f * dp;
                        auto pressOption = [&](int row, int sub) {
                            settingsPressState = { SettingsPressState::Target::OptionRow, row, sub };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                        };
                        // Live preview is visual-only; the first interactive
                        // card starts after it and the shared card gap.
                        float cardTop = contentY + SettingsLayout::kSubtitlePreviewCardHeight * dp + cardGap;
                        // Preferred audio track (Row 0)
                        float controlsTop = cardTop + 44.0f * dp;
                        for (int i = 0; i < 7; ++i) {
                            D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (trackW + controlGap), controlsTop,
                                                           subLeft + i * (trackW + controlGap) + trackW, controlsTop + controlH);
                            if (PtInRectF(rect, x, y)) { pressOption(0, i); return 0; }
                        }
                        cardTop += trackCardH + cardGap;
                        // Preferred subtitle track (Row 1)
                        controlsTop = cardTop + 44.0f * dp;
                        for (int i = 0; i < 7; ++i) {
                            D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (trackW + controlGap), controlsTop,
                                                           subLeft + i * (trackW + controlGap) + trackW, controlsTop + controlH);
                            if (PtInRectF(rect, x, y)) { pressOption(1, i); return 0; }
                        }
                        cardTop += trackCardH + cardGap;
                        // Subtitle size slider (Row 2)
                        const float sliderLeft = subLeft + 22.0f * dp + 64.0f * dp + 18.0f * dp, sliderRight = cardRight - 22.0f * dp;
                        D2D1_RECT_F sliderHit = D2D1::RectF(sliderLeft - 14.0f * dp, cardTop + 36.0f * dp,
                                                             sliderRight + 14.0f * dp, cardTop + 112.0f * dp);
                        if (PtInRectF(sliderHit, x, y)) {
                            const int pixels = SubtitleSizeFromSliderX(x, sliderLeft, sliderRight);
                            settingsPressState = { SettingsPressState::Target::OptionRow, 2, pixels - 14 };
                            subtitleSizeDragging = true;
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        cardTop += SettingsLayout::kSubtitleFontCardHeight * dp + cardGap;
                        // Subtitle color swatches (Row 3)
                        const char* subColors[] = { "#cad3f5", "#eed49f", "#a6da95", "#91d7e3", "#f5a97f", "#ed8796" };
                        const float swatchRadius = 15.0f * dp, swatchGap = 16.0f * dp, swatchY = cardTop + 54.0f * dp;
                        for (int i = 0; i < 6; ++i) {
                            const float cx = subLeft + 22.0f * dp + swatchRadius + i * (2.0f * swatchRadius + swatchGap);
                            D2D1_RECT_F hit = D2D1::RectF(cx - 22.0f * dp, swatchY - 22.0f * dp, cx + 22.0f * dp, swatchY + 22.0f * dp);
                            if (PtInRectF(hit, x, y)) { pressOption(3, i); return 0; }
                        }
                        cardTop += SettingsLayout::kSubtitleColorCardHeight * dp + cardGap;
                        // Background style previews (Row 4)
                        const float styleW = (usableW - 3.0f * controlGap) / 4.0f;
                        const float styleTop = cardTop + 42.0f * dp, styleH = 92.0f * dp;
                        for (int i = 0; i < 4; ++i) {
                            D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (styleW + controlGap), styleTop,
                                                           subLeft + i * (styleW + controlGap) + styleW, styleTop + styleH);
                            if (PtInRectF(rect, x, y)) { pressOption(4, i); return 0; }
                        }
                        cardTop += SettingsLayout::kSubtitleStyleCardHeight * dp + cardGap;
                        // Vertical position previews (Row 5), ordered Top -> Elevated -> Bottom.
                        const float posW = (usableW - 2.0f * controlGap) / 3.0f;
                        const float posTop = cardTop + 42.0f * dp, posH = 92.0f * dp;
                        for (int i = 0; i < 3; ++i) {
                            D2D1_RECT_F rect = D2D1::RectF(subLeft + i * (posW + controlGap), posTop,
                                                           subLeft + i * (posW + controlGap) + posW, posTop + posH);
                            if (PtInRectF(rect, x, y)) { pressOption(5, i); return 0; }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
                        float resetW = 150.0f * dp, resetH = 26.0f * dp;
                        const float pageHeaderTop = contentY - SettingsLayout::kPageHeaderReserve * dp;
                        D2D1_RECT_F resetRect = D2D1::RectF(contentBounds.right - padX - resetW, pageHeaderTop - 2.0f * dp, contentBounds.right - padX, pageHeaderTop - 2.0f * dp + resetH);
                        if (PtInRectF(resetRect, x, y)) {
                            settingsPressState = { SettingsPressState::Target::ResetShortcuts, -1, -1 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                        float scTop = contentY + 32.0f * dp;
                        auto scList = settings.GetShortcutActionListRef();
                        size_t totalCount = scList.size();
                        size_t mid = (totalCount + 1) / 2;
                        float colGap = 16.0f * dp;
                        float colW = (contentW - colGap) * 0.5f;
                        float scRowH = 38.0f * dp, rowGap = 5.0f * dp;
                        for (size_t rowIdx = 0; rowIdx < mid; ++rowIdx) {
                            float rTop = scTop + rowIdx * (scRowH + rowGap);
                            if (rowIdx < totalCount) {
                                D2D1_RECT_F leftRect = D2D1::RectF(contentBounds.left + padX, rTop, contentBounds.left + padX + colW, rTop + scRowH);
                                if (PtInRectF(leftRect, x, y)) {
                                    settingsPressState = { SettingsPressState::Target::OptionRow, (int)rowIdx, 0 };
                                    SetCapture(hWnd);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    return 0;
                                }
                            }
                            if (mid + rowIdx < totalCount) {
                                D2D1_RECT_F rightRect = D2D1::RectF(contentBounds.left + padX + colW + colGap, rTop, contentBounds.right - padX, rTop + scRowH);
                                if (PtInRectF(rightRect, x, y)) {
                                    settingsPressState = { SettingsPressState::Target::OptionRow, (int)rowIdx, 1 };
                                    SetCapture(hWnd);
                                    InvalidateRect(hWnd, NULL, FALSE);
                                    return 0;
                                }
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::System) {
                        const int pressedRow = HitTestSystemSettingsRow(contentBounds, contentY, x, y);
                        if (pressedRow >= 0) {
                            settingsPressState = { SettingsPressState::Target::SystemAction, pressedRow, -1 };
                            SetCapture(hWnd);
                            InvalidateRect(hWnd, NULL, FALSE);
                            return 0;
                        }
                    }
                }
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (isJumpAheadPressed) {
                isJumpAheadPressed = false;
                if (GetCapture() == hWnd) ReleaseCapture();
                if (isJumpAheadHovered) {
                    if (jumpAheadTargetTime > 0.0) {
                        player.Seek(jumpAheadTargetTime, true);
                        ShowOSD(IconType::Play, L"Jumped ahead");
                        AnnounceStatus(L"Jumped ahead to " + FormatDuration(jumpAheadTargetTime), true);
                    }
                    isJumpAheadVisible = false;
                    jumpAheadTimer = 0.0f;
                    AnimateVisual(jumpAheadAnim, 0.0f, MotionTokens::kFast, MotionTokens::Standard());
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (autoscroll.suppressNextClick) { autoscroll.suppressNextClick = false; return 0; }
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            if (isPiPVolumeDragging) { isPiPVolumeDragging = false; if (GetCapture() == hWnd) ReleaseCapture(); }
            if (isListenVolumeDragging) {
                isListenVolumeDragging = false;
                if (GetCapture() == hWnd) ReleaseCapture();
                SaveSettings();
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (isListenSeekDragging) {
                isListenSeekDragging = false;
                if (GetCapture() == hWnd) ReleaseCapture();
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (isPiPPotentialWindowDrag || isPiPDraggingWindow || isPiPDragOccurred) {
                const bool wasDragging = isPiPDraggingWindow || isPiPDragOccurred;
                isPiPPotentialWindowDrag = false;
                isPiPDraggingWindow = false;
                isPiPDragOccurred = false;
                if (GetCapture() == hWnd || GetCapture() == hOverlayWnd) {
                    ReleaseCapture();
                }
                SyncOverlayPosition();
                if (!wasDragging) {
                    commandDispatcher.Execute(AppCommand::TogglePlay);
                    InvalidateRect(hWnd, NULL, FALSE);
                } else {
                    PersistPiPGeometry();
                }
                return 0;
            }
            const bool modalOkWasPressed = isModalOkPressed, modalCancelWasPressed = isModalCancelPressed;
            const bool clearHistoryWasPressed = isClearHistoryPressed;
            const int mediaEmptyBtnWasPressed = pressedMediaEmptyBtn, historyEmptyBtnWasPressed = pressedHistoryEmptyBtn;
            const int navWasPressed = pressedNavIndex;
            const SettingsPressState pressed = settingsPressState;
            settingsPressState = {};
            if (GetCapture() == hWnd && !textInputDragging && !isScrubbing && !isPiPVolumeDragging && activeDragType == DragType::None && !IsAutoscrolling())
                ReleaseCapture();
            if (navWasPressed >= 0) {
                pressedNavIndex = -1;
                // Re-derive the target under the pointer at release time; a
                // press that slides off the control before release cancels.
                int releaseIdx = -1;
                if (isNavOpenHovered) releaseIdx = 4;
                else if (isNavSettingsHovered) releaseIdx = 3;
                else if (hoveredTab == TabView::Media) releaseIdx = 0;
                else if (hoveredTab == TabView::Playlists) releaseIdx = 1;
                else if (hoveredTab == TabView::History) releaseIdx = 2;
                if (releaseIdx == navWasPressed) {
                    if (releaseIdx == 4) { OpenFileDialog(); } else if (releaseIdx == 3) {
                        currentTab = TabView::Settings;
                        activeFocusPane = FocusPane::SettingsCategoryList;
                        topNavFocusedIndex = 3;
                        keyboardFocusedIndex = (int)currentSettingsCat;
                    } else {
                        currentTab = (releaseIdx == 0) ? TabView::Media : (releaseIdx == 1) ? TabView::Playlists : TabView::History;
                        if (currentTab == TabView::Media) {
                            activeFocusPane = FocusPane::MediaList;
                            topNavFocusedIndex = 0;
                            keyboardFocusedIndex = 0;
                            // No implicit selection: seeding index 0 here used to
                            // mark the pinned last-watched card as selected, which
                            // then leaked into "Add N items" background actions.
                        } else if (currentTab == TabView::Playlists) {
                            activeFocusPane = FocusPane::PlaylistRail;
                            topNavFocusedIndex = 1;
                            keyboardFocusedIndex = 0;
                            selectedPlaylistIndices = { 0 };
                        } else if (currentTab == TabView::History) {
                            activeFocusPane = FocusPane::HistoryList;
                            topNavFocusedIndex = 2;
                            keyboardFocusedIndex = 0;
                            selectedHistoryIndices = { 0 };
                        }
                    }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (mediaEmptyBtnWasPressed >= 0) {
                pressedMediaEmptyBtn = -1;
                if (hoveredMediaEmptyBtn == mediaEmptyBtnWasPressed) {
                    if (library.mediaItems.empty()) {
                        if (mediaEmptyBtnWasPressed == 0) {
                            OpenFileDialog();
                        } else if (mediaEmptyBtnWasPressed == 1) { AddWatchedFolderPrompt(); }
                    } else {
                        if (mediaEmptyBtnWasPressed == 0) { ClearMediaSearch(); SetLibraryGroupMode(LibraryGroupMode::Flat); }
                    }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (historyEmptyBtnWasPressed >= 0) {
                pressedHistoryEmptyBtn = -1;
                if (hoveredHistoryEmptyBtn == historyEmptyBtnWasPressed) {
                    if (historyEmptyBtnWasPressed == 0) { OpenFileDialog(); }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (modalOkWasPressed) {
                isModalOkPressed = false;
                bool confirmHovered = isModalOkHovered;
                if (!confirmHovered && activeModal != ModalType::None) {
                    // A mouse-up can arrive without a preceding move event (notably after a modal is created from a context menu). Resolve
                    // the release from its coordinates instead of relying only on the cached hover flag.
                    RECT rc{};
                    GetClientRect(hWnd, &rc);
                    const float clientW = static_cast<float>(rc.right - rc.left);
                    const float clientH = static_cast<float>(rc.bottom - rc.top);
                    const bool isInputModal = (activeModal == ModalType::NewPlaylist || activeModal == ModalType::NewSmartPlaylist || activeModal == ModalType::EditSmartPlaylist || activeModal == ModalType::SeekStepInput || activeModal == ModalType::FrameSeekInput || activeModal == ModalType::CustomSettingInput || activeModal == ModalType::BookmarkInput || activeModal == ModalType::EditMetadataOverrides || activeModal == ModalType::SaveSubtitleProfile || activeModal == ModalType::RenameSubtitleProfile || activeModal == ModalType::JumpToTime || activeModal == ModalType::CustomAspectInput || activeModal == ModalType::CustomRotationInput);
                    const float maxBoxW = isInputModal ? 460.0f * dp : 500.0f * dp;
                    const float boxW = std::min(maxBoxW, std::max(320.0f * dp, clientW - 48.0f * dp));
                    const float boxH = (isInputModal ? 204.0f : Presentation::ConfirmationHeight) * dp;
                    const float boxLeft = (clientW - boxW) * 0.5f;
                    const float boxTop = (clientH - boxH) * 0.5f;
                    const float confirmW = isInputModal ? 92.0f * dp : std::max(110.0f * dp, renderer.MeasureString(modalConfirmText, FontType::Medium).width + 32.0f * dp);
                    const float buttonY = boxTop + boxH - 56.0f * dp, confirmRight = boxLeft + boxW - 24.0f * dp;
                    const D2D1_RECT_F confirmRect = D2D1::RectF(confirmRight - confirmW, buttonY, confirmRight, buttonY + 36.0f * dp);
                    confirmHovered = PtInRectF(confirmRect, x, y);
                }
                if (confirmHovered) DismissActiveModal(true);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (modalCancelWasPressed) {
                isModalCancelPressed = false;
                if (isModalCancelHovered) { DismissActiveModal(false); }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (clearHistoryWasPressed) {
                isClearHistoryPressed = false;
                if (isClearHistoryHovered) {
                    ShowConfirmationModal(L"Clear Watch History", L"Are you sure you want to erase all watch history and playback progress? This action cannot be undone.", L"Clear History", [this]() {
                        library.ClearHistory();
                        MarkJumpListDirty();
                        selectedHistoryIndices.clear();
                        isLastWatchedCleared = true;
                        InvalidateMediaLayoutCache();
                        InvalidateRect(hWnd, NULL, FALSE);
                    });
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (pressed.target != SettingsPressState::Target::None) {
                if (pressed.target == SettingsPressState::Target::Category) {
                    SettingsCategory cats[] = { SettingsCategory::Experience, SettingsCategory::Appearance, SettingsCategory::Playback,
                                                SettingsCategory::Subtitles, SettingsCategory::Shortcuts, SettingsCategory::System };
                    if (pressed.index >= 0 && pressed.index < 6) {
                        currentSettingsCat = cats[pressed.index];
                        settingsScrollY = 0.0f;
                        hoveredSettingsRow = -1;
                        activeFocusPane = FocusPane::SettingsCategoryList;
                        keyboardFocusedIndex = pressed.index;
                    }
                } else if (pressed.target == SettingsPressState::Target::ResetShortcuts) {
                    ShowConfirmationModal(L"Reset Shortcuts", L"Are you sure you want to reset all keyboard shortcuts to their default key assignments?", L"Reset", [this]() {
                        settings.ResetShortcuts();
                        SaveSettings();
                    });
                } else if (pressed.target == SettingsPressState::Target::SystemAction) {
                    // Re-derive the row under the release coordinates from the same
                    // geometry the renderer uses; a press that slides off the row
                    // (or into the divider gap) before release cancels, matching the
                    // nav-bar press model.
                    RECT clientRc{};
                    GetClientRect(hWnd, &clientRc);
                    const float winW = (float)(clientRc.right - clientRc.left);
                    const float winH = (float)(clientRc.bottom - clientRc.top);
                    const D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
                    const D2D1_RECT_F contentBounds = SettingsLayout::ContentBounds(viewBounds, dp);
                    const float contentY = SettingsLayout::ContentTop(contentBounds, dp, settingsScrollY);
                    const int releaseRow = HitTestSystemSettingsRow(contentBounds, contentY, x, y);
                    if (releaseRow == pressed.index) {
                        activeFocusPane = FocusPane::SettingsContent;
                        keyboardFocusedIndex = pressed.index;
                        keyboardFocusedSubIndex = 0;
                        HandleGlobalKeyDown(VK_RETURN, 0);
                    }
                } else if (pressed.target == SettingsPressState::Target::OptionRow) {
                    if (currentSettingsCat == SettingsCategory::Experience) {
                        if (pressed.index == 0 && pressed.subIndex >= 0 && pressed.subIndex <= 2) {
                            ApplyExperienceModeSelection(static_cast<ExperienceMode>(pressed.subIndex));
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 0;
                            keyboardFocusedSubIndex = pressed.subIndex;
                        } else if (pressed.index >= 1 &&
                                   FeatureGate::ModeFromString(settings.experienceMode) == ExperienceMode::Custom) {
                            const auto& catalog = FeatureGate::Catalog();
                            const int fi = pressed.index - 1;
                            if (fi < static_cast<int>(catalog.size())) {
                                ToggleExperienceFeature(catalog[fi].key);
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = pressed.index;
                                keyboardFocusedSubIndex = 0;
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Appearance) {
                        if (pressed.index == 0) {
                            const auto& allThemes = settings.GetAllThemes();
                            if (pressed.subIndex >= 0 && pressed.subIndex < (int)allThemes.size()) {
                                settings.themePreset = allThemes[pressed.subIndex].id;
                                settings.UpdateThemeColors();
                                ApplyWindowTheme();
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 0;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        } else if (pressed.index == 1) {
                            auto languages = settings.GetAvailableLanguages();
                            if (pressed.subIndex >= 0 && pressed.subIndex < (int)languages.size()) {
                                settings.appLanguage = languages[pressed.subIndex].tag;
                                Localization::Instance().SetLocale(settings.appLanguage);
                                renderer.CreateFonts();
                                overlayRenderer.CreateFonts();
                                SaveSettings();
                                ShowOSD(IconType::SettingsGear, Loc::Format("osd.language_applied", Utf8ToUtf16(languages[pressed.subIndex].displayName)));
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 1;
                                keyboardFocusedSubIndex = pressed.subIndex;
                                InvalidateRect(hWnd, NULL, FALSE);
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Playback) {
                        if (pressed.index >= 0 && pressed.index <= 3) {
                            int i = pressed.index;
                            if (i == 0) {
                                settings.autoplay = !settings.autoplay;
                                AnimateVisual(autoplayToggleAnim, settings.autoplay ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                            } else if (i == 1) {
                                settings.rememberPos = !settings.rememberPos;
                                AnimateVisual(rememberPosToggleAnim, settings.rememberPos ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                            } else if (i == 2) {
                                settings.hwAccel = !settings.hwAccel;
                                AnimateVisual(hwAccelToggleAnim, settings.hwAccel ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                            } else if (i == 3) {
                                settings.loop = !settings.loop;
                                AnimateVisual(loopToggleAnim, settings.loop ? 1.0f : 0.0f, MotionTokens::kToggle, MotionTokens::Standard());
                            }
                            SaveSettings();
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = i;
                            keyboardFocusedSubIndex = 0;
                        } else if (pressed.index == 4) {
                            if (pressed.subIndex == 0) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 4;
                                keyboardFocusedSubIndex = 0;
                                OpenSeekStepContextMenu(x, y);
                            } else if (pressed.subIndex == 1) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 4;
                                keyboardFocusedSubIndex = 1;
                                ShowCustomSeekStepModal();
                            }
                        } else if (pressed.index == 5) {
                            if (pressed.subIndex == 0) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 5;
                                keyboardFocusedSubIndex = 0;
                                OpenFrameSeekContextMenu(x, y);
                            } else if (pressed.subIndex == 1) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 5;
                                keyboardFocusedSubIndex = 1;
                                ShowCustomFrameSeekModal();
                            }
                        } else if (pressed.index == 6) {
                            double speeds[] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
                            if (pressed.subIndex >= 0 && pressed.subIndex < 6) {
                                settings.defaultSpeed = speeds[pressed.subIndex];
                                player.SetPlaybackRate(speeds[pressed.subIndex]);
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 6;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        } else if (pressed.index == 7) {
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 7;
                            keyboardFocusedSubIndex = 0;
                            ToggleCustomMpvConfig();
                        } else if (pressed.index == 8) {
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 8;
                            keyboardFocusedSubIndex = pressed.subIndex;
                            if (pressed.subIndex == 0) EditCustomMpvConfigFile();
                            else if (pressed.subIndex == 1) ResetCustomMpvConfigToDefault();
                            else if (pressed.subIndex == 2) RevertCustomMpvConfigToLastKnownGood();
                        } else if (pressed.index == 9) {
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 9;
                            keyboardFocusedSubIndex = 0;
                            settings.globalHotkeysEnabled = !settings.globalHotkeysEnabled;
                            ApplyGlobalHotkeys();
                            SaveSettings();
                            ShowOSD(IconType::SettingsGear, settings.globalHotkeysEnabled
                                ? (L"Global Hotkeys: On (" + std::to_wstring(registeredGlobalHotkeys) + L" registered)")
                                : L"Global Hotkeys: Off");
                        } else if (pressed.index == 10) {
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 10;
                            keyboardFocusedSubIndex = 0;
                            settings.trayIconEnabled = !settings.trayIconEnabled;
                            if (!settings.trayIconEnabled && isMinimizedToTray) { RestoreFromTray(); }
                            UpdateTrayIcon();
                            SaveSettings();
                            ShowOSD(IconType::SettingsGear, settings.trayIconEnabled ? L"Tray Icon: On" : L"Tray Icon: Off");
                        } else if (pressed.index == 11) {
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 11;
                            keyboardFocusedSubIndex = 0;
                            settings.minimizeToTray = !settings.minimizeToTray;
                            SaveSettings();
                            ShowOSD(IconType::SettingsGear, settings.minimizeToTray ? L"Minimize to Tray: On" : L"Minimize to Tray: Off");
                        } else if (pressed.index == 12) {
                            if (pressed.subIndex == 0) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 12;
                                keyboardFocusedSubIndex = 0;
                                OpenNetworkForwardCacheContextMenu(x, y);
                            } else if (pressed.subIndex == 1) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 12;
                                keyboardFocusedSubIndex = 1;
                                ShowCustomNetworkForwardCacheModal();
                            }
                        } else if (pressed.index == 13) {
                            if (pressed.subIndex == 0) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 13;
                                keyboardFocusedSubIndex = 0;
                                OpenNetworkBackCacheContextMenu(x, y);
                            } else if (pressed.subIndex == 1) {
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 13;
                                keyboardFocusedSubIndex = 1;
                                ShowCustomNetworkBackCacheModal();
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Subtitles) {
                        const char* audioLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "auto" };
                        const char* subLangCodes[] = { "eng", "jpn", "fra", "deu", "spa", "ita", "off" };
                        if (pressed.index == 0 && pressed.subIndex >= 0 && pressed.subIndex < 7) {
                            settings.audioLang = audioLangCodes[pressed.subIndex];
                            ApplyPlayerPreferences();
                            SaveSettings();
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 0;
                            keyboardFocusedSubIndex = pressed.subIndex;
                        } else if (pressed.index == 1 && pressed.subIndex >= 0 && pressed.subIndex < 7) {
                            if (pressed.subIndex == 6) {
                                settings.subtitlesEnabled = false;
                            } else { settings.subLang = subLangCodes[pressed.subIndex]; settings.subtitlesEnabled = true; }
                            ApplyPlayerPreferences();
                            SaveSettings();
                            activeFocusPane = FocusPane::SettingsContent;
                            keyboardFocusedIndex = 1;
                            keyboardFocusedSubIndex = pressed.subIndex;
                        } else if (pressed.index == 2) {
                            if (pressed.subIndex >= 0 && pressed.subIndex <= 18) {
                                settings.subFontSize = SubtitleSizeToString(14 + pressed.subIndex);
                                ApplyPlayerPreferences();
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 2;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        } else if (pressed.index == 3) {
                            const char* colHex[] = { "#cad3f5", "#eed49f", "#a6da95", "#91d7e3", "#f5a97f", "#ed8796" };
                            if (pressed.subIndex >= 0 && pressed.subIndex < 6) {
                                settings.subColorHex = colHex[pressed.subIndex];
                                ApplyPlayerPreferences();
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 3;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        } else if (pressed.index == 4) {
                            const char* bgKeys[] = { "vlc-outline", "translucent", "solid", "clean" };
                            if (pressed.subIndex >= 0 && pressed.subIndex < 4) {
                                settings.subBgStyle = bgKeys[pressed.subIndex];
                                ApplyPlayerPreferences();
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 4;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        } else if (pressed.index == 5) {
                            const char* posKeys[] = { "top", "elevated", "bottom" };
                            if (pressed.subIndex >= 0 && pressed.subIndex < 3) {
                                settings.subPosition = posKeys[pressed.subIndex];
                                ApplyPlayerPreferences();
                                SaveSettings();
                                activeFocusPane = FocusPane::SettingsContent;
                                keyboardFocusedIndex = 5;
                                keyboardFocusedSubIndex = pressed.subIndex;
                            }
                        }
                    } else if (currentSettingsCat == SettingsCategory::Shortcuts) {
                        auto scList = settings.GetShortcutActionListRef();
                        size_t mid = (scList.size() + 1) / 2;
                        size_t actualIdx = (pressed.subIndex == 1) ? (mid + pressed.index) : (size_t)pressed.index;
                        if (actualIdx < scList.size())
                            StartShortcutCapture(scList[actualIdx].first, scList[actualIdx].second, (int)pressed.index + 1, pressed.subIndex);
                    }
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            if (textInputDragging) { textInputDragging = false; ReleaseCapture(); return 0; }
            if (isScrubbing) {
                if (scrubTargetTime >= 0.0) { player.Seek(scrubTargetTime, true); }
                isScrubbing = false;
                scrubTargetTime = -1.0;
                ReleaseCapture();
            }
            if (pendingDragType != DragType::None) { CancelCardDrag(); return 0; }
            if (activeDragType != DragType::None) {
                RECT client{}; GetClientRect(hWnd, &client);
                const float dropDpi = renderer.GetDpiScale();
                const float dropTop = activeDragType == DragType::PlaylistItem ? 336.0f * dropDpi : 128.0f * dropDpi;
                if (!isPlaylistsNavDropHovered && (x < 18.0f * dropDpi || x > client.right - 18.0f * dropDpi ||
                    y < dropTop || y > client.bottom - 14.0f * dropDpi)) { CancelCardDrag(); return 0; }
                if (activeDragType == DragType::MediaList) {
                    if (isPlaylistsNavDropHovered) {
                        if (dragFromIndex >= 0 && dragFromIndex < (int)library.mediaItems.size()) {
                            const auto* source = dragFromIndex < static_cast<int>(dragPathSnapshot.size())
                                ? library.FindByPath(dragPathSnapshot[dragFromIndex]) : nullptr;
                            if (!source) { CancelCardDrag(); return 0; }
                            const auto item = *source;
                            if (library.playlists.empty()) {
                                Playlist newPl;
                                newPl.id = "pl_favorites";
                                newPl.name = "Favorites";
                                library.playlists.push_back(newPl);
                                activePlaylistId = newPl.id;
                                ArmCreatedPlaylistUndo(newPl.id);
                            }
                            Playlist* pActivePl = library.FindPlaylist(activePlaylistId);
                            if (!pActivePl && !library.playlists.empty()) {
                                pActivePl = &library.playlists[0];
                                activePlaylistId = pActivePl->id;
                            }
                            if (pActivePl && !pActivePl->IsSmart()) {
                                AddToPlaylistWithUndo(pActivePl->id, { item.filePath });
                                SaveLibrary();
                                currentTab = TabView::Playlists;
                                ShowOSD(IconType::FolderPlaylist, L"Added to \"" + Utf8ToUtf16(pActivePl->name) + L"\"");
                            }
                        }
                    } else if (dragFromIndex != dragToIndex && dragFromIndex >= 0 && dragToIndex >= 0 && dragFromIndex < (int)library.mediaItems.size() && dragToIndex < (int)library.mediaItems.size()) {
                        const auto& visible = GetVisibleMediaIndices();
                        std::vector<std::wstring> currentPaths;
                        for (size_t index : visible) currentPaths.push_back(library.mediaItems[index].filePath);
                        if (currentPaths == dragPathSnapshot && visible.size() == library.mediaItems.size()) {
                            auto order = visible;
                            const bool manualOrder = settings.manualMediaOrder;
                            Reorder::Move(order, dragFromIndex, dragToIndex);
                            std::vector<uint64_t> expected;
                            expected.reserve(order.size());
                            for (auto index : order) {
                                const auto& path = library.mediaItems[index].filePath;
                                expected.push_back(Fnv1a64(path.data(), path.size() * sizeof(wchar_t)));
                            }
                            const size_t undoBytes = order.capacity() * sizeof(size_t) + expected.capacity() * sizeof(uint64_t);
                            ArmLibraryUndo(L"Media order", [this, order, expected = std::move(expected), manualOrder]() {
                                if (library.mediaItems.size() != order.size()) return false;
                                for (size_t i = 0; i < order.size(); ++i) {
                                    const auto& path = library.mediaItems[i].filePath;
                                    if (Fnv1a64(path.data(), path.size() * sizeof(wchar_t)) != expected[i]) return false;
                                }
                                std::vector<MediaItem> restored(order.size());
                                for (size_t i = 0; i < order.size(); ++i) restored[order[i]] = std::move(library.mediaItems[i]);
                                library.mediaItems = std::move(restored);
                                library.RebuildPathIndex();
                                settings.manualMediaOrder = manualOrder;
                                SaveSettings();
                                return true;
                            }, undoBytes);
                            std::vector<MediaItem> reordered;
                            reordered.reserve(order.size());
                            for (size_t index : order) reordered.push_back(std::move(library.mediaItems[index]));
                            library.mediaItems = std::move(reordered);
                            library.RebuildPathIndex();
                            settings.manualMediaOrder = true;
                            settings.Save();
                            InvalidateMediaLayoutCache();
                            selectedMediaIndices.clear();
                            selectedMediaIndices.insert(static_cast<size_t>(dragToIndex));
                            selectedMediaIndex = dragToIndex;
                            SaveLibrary();
                        }
                    }
                } else if (activeDragType == DragType::MediaFolder) {
                    const auto& proj = GetCurrentMediaProjection();
                    if (dragFromIndex != dragToIndex && dragFromIndex >= 0 && dragToIndex >= 0 &&
                        dragFromIndex < (int)proj.groups.size() && dragToIndex < (int)proj.groups.size()) {
                        std::vector<std::wstring> currentKeys;
                        for (const auto& g : proj.groups) currentKeys.push_back(g.key);
                        std::wstring movedKey = currentKeys[dragFromIndex];
                        currentKeys.erase(currentKeys.begin() + dragFromIndex);
                        currentKeys.insert(currentKeys.begin() + dragToIndex, movedKey);
                        const auto previousOrder = settings.customFolderOrder;
                        ArmLibraryUndo(L"Folder order", [this, previousOrder, currentKeys]() {
                            if (settings.customFolderOrder != currentKeys) return false;
                            settings.customFolderOrder = previousOrder;
                            m_folderOrderSnapshot.reset();
                            SaveSettings();
                            return true;
                        });
                        settings.customFolderOrder = currentKeys;
                        m_folderOrderSnapshot.reset();
                        settings.Save();
                        InvalidateMediaLayoutCache();
                        SaveLibrary();
                    }
                } else if (activeDragType == DragType::PlaylistRail) {
                    if (dragFromIndex != dragToIndex && dragFromIndex >= 0 && dragToIndex >= 0 && dragFromIndex < (int)library.playlists.size() && dragToIndex < (int)library.playlists.size()) {
                        std::vector<std::string> ids;
                        for (const auto& pl : library.playlists) ids.push_back(pl.id);
                        if (ids == dragRailSnapshot) {
                            const int from = dragFromIndex, to = dragToIndex;
                            const auto id = library.playlists[from].id;
                            ArmLibraryUndo(L"Playlist order", [this, from, to, id]() {
                                if (to >= (int)library.playlists.size() || library.playlists[to].id != id ||
                                    !Reorder::Move(library.playlists, to, from)) return false;
                                library.NotifyPlaylistEdited();
                                return true;
                            });
                            Reorder::Move(library.playlists, dragFromIndex, dragToIndex);
                            selectedPlaylistIndices.clear();
                            selectedPlaylistIndices.insert(static_cast<size_t>(dragToIndex));
                            keyboardFocusedIndex = dragToIndex;
                            library.NotifyPlaylistEdited();
                            SaveLibrary();
                        }
                    }
                    } else if (activeDragType == DragType::PlaylistItem && playlistSearchInput.GetText().empty()) {
                    Playlist* pl = library.FindPlaylist(activePlaylistId);
                    if (pl && !pl->IsSmart() && pl->id == dragPlaylistId && pl->itemPaths == dragPathSnapshot &&
                        dragFromIndex != dragToIndex && dragFromIndex >= 0 && dragToIndex >= 0 &&
                        dragFromIndex < (int)pl->itemPaths.size() && dragToIndex < (int)pl->itemPaths.size()) {
                        const int from = dragFromIndex, to = dragToIndex;
                        const auto id = pl->id;
                        const auto path = pl->itemPaths[from];
                        ArmLibraryUndo(L"Playlist item order", [this, from, to, id, path]() {
                            auto* item = library.FindPlaylist(id);
                            if (!item || to >= (int)item->itemPaths.size() || item->itemPaths[to] != path ||
                                !Reorder::Move(item->itemPaths, to, from)) return false;
                            library.NotifyPlaylistEdited();
                            return true;
                        });
                        Reorder::Move(pl->itemPaths, dragFromIndex, dragToIndex);
                        selectedEpisodeIndices.clear();
                        selectedEpisodeIndices.insert(static_cast<size_t>(dragToIndex));
                        keyboardFocusedIndex = dragToIndex;
                        library.NotifyPlaylistEdited();
                        SaveLibrary();
                    }
                }
                CancelCardDrag();
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (autoscroll.suppressNextClick) { autoscroll.suppressNextClick = false; return 0; }
            if (IsAutoscrolling()) { StopAutoscroll(true); return 0; }
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            RECT clickClientRect{};
            GetClientRect(hWnd, &clickClientRect);
            const float winW = static_cast<float>(clickClientRect.right - clickClientRect.left);
            const float winH = static_cast<float>(clickClientRect.bottom - clickClientRect.top);
            if (playbackSession.IsOpen()) {
                RefreshOverlayHitTargets(x, y);
                if (playbackSession.IsPiP()) {
                    const auto pip = Presentation::PiP(winW, winH, dp);
                    if (PtInRectF(pip.audio, x, y)) {
                        commandDispatcher.Execute(AppCommand::ToggleMute);
                        return 0;
                    }
                    if (isPiPVolumeSliderOpen && PtInRectF(pip.volumeSlider, x, y)) return 0;
                    const bool overPiPControl = PtInRectF(pip.close, x, y) || PtInRectF(pip.restore, x, y) || PtInRectF(pip.pin, x, y) ||
                                                PtInRectF(pip.audio, x, y) || PtInRectF(pip.next, x, y) || PtInRectF(pip.previous, x, y) ||
                                                PtInRectF(pip.playPause, x, y) || PtInRectF(pip.timelineHit, x, y);
                    if (!overPiPControl && isSettingsFlyoutOpen) {
                        float flyoutW = 280.0f * dp;
                        float flyoutH = GetFlyoutHeight();
                        D2D1_RECT_F flyoutBounds = D2D1::RectF( winW - flyoutW - 20.0f * dp, winH - 76.0f * dp - flyoutH - 10.0f * dp,
                            winW - 20.0f * dp, winH - 76.0f * dp - 10.0f * dp);
                        if (PtInRectF(flyoutBounds, x, y)) return 0;
                    }
                    if (!overPiPControl) commandDispatcher.Execute(AppCommand::TogglePiP);
                    return 0;
                }
                float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp, padX = 20.0f * dp, btnSize = 36.0f * dp;
                // Timeline Scrubber double-click
                float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
                float trackCenterY = winH - bottomBarH + 16.0f * dp;
                if (x >= barLeft && x <= barRight && y >= trackCenterY - 12.0f * dp && y <= trackCenterY + 12.0f * dp) {
                    isScrubbing = true;
                    SetCapture(hWnd);
                    double ratio = std::clamp((double)(x - barLeft) / (double)barW, 0.0, 1.0);
                    std::wstring snappedChapter;
                    double seekTarget = TimelineClickTarget(x, barLeft, barW, ratio * player.GetDuration(), snappedChapter, (wParam & MK_CONTROL) != 0);
                    if (!snappedChapter.empty()) ShowOSD(IconType::MediaFilm, snappedChapter);
                    scrubTargetTime = seekTarget;
                    player.Seek(seekTarget, false);
                    lastMpvSeekTime = seekTarget;
                    lastMpvSeekTimestamp = std::chrono::steady_clock::now();
                    return 0;
                }
                D2D1_RECT_F backBtn = D2D1::RectF(24.0f * dp, 16.0f * dp, 24.0f * dp + btnSize, 16.0f * dp + btnSize);
                if (PtInRectF(backBtn, x, y)) { commandDispatcher.Execute(AppCommand::ClosePlayer); return 0; }
                if (hoveredPlayerButtonId == 1) commandDispatcher.Execute(AppCommand::TogglePlay);
                else if (hoveredPlayerButtonId == 2) commandDispatcher.Execute(AppCommand::PlayNext);
                else if (hoveredPlayerButtonId == 3) commandDispatcher.Execute(AppCommand::ToggleMute);
                else if (hoveredPlayerButtonId == 4) commandDispatcher.Execute(AppCommand::ToggleSubtitles);
                else if (hoveredPlayerButtonId == 5) { isSettingsFlyoutOpen = !isSettingsFlyoutOpen; currentFlyoutMenu = "main"; }
                else if (hoveredPlayerButtonId == 6) commandDispatcher.Execute(AppCommand::TogglePiP);
                else if (hoveredPlayerButtonId == 7) commandDispatcher.Execute(AppCommand::ToggleFullscreen);
                else if (hoveredPlayerButtonId == 8) {
                    if (playbackSession.GetQueueIndex() > 0 || settings.loop) commandDispatcher.Execute(AppCommand::PlayPrevious);
                } else if (y > topBarH && y < winH - bottomBarH) {
                    commandDispatcher.Execute(AppCommand::TogglePlay);
                    playPauseAnimTimer = 0.0f;
                    commandDispatcher.Execute(AppCommand::ToggleFullscreen);
                }
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            } else {
                if (listenPlaybackActive && PtInRectF(listenIslandHitRect, x, y)) {
                    if (PtInRectF(listenIslandBadgeBounds, x, y)) {
                        ToggleListenPause();
                    } else if (PtInRectF(listenIslandMuteBounds, x, y)) {
                        settings.muted = !settings.muted;
                        const double eff = settings.muted ? 0.0 : std::clamp(settings.volume, 0.0, 1.0);
                        listenVolumeApplied = eff;
                        listenPlayer.SetVolume(eff);
                        SaveSettings();
                        InvalidateRect(hWnd, NULL, FALSE);
                    } else if (!PtInRectF(listenIslandVolumeBounds, x, y) && !PtInRectF(listenIslandXBounds, x, y) &&
                               !(listenIslandExpand.current > 0.25f && !listenPlaybackIsRadio && PtInRectF(listenIslandSeekBounds, x, y))) {
                        listenPinExpanded = !listenPinExpanded;
                        InvalidateRect(hWnd, NULL, FALSE);
                    }
                    return 0;
                }
                RefreshHubHitTargets(x, y);
                if (currentTab == TabView::Media) {
                    const auto& visibleIndices = GetVisibleMediaIndices();
                    if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
                        if (!isPinnedCloseHovered) {
                            size_t origIdx = visibleIndices[hoveredMediaIndex];
                            if (origIdx < library.mediaItems.size()) { OpenAndPlayMedia(library.mediaItems[origIdx].filePath); return 0; }
                        }
                    }
                } else if (currentTab == TabView::Playlists) {
                    if (hoveredPlaylistDetailItemIndex >= 0) {
                        PlayPlaylist(activePlaylistId, (size_t)hoveredPlaylistDetailItemIndex);
                        return 0;
                    } else if (hoveredPlaylistRailIndex >= 0) { PlayPlaylist(library.playlists[hoveredPlaylistRailIndex].id, 0); return 0; }
                } else if (currentTab == TabView::History) {
                    if (hoveredHistoryIndex >= 0 && hoveredHistoryIndex < (int)library.history.size()) {
                        OpenAndPlayMedia(library.history[hoveredHistoryIndex].filePath, library.history[hoveredHistoryIndex].lastPosSec);
                        return 0;
                    }
                }
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (activeDragType != DragType::None || pendingDragType != DragType::None) { CancelCardDrag(); return 0; }
            float x = (float)GET_X_LPARAM(lParam);
            float y = (float)GET_Y_LPARAM(lParam);
            if (IsAutoscrolling()) { StopAutoscroll(true); autoscroll.suppressNextClick = true; return 0; }
            if (isContextMenuOpen) { RepositionContextMenu(x, y); return 0; }
            if (playbackSession.IsOpen()) { RefreshOverlayHitTargets(x, y); OpenPlayerContextMenu(x, y); } else {
                RefreshHubHitTargets(x, y);
                if (currentTab == TabView::Media) {
                    if (libraryGroupMode == LibraryGroupMode::Listen) {
                        bool rowHandled = false;
                        for (size_t i = 0; i < listenEpisodeBounds.size() && !rowHandled; ++i) {
                            if (PtInRectF(listenEpisodeBounds[i], x, y)) {
                                const int feedIdx = (i < listenEpisodeFeedIndex.size()) ? listenEpisodeFeedIndex[i] : -1;
                                if (feedIdx >= 0 && (size_t)feedIdx < podcastSubscriptions.GetFeeds().size()) {
                                    OpenListenFeedContextMenu((size_t)feedIdx, x, y);
                                    rowHandled = true;
                                }
                            }
                        }
                        for (size_t i = 0; i < listenFeedBounds.size() && !rowHandled; ++i) {
                            if (PtInRectF(listenFeedBounds[i], x, y) && !podcastSubscriptions.GetFeeds().empty()) {
                                OpenListenFeedContextMenu(i, x, y);
                                rowHandled = true;
                            }
                        }
                        for (size_t i = 0; i < listenStationBounds.size() && !rowHandled; ++i) {
                            if (PtInRectF(listenStationBounds[i], x, y) && i < listenStations.size()) {
                                OpenListenStationContextMenu(i, x, y);
                                rowHandled = true;
                            }
                        }
                        if (!rowHandled) OpenBackgroundContextMenu(x, y);
                    } else if (hoveredMediaGroupHeader >= 0) { OpenFolderContextMenu(hoveredMediaGroupHeader, x, y); } else {
                        const auto& visibleIndices = GetVisibleMediaIndices();
                        if (hoveredMediaIndex >= 0 && hoveredMediaIndex < (int)visibleIndices.size()) {
                            size_t origIdx = visibleIndices[hoveredMediaIndex];
                            if (origIdx < library.mediaItems.size()) {
                                if (selectedMediaIndices.count(origIdx) == 0)
                                    HandleItemSelection(FocusPane::MediaList, (size_t)hoveredMediaIndex, false, false, visibleIndices.size());
                                OpenMediaContextMenu(library.mediaItems[origIdx], x, y);
                            }
                        } else { OpenBackgroundContextMenu(x, y); }
                    }
} else if (currentTab == TabView::Playlists) {
                    Playlist* pl = library.FindPlaylist(activePlaylistId);
                    if (hoveredPlaylistDetailItemIndex >= 0 && pl) {
                        if (selectedEpisodeIndices.count((size_t)hoveredPlaylistDetailItemIndex) == 0)
                            HandleItemSelection(FocusPane::PlaylistDetail, (size_t)hoveredPlaylistDetailItemIndex, false, false, pl->itemPaths.size());
                        OpenPlaylistItemContextMenu(hoveredPlaylistDetailItemIndex, x, y);
                    } else if (hoveredPlaylistRailIndex >= 0 && hoveredPlaylistRailIndex < (int)library.playlists.size()) {
                        if (selectedPlaylistIndices.count((size_t)hoveredPlaylistRailIndex) == 0)
                            HandleItemSelection(FocusPane::PlaylistRail, (size_t)hoveredPlaylistRailIndex, false, false, library.playlists.size());
                        OpenPlaylistContextMenu(library.playlists[hoveredPlaylistRailIndex], x, y);
                    } else {
                        // Right-clicking the empty rail creates a playlist, matching the "+" affordance that used to live here.
                        float dp2 = std::max(0.5f, renderer.GetDpiScale());
                        float pad2 = 18.0f * dp2, padY2 = 14.0f * dp2;
                        RECT rc2;
                        GetClientRect(hWnd, &rc2);
                        float viewW2 = (float)(rc2.right - rc2.left);
                        float railW2 = std::min(260.0f * dp2, std::max(120.0f * dp2, (viewW2 - pad2 * 2.0f) * 0.42f));
                        if (x >= pad2 && x <= pad2 + railW2 && y >= padY2 && y <= (float)(rc2.bottom - rc2.top) - padY2) {
                            OpenRailBackgroundContextMenu(x, y);
                        } else if (pl) { OpenPlaylistContextMenu(*pl, x, y); } else { OpenBackgroundContextMenu(x, y); }
                    }
                } else if (currentTab == TabView::History) {
                    if (hoveredHistoryIndex >= 0 && hoveredHistoryIndex < (int)library.history.size()) {
                        if (selectedHistoryIndices.count((size_t)hoveredHistoryIndex) == 0)
                            HandleItemSelection(FocusPane::HistoryList, (size_t)hoveredHistoryIndex, false, false, library.history.size());
                        OpenHistoryContextMenu(library.history[hoveredHistoryIndex], x, y);
                    } else { OpenBackgroundContextMenu(x, y); }
                } else { OpenBackgroundContextMenu(x, y); }
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            if (autoscroll.suppressNextClick) { autoscroll.suppressNextClick = false; return 0; }
            break;
        }
        case WM_XBUTTONDOWN: {
            // Active shortcut capture: an X button completes the binding.
            if (HandleShortcutCaptureMouse((HIWORD(wParam) == XBUTTON1) ? kMouseX1 : kMouseX2)) return TRUE;
            if (IsAutoscrolling()) { StopAutoscroll(true); autoscroll.suppressNextClick = true; return TRUE; }
            break;
        }
        case WM_XBUTTONUP: {
            if (!rebindingActionId.empty()) return TRUE;
            if (autoscroll.suppressNextClick) { autoscroll.suppressNextClick = false; return TRUE; }
            break;
        }
        case WM_CLOSE: { BeginClose(); return 0; }
        case WM_DESTROY: { PostQuitMessage(0); return 0; }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT App::HandleOverlayMessage(HWND hOverlay, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (HandlePlaybackSpeedDragMessage(uMsg, lParam)) return 0;
    float dp = overlayRenderer.GetDpiScale();
    float x = (float)GET_X_LPARAM(lParam);
    float y = (float)GET_Y_LPARAM(lParam);
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    float topBarH = 64.0f * dp, bottomBarH = 76.0f * dp, padX = Presentation::PlayerPadding * dp, btnSize = 36.0f * dp;
    // Shortcut capture over the player/modal layer: while a capture is active,
    // middle/X/wheel inputs complete it instead of their normal behavior.
    // Without an active capture these messages keep their ordinary routing.
    if (!rebindingActionId.empty() &&
        (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP || uMsg == WM_XBUTTONDOWN || uMsg == WM_XBUTTONUP || uMsg == WM_MOUSEWHEEL)) {
        if (uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN) {
            const UINT captureVk = (uMsg == WM_MBUTTONDOWN) ? kMouseMiddle
                : (HIWORD(wParam) == XBUTTON1 ? kMouseX1 : kMouseX2);
            HandleShortcutCaptureMouse(captureVk);
        } else if (uMsg == WM_MOUSEWHEEL) {
            HandleShortcutCaptureMouse(GET_WHEEL_DELTA_WPARAM(wParam) >= 0 ? kMouseWheelUp : kMouseWheelDown);
        }
        return 0;
    }
    if (activeModal != ModalType::None || activeTextInputField == TextInputField::PlaybackSpeed) {
        if (uMsg == WM_MOUSEMOVE || uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK)
            return SendMessageW(hWnd, uMsg, wParam, lParam);
        if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_MOUSEWHEEL) return 0;
    }
    const bool relayTooltipMouse = uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSELEAVE ||
        uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_LBUTTONDBLCLK ||
        uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP || uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP;
    if (relayTooltipMouse) { playerTooltips.HideForKeyboard(); playerTooltips.RelayMouseMessage(uMsg, wParam, lParam); }
    switch (uMsg) {
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                isOverlayMouseTracking = false;
                isVideoMouseTracking = false;
                lastPlayerMouseX = -1.0f;
                lastPlayerMouseY = -1.0f;
            } else {
                lastPlayerMouseX = -1.0f;
                lastPlayerMouseY = -1.0f;
                controlsIdleTimer = 0.0f;
                controlsOpacity = 1.0f;
                SyncOverlayPosition();
                if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
                Render();
            }
            return 0;
        }
        case WM_KILLFOCUS:
            isOverlayMouseTracking = false;
            isVideoMouseTracking = false;
            lastPlayerMouseX = -1.0f;
            lastPlayerMouseY = -1.0f;
            return 0;
        case WM_SETFOCUS:
            lastPlayerMouseX = -1.0f;
            lastPlayerMouseY = -1.0f;
            controlsIdleTimer = 0.0f;
            if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
            Render();
            return 0;
        case WM_MOUSELEAVE: {
            if (hOverlay == hVideoWnd) isVideoMouseTracking = false;
            else isOverlayMouseTracking = false;
            if (!isOverlayMouseTracking && !isVideoMouseTracking) { OnMouseLeave(); Render(); }
            return 0;
        }
        case WM_SETCURSOR: {
            if (IsPlaybackDiagnosticsCloseHit(lastPlayerMouseX, lastPlayerMouseY)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
            if (IsLogViewerCloseHit(lastPlayerMouseX, lastPlayerMouseY)) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
            if (playbackSession.IsOpen() && controlsOpacity <= 0.02f && !isSettingsFlyoutOpen && !isContextMenuOpen && !isScrubbing && !isVolumeScrubbing && !isJumpAheadVisible) {
                SetCursor(NULL);
                return TRUE;
            }
            if (hoveredPlayerButtonId > 0 || isBackHovered || isScrubbing || isVolumeScrubbing || isVolumeHovered || isTimelineHovered || isJumpAheadHovered) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
            } else { SetCursor(LoadCursor(NULL, IDC_ARROW)); }
            return TRUE;
        }
        case WM_MOUSEMOVE: {
            bool& trackingRef = (hOverlay == hVideoWnd) ? isVideoMouseTracking : isOverlayMouseTracking;
            if (!trackingRef) {
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hOverlay, 0 };
                TrackMouseEvent(&tme);
                trackingRef = true;
            }
            // Check if mouse actually moved position to avoid synthetic WM_MOUSEMOVE resetting idle timer
            bool mouseMoved = (lastPlayerMouseX < 0.0f || std::abs(x - lastPlayerMouseX) >= 1.0f || std::abs(y - lastPlayerMouseY) >= 1.0f);
            if (mouseMoved) {
                lastPlayerMouseX = x;
                lastPlayerMouseY = y;
                controlsIdleTimer = 0.0f;
                controlsOpacity = 1.0f;
                isKeyboardNavActive = false;
                if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
            }
            if (playbackSession.IsPiP()) {
                if (isPiPPotentialWindowDrag || isPiPDraggingWindow) {
                    POINT curPt;
                    GetCursorPos(&curPt);
                    int dx = curPt.x - pipWindowDragStartPoint.x;
                    int dy = curPt.y - pipWindowDragStartPoint.y;
                    if (!isPiPDraggingWindow && (std::abs(dx) > 4 || std::abs(dy) > 4)) {
                        isPiPDraggingWindow = true;
                        isPiPDragOccurred = true;
                    }
                    if (isPiPDraggingWindow) {
                        isPiPDragOccurred = true;
                        int newX = pipWindowDragStartWindowPos.x + dx;
                        int newY = pipWindowDragStartWindowPos.y + dy;
                        SetWindowPos(hWnd, NULL, newX, newY, 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        // Drag coordinates are the top-level window's outer
                        // bounds. Recompute the overlay from the owner's
                        // client origin so the PiP frame border never
                        // becomes a visible control offset.
                        SyncOverlayPosition();
                        return 0;
                    }
                }
                const auto pip = Presentation::PiP(winW, winH, dp);
                RefreshOverlayHitTargets(x, y);
                // Track scrubbing
                if (isScrubbing && player.GetDuration() > 0.0) {
                    double ratio = std::clamp((double)(x - pip.timeline.left) / (double)std::max(1.0f, RectWidth(pip.timeline)), 0.0, 1.0);
                    double scrubTime = ratio * player.GetDuration();
                    scrubTargetTime = scrubTime;
                    auto now = std::chrono::steady_clock::now();
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMpvSeekTimestamp).count();
                    if (elapsedMs >= 30) { lastMpvSeekTimestamp = now; lastMpvSeekTime = scrubTime; player.Seek(scrubTime, false); }
                }
                if (isPiPVolumeDragging) {
                    float trackH = pip.volumeTrack.bottom - pip.volumeTrack.top;
                    if (trackH > 1.0f) {
                        float ratio = (pip.volumeTrack.bottom - y) / trackH;
                        double newVol = std::clamp((double)ratio, 0.0, 1.0) * kMaxVolumeRatio;
                        commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ newVol });
                    }
                }
                Render();
                return 0;
            }
            // If controls are hidden and not scrubbing/flying out, suppress hover calculations
            if (controlsOpacity <= 0.02f && controlsIdleTimer >= (player.IsPlaying() ? 1.2f : 2.0f) && !isScrubbing && !isVolumeScrubbing && !isSettingsFlyoutOpen && !isContextMenuOpen && !isJumpAheadVisible && !isVideoPanDragging) {
                hoveredPlayerButtonId = 0;
                isBackHovered = false;
                isVolumeHovered = false;
                isTimelineHovered = false;
                hoveredFlyoutOption = -1;
                hoveredContextMenuItem = -1;
                isJumpAheadHovered = false;
                Render();
                return 0;
            }
            // Check scrubbing (timeline track drag)
            float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
            if (isScrubbing) {
                ScrubTimelineToPx(x, winW, dp);
            }
            // Check volume scrubbing
            float controlsY = winH - bottomBarH + Presentation::PlayerControlsY * dp;
            const auto playerLayoutForVolume = Presentation::Player(winW, winH, dp, playbackSession.IsQueuePlayback());
            float volTrackX = playerLayoutForVolume.volume.left, volW = 60.0f * dp;
            if (isVolumeScrubbing) {
                double vol = std::clamp((double)(x - volTrackX) / (double)volW, 0.0, 1.0) * kMaxVolumeRatio;
                player.SetVolume(vol);
                settings.volume = vol;
                settings.muted = false;
                if (vol > 0.01) playbackSession.SetPriorUnmutedVolume(vol);
                int pct = (int)std::round(vol * 100.0);
                ShowOSD(vol <= 0.01 ? IconType::VolumeMuted : (vol > 0.5 ? IconType::VolumeHigh : IconType::VolumeLow), std::to_wstring(pct) + L"%");
            }
            // Ctrl+drag pan: grabbing the image moves it with the cursor.
            // mpv's video-pan-x is positive to the right and video-pan-y is
            // positive upward, so vertical deltas are inverted — dragging
            // right moves the image right, dragging down moves it down.
            if (isVideoPanDragging && playbackSession.IsOpen() && !playbackSession.IsPiP()) {
                const float panW = std::max(1.0f, winW), panH = std::max(1.0f, winH);
                player.NudgeVideoPan((double)(x - panDragLastX) / panW, -(double)(y - panDragLastY) / panH);
                panDragLastX = x;
                panDragLastY = y;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            // Re-evaluate all overlay hit targets (backBtn, timeline track, buttons, volume, flyouts, menus)
            RefreshOverlayHitTargets(x, y);
            Render();
            return 0;
        }
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP: {
            if (playbackSession.IsOpen() && HandlePlayerTouchPointer(hOverlay, uMsg, wParam, lParam)) return 0;
            UINT mouseMessage = 0;
            WPARAM mouseWParam = 0;
            LPARAM mouseLParam = 0;
            if (TranslatePointerMessage(hOverlay, uMsg, wParam, mouseMessage, mouseWParam, mouseLParam))
                return HandleOverlayMessage(hOverlay, mouseMessage, mouseWParam, mouseLParam);
            return 0;
        }
        case WM_POINTERCAPTURECHANGED:
            // The system took the pointer away (palm rejection); end cleanly.
            CancelTouchGestures();
            Render();
            return 0;
        case WM_LBUTTONDOWN: {
            isKeyboardNavActive = false;
            RefreshOverlayHitTargets(x, y);
            if (IsPlaybackDiagnosticsCloseHit(x, y)) { TogglePlaybackDiagnostics(); Render(); return 0; }
            if (IsLogViewerCloseHit(x, y)) { ToggleLogViewer(); Render(); return 0; }
            controlsIdleTimer = 0.0f;
            lastPlayerMouseX = x;
            lastPlayerMouseY = y;
            if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
            if (playbackSession.IsPiP()) {
                const auto pip = Presentation::PiP(winW, winH, dp);
                isKeyboardNavActive = false;
                playerKeyboardFocusId = 0;
                if (isContextMenuOpen) {
                    HandleContextMenuClick(x, y, dp);
                    Render();
                    return 0;
                }
                if (isSettingsFlyoutOpen) {
                    float flyoutW = 280.0f * dp;
                    float flyoutH = GetFlyoutHeight();
                    D2D1_RECT_F flyoutBounds = D2D1::RectF( winW - flyoutW - 20.0f * dp, winH - bottomBarH - flyoutH - 10.0f * dp,
                        winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
                    if (PtInRectF(flyoutBounds, x, y)) {
                        HandleFlyoutClick(x - flyoutBounds.left, y - flyoutBounds.top);
                    } else { CloseSettingsFlyout(); }
                    Render();
                    return 0;
                }
                if (isPiPVolumeSliderOpen) {
                    if (PtInRectF(pip.volumeSlider, x, y)) {
                        isPiPVolumeDragging = true;
                        SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                        float trackH = pip.volumeTrack.bottom - pip.volumeTrack.top;
                        double newVol = std::clamp((double)(pip.volumeTrack.bottom - y) / (double)std::max(1.0f, trackH), 0.0, 1.0) * kMaxVolumeRatio;
                        commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ newVol });
                        Render();
                        return 0;
                    }
                    if (!PtInRectF(pip.audio, x, y)) {
                        isPiPVolumeSliderOpen = false;
                        AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                        Render();
                        const bool overOtherPiPControl = PtInRectF(pip.restore, x, y) || PtInRectF(pip.pin, x, y) ||
                            PtInRectF(pip.close, x, y) || PtInRectF(pip.next, x, y) || PtInRectF(pip.previous, x, y) ||
                            PtInRectF(pip.playPause, x, y) || PtInRectF(pip.timelineHit, x, y);
                        if (!overOtherPiPControl) return 0;
                    }
                }
                if (PtInRectF(pip.restore, x, y)) { commandDispatcher.Execute(AppCommand::TogglePiP); return 0; }
                if (PtInRectF(pip.close, x, y)) { SendMessage(hWnd, WM_CLOSE, 0, 0); return 0; }
                if (PtInRectF(pip.pin, x, y)) {
                    playbackSession.TogglePiPPinned(hWnd);
                    SyncOverlayPosition();
                    MarkAccessibilityDirty();
                    RefreshAccessibilityTree(true);
                    Render();
                    return 0;
                }
                if (PtInRectF(pip.audio, x, y)) {
                    isPiPVolumeSliderOpen = !isPiPVolumeSliderOpen;
                    if (isPiPVolumeSliderOpen) {
                        pipVolumeAnim.SnapTo(0.0f);
                        AnimateVisual(pipVolumeAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                    } else {
                        AnimateVisual(pipVolumeAnim, 0.0f, MotionTokens::kToggle, MotionTokens::Spring());
                    }
                    Render();
                    return 0;
                }
                if (PtInRectF(pip.next, x, y)) {
                    if (playbackSession.IsQueuePlayback() && playbackSession.GetQueueSize() > 1)
                        commandDispatcher.Execute(AppCommand::PlayNext);
                    Render();
                    return 0;
                }
                if (PtInRectF(pip.previous, x, y)) {
                    if (playbackSession.IsQueuePlayback() && (playbackSession.GetQueueIndex() > 0 || settings.loop))
                        commandDispatcher.Execute(AppCommand::PlayPrevious);
                    Render();
                    return 0;
                }
                if (PtInRectF(pip.playPause, x, y)) { commandDispatcher.Execute(AppCommand::TogglePlay); Render(); return 0; }
                if (PtInRectF(pip.timelineHit, x, y) && player.GetDuration() > 0.0) {
                    isScrubbing = true;
                    SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                    double ratio = std::clamp((double)(x - pip.timeline.left) / (double)std::max(1.0f, RectWidth(pip.timeline)), 0.0, 1.0);
                    double seekTarget = ratio * player.GetDuration();
                    scrubTargetTime = seekTarget;
                    commandDispatcher.Execute(AppCommand::SeekTo, CommandArgs{ seekTarget });
                    AnnounceStatus(L"Seek to " + FormatDuration(seekTarget));
                    Render();
                    return 0;
                }
                GetCursorPos(&pipWindowDragStartPoint);
                RECT winRect;
                GetWindowRect(hWnd, &winRect);
                pipWindowDragStartWindowPos = { winRect.left, winRect.top };
                isPiPPotentialWindowDrag = true;
                isPiPDraggingWindow = false;
                isPiPDragOccurred = false;
                SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                return 0;
            }
            // Back button (single click). Geometry matches DrawPlayerTopBar: padX 24dp, padY 16dp, 36dp square. The overlay consumes the
            // click, so the main window's handler never sees it — previously only the double-click path closed the player.
            {
                float topBtnSize = 36.0f * dp;
                D2D1_RECT_F backRect = D2D1::RectF(24.0f * dp, 16.0f * dp, 24.0f * dp + topBtnSize, 16.0f * dp + topBtnSize);
                if (PtInRectF(backRect, x, y)) { commandDispatcher.Execute(AppCommand::ClosePlayer); Render(); return 0; }
            }
            if (isContextMenuOpen) {
                HandleContextMenuClick(x, y, dp);
                Render();
                return 0;
            }
            // Timeline track click
            float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
            float trackCenterY = winH - bottomBarH + 16.0f * dp;
            if (x >= barLeft && x <= barRight && y >= trackCenterY - 12.0f * dp && y <= trackCenterY + 12.0f * dp) {
                isScrubbing = true;
                SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                double ratio = std::clamp((double)(x - barLeft) / (double)barW, 0.0, 1.0);
                double seekTarget = ratio * player.GetDuration();
                scrubTargetTime = seekTarget;
                player.Seek(seekTarget, false);
                lastMpvSeekTime = seekTarget;
                lastMpvSeekTimestamp = std::chrono::steady_clock::now();
                Render();
                return 0;
            }
            // Volume slider click
            float controlsY = winH - bottomBarH + Presentation::PlayerControlsY * dp;
            const auto playerLayoutForVolume = Presentation::Player(winW, winH, dp, playbackSession.IsQueuePlayback());
            float volTrackX = playerLayoutForVolume.volume.left, volW = 60.0f * dp;
            if (winW >= 720.0f * dp && x >= volTrackX - 10.0f * dp && x <= volTrackX + volW + 10.0f * dp &&
                y >= controlsY - 8.0f * dp && y <= controlsY + btnSize + 8.0f * dp) {
                isVolumeScrubbing = true;
                SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                double vol = std::clamp((double)(x - volTrackX) / (double)volW, 0.0, 1.0) * kMaxVolumeRatio;
                commandDispatcher.Execute(AppCommand::SetVolume, CommandArgs{ vol });
                Render();
                return 0;
            }
            // Settings Flyout click
            if (isSettingsFlyoutOpen) {
                float flyoutW = 280.0f * dp;
                float flyoutH = GetFlyoutHeight();
                D2D1_RECT_F flyoutBounds = D2D1::RectF(winW - flyoutW - 20.0f * dp, winH - bottomBarH - flyoutH - 10.0f * dp, winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
                if (PtInRectF(flyoutBounds, x, y)) {
                    HandleFlyoutClick(x - flyoutBounds.left, y - flyoutBounds.top);
                    Render();
                    return 0;
                } else { CloseSettingsFlyout(); Render(); return 0; }
            }
            // Jump ahead button click
            if (isJumpAheadHovered && jumpAheadAnim.current > 0.1f) {
                isJumpAheadPressed = true;
                SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                Render();
                return 0;
            }
            // Bottom Buttons
            if (hoveredPlayerButtonId == 1) { commandDispatcher.Execute(AppCommand::TogglePlay); }
            else if (hoveredPlayerButtonId == 2) { commandDispatcher.Execute(AppCommand::PlayNext); }
            else if (hoveredPlayerButtonId == 3) { commandDispatcher.Execute(AppCommand::ToggleMute); }
            else if (hoveredPlayerButtonId == 4) { commandDispatcher.Execute(AppCommand::ToggleSubtitles); }
            else if (hoveredPlayerButtonId == 5) {
                if (isSettingsFlyoutOpen) { CloseSettingsFlyout(); } else {
                    PushFocusSnapshot(TransientSurfaceType::SettingsFlyout);
                    isSettingsFlyoutOpen = true;
                    currentFlyoutMenu = "main";
                    flyoutClosing = false;
                    flyoutAnim.SnapTo(0.0f);
                    AnimateVisual(flyoutAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                }
            }
            else if (hoveredPlayerButtonId == 6) { commandDispatcher.Execute(AppCommand::TogglePiP); }
            else if (hoveredPlayerButtonId == 7) { commandDispatcher.Execute(AppCommand::ToggleFullscreen); }
            else if (hoveredPlayerButtonId == 8) {
                if (playbackSession.GetQueueIndex() > 0 || settings.loop) commandDispatcher.Execute(AppCommand::PlayPrevious);
            } else if ((wParam & MK_CONTROL) != 0 && playbackSession.IsOpen() && y > topBarH && y < winH - bottomBarH) {
                // Ctrl+left-drag on the video area pans the image (grab
                // semantics). Every bar/button/flyout hit target has already
                // declined above, so the drag never fights timeline scrubbing,
                // volume scrubbing, or the control buttons; PiP returns
                // earlier and keeps its window-drag gesture.
                isVideoPanDragging = true;
                panDragLastX = x;
                panDragLastY = y;
                SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
            } else {
                // Click on the central video area toggles playback immediately.
                if (y > topBarH && y < winH - bottomBarH) { commandDispatcher.Execute(AppCommand::TogglePlay); }
            }
            Render();
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                isPiPPotentialWindowDrag = false;
                isPiPDraggingWindow = false;
            }
            if (isJumpAheadPressed) {
                isJumpAheadPressed = false;
                Render();
            }
            if (isScrubbing || isVolumeScrubbing || isPiPVolumeDragging) {
                isScrubbing = false;
                scrubTargetTime = -1.0;
                isVolumeScrubbing = false;
                isPiPVolumeDragging = false;
                isTimelineHovered = false;
                playerTouch.seekScrub = false;
                playerTouch.volumeDrag = false;
                Render();
            }
            isVideoPanDragging = false;
            return 0;
        }
        case WM_LBUTTONUP: {
            if (isPiPPotentialWindowDrag || isPiPDraggingWindow || isPiPDragOccurred) {
                const bool wasDragging = isPiPDraggingWindow || isPiPDragOccurred;
                isPiPPotentialWindowDrag = false;
                isPiPDraggingWindow = false;
                isPiPDragOccurred = false;
                if (GetCapture() == (hOverlayWnd ? hOverlayWnd : hOverlay) || GetCapture() == hWnd) {
                    ReleaseCapture();
                }
                SyncOverlayPosition();
                if (!wasDragging) {
                    commandDispatcher.Execute(AppCommand::TogglePlay);
                    Render();
                } else {
                    PersistPiPGeometry();
                }
                RefreshOverlayHitTargets((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                return 0;
            }
            if (isJumpAheadPressed) {
                isJumpAheadPressed = false;
                ReleaseCapture();
                if (isJumpAheadHovered) {
                    if (jumpAheadTargetTime > 0.0) {
                        player.Seek(jumpAheadTargetTime, true);
                        ShowOSD(IconType::Play, L"Jumped ahead");
                        AnnounceStatus(L"Jumped ahead to " + FormatDuration(jumpAheadTargetTime), true);
                    }
                    isJumpAheadVisible = false;
                    jumpAheadTimer = 0.0f;
                    AnimateVisual(jumpAheadAnim, 0.0f, MotionTokens::kFast, MotionTokens::Standard());
                }
                RefreshOverlayHitTargets((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                Render();
                return 0;
            }
            if (isScrubbing || isVolumeScrubbing || isPiPVolumeDragging) {
                if (isScrubbing && player.GetDuration() > 0.0) {
                    if (scrubTargetTime >= 0.0) { player.Seek(scrubTargetTime, true); }
                    AnnounceStatus(L"Seek to " + FormatDuration(player.GetCurrentTime()), true);
                }
                isScrubbing = false;
                scrubTargetTime = -1.0;
                isVolumeScrubbing = false;
                isPiPVolumeDragging = false;
                ReleaseCapture();
            }
            if (isVideoPanDragging) {
                isVideoPanDragging = false;
                ReleaseCapture();
            }
            RefreshOverlayHitTargets((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
            Render();
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            RefreshOverlayHitTargets(x, y);
            if (isJumpAheadHovered) return 0;
            if (isContextMenuOpen) {
                HandleContextMenuClick(x, y, dp);
                Render();
                return 0;
            }
            if (playbackSession.IsPiP()) {
                const auto pip = Presentation::PiP(winW, winH, dp);
                if (PtInRectF(pip.audio, x, y)) {
                    commandDispatcher.Execute(AppCommand::ToggleMute);
                    Render();
                    return 0;
                }
                if (isPiPVolumeSliderOpen && PtInRectF(pip.volumeSlider, x, y)) return 0;
                const bool overPiPControl = PtInRectF(pip.restore, x, y) || PtInRectF(pip.close, x, y) ||
                    PtInRectF(pip.pin, x, y) || PtInRectF(pip.audio, x, y) || PtInRectF(pip.next, x, y) || PtInRectF(pip.previous, x, y) ||
                    PtInRectF(pip.playPause, x, y) || PtInRectF(pip.timelineHit, x, y);
                if (!overPiPControl && isSettingsFlyoutOpen) {
                    float flyoutW = 280.0f * dp;
                    float flyoutH = GetFlyoutHeight();
                    D2D1_RECT_F flyoutBounds = D2D1::RectF( winW - flyoutW - 20.0f * dp, winH - bottomBarH - flyoutH - 10.0f * dp,
                        winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
                    if (PtInRectF(flyoutBounds, x, y)) return 0;
                }
                if (!overPiPControl) commandDispatcher.Execute(AppCommand::TogglePiP);
                return 0;
            }
            // Timeline track double-click: seek immediately and engage scrubbing
            {
                float barLeft = 20.0f * dp, barRight = winW - 20.0f * dp, barW = barRight - barLeft;
                float trackCenterY = winH - bottomBarH + 16.0f * dp;
                if (x >= barLeft && x <= barRight && y >= trackCenterY - 12.0f * dp && y <= trackCenterY + 12.0f * dp) {
                    isScrubbing = true;
                    SetCapture(hOverlayWnd ? hOverlayWnd : hOverlay);
                    double ratio = std::clamp((double)(x - barLeft) / (double)barW, 0.0, 1.0);
                    std::wstring snappedChapter;
                    double seekTarget = TimelineClickTarget(x, barLeft, barW, ratio * player.GetDuration(), snappedChapter, (wParam & MK_CONTROL) != 0);
                    if (!snappedChapter.empty()) ShowOSD(IconType::MediaFilm, snappedChapter);
                    scrubTargetTime = seekTarget;
                    player.Seek(seekTarget);
                    lastMpvSeekTime = seekTarget;
                    lastMpvSeekTimestamp = std::chrono::steady_clock::now();
                    AnnounceStatus(L"Seek to " + FormatDuration(seekTarget));
                    Render();
                    return 0;
                }
            }
            // Back button
            D2D1_RECT_F backBtn = D2D1::RectF(24.0f * dp, 16.0f * dp, 24.0f * dp + btnSize, 16.0f * dp + btnSize);
            if (PtInRectF(backBtn, x, y)) { commandDispatcher.Execute(AppCommand::ClosePlayer); return 0; }
            // Settings Flyout click
            if (isSettingsFlyoutOpen) {
                float flyoutW = 280.0f * dp;
                float flyoutH = GetFlyoutHeight();
                D2D1_RECT_F flyoutBounds = D2D1::RectF(winW - flyoutW - 20.0f * dp, winH - bottomBarH - flyoutH - 10.0f * dp, winW - 20.0f * dp, winH - bottomBarH - 10.0f * dp);
                if (PtInRectF(flyoutBounds, x, y)) {
                    HandleFlyoutClick(x - flyoutBounds.left, y - flyoutBounds.top);
                    Render();
                    return 0;
                } else { CloseSettingsFlyout(); Render(); return 0; }
            }
            // Bottom Buttons
            if (hoveredPlayerButtonId == 1) { commandDispatcher.Execute(AppCommand::TogglePlay); }
            else if (hoveredPlayerButtonId == 2) { commandDispatcher.Execute(AppCommand::PlayNext); }
            else if (hoveredPlayerButtonId == 3) { commandDispatcher.Execute(AppCommand::ToggleMute); }
            else if (hoveredPlayerButtonId == 4) { commandDispatcher.Execute(AppCommand::ToggleSubtitles); }
            else if (hoveredPlayerButtonId == 5) {
                if (isSettingsFlyoutOpen) { CloseSettingsFlyout(); } else {
                    PushFocusSnapshot(TransientSurfaceType::SettingsFlyout);
                    isSettingsFlyoutOpen = true;
                    currentFlyoutMenu = "main";
                    flyoutClosing = false;
                    flyoutAnim.SnapTo(0.0f);
                    AnimateVisual(flyoutAnim, 1.0f, MotionTokens::kToggle, MotionTokens::Spring());
                }
            }
            else if (hoveredPlayerButtonId == 6) { commandDispatcher.Execute(AppCommand::TogglePiP); }
            else if (hoveredPlayerButtonId == 7) { commandDispatcher.Execute(AppCommand::ToggleFullscreen); }
            else if (hoveredPlayerButtonId == 8) {
                if (playbackSession.GetQueueIndex() > 0 || settings.loop) commandDispatcher.Execute(AppCommand::PlayPrevious);
            } else if (y > topBarH && y < winH - bottomBarH) {
                commandDispatcher.Execute(AppCommand::TogglePlay);
                playPauseAnimTimer = 0.0f;
                commandDispatcher.Execute(AppCommand::ToggleFullscreen);
            }
            Render();
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (isContextMenuOpen) RepositionContextMenu(x, y);
            else { RefreshOverlayHitTargets(x, y); OpenPlayerContextMenu(x, y); }
            Render();
            return 0;
        }
        case WM_MBUTTONDOWN: {
            // A bound middle-click action overrides the default no-op in the
            // player context only (CommandDispatcher::FindMouseBinding).
            if (TryExecutePlayerMouseBinding(kMouseMiddle)) { Render(); return 0; }
            break;
        }
        case WM_XBUTTONDOWN: {
            // A bound X-button action overrides the default behavior in the
            // player context only. XBUTTON messages return TRUE when consumed.
            const UINT xVk = (HIWORD(wParam) == XBUTTON1) ? kMouseX1 : kMouseX2;
            if (TryExecutePlayerMouseBinding(xVk)) { Render(); return TRUE; }
            break;
        }
        case WM_MOUSEWHEEL: {
            controlsIdleTimer = 0.0f;
            if (isCursorHidden) { isCursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            bool isPhysicalCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (HandleContextMenuWheel(delta)) {
                Render();
                return 0;
            }
            if (isSettingsFlyoutOpen && currentFlyoutMenu == "queue") {
                const size_t queueSize = playbackSession.GetQueueSize();
                const size_t maxFirst = queueSize > 7 ? queueSize - 7 : 0;
                if (delta > 0 && queueFlyoutFirstVisible > 0) {
                    --queueFlyoutFirstVisible;
                } else if (delta < 0 && queueFlyoutFirstVisible < maxFirst) { ++queueFlyoutFirstVisible; }
                Render();
                return 0;
            }
            // A bound wheel action overrides the volume wheel in the player
            // context only (checked after menu/queue scrolling, before the
            // hardcoded Ctrl subtitle-delay and volume behavior).
            if (TryExecutePlayerMouseBinding(delta >= 0 ? kMouseWheelUp : kMouseWheelDown)) {
                Render();
                return 0;
            }
            if (isPhysicalCtrl) {
                // Hold physical CTRL + Mouse Scroll: Adjust Subtitle Delay
                commandDispatcher.Execute(delta > 0 ? AppCommand::SubDelayPlus : AppCommand::SubDelayMinus);
                Render();
                return 0;
            }
            // Normal Touchpad / Mouse Scroll: Adjust Volume
            commandDispatcher.Execute(delta > 0 ? AppCommand::VolumeUp : AppCommand::VolumeDown);
            Render();
            return 0;
        }
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (uMsg == WM_SYSKEYDOWN && wParam == VK_SPACE && (lParam & (1LL << 29)) != 0)
                return HandleMessage(hWnd, uMsg, wParam, lParam);
            const bool handled = HandleGlobalKeyDown(wParam, lParam);
            if (handled) {
                playerTooltips.HideForKeyboard();
                if (playbackSession.IsOpen()) Render();
                return 0;
            }
            return HandleMessage(hWnd, uMsg, wParam, lParam);
        }
        case WM_DROPFILES: { return HandleMessage(hWnd, WM_DROPFILES, wParam, lParam); }
    }
    return DefWindowProc(hOverlay, uMsg, wParam, lParam);
}

LRESULT App::HandleVideoMessage(HWND hVideo, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!playbackSession.IsOpen()) { return DefWindowProc(hVideo, uMsg, wParam, lParam); }
    return HandleOverlayMessage(hVideo, uMsg, wParam, lParam);
}

bool App::CaptureClientContent(const std::wstring& outputPath) {
    // A flip-model Present rotates buffers. Draw the requested hub state into
    // the current buffer and finish it without rotating before readback.
    if (playbackSession.IsOpen()) return false;
    renderer.BeginDraw();
    previewPlayer.BindDeviceContext(renderer.GetD2DContext());
    auto vm = BuildHubRenderViewModel();
    UIComponents::DrawHub(renderer, settings, vm);
    if (!renderer.EndDraw(false)) return false;
    return renderer.CaptureBackBufferToPng(outputPath);
}

void App::SetTab(TabView tab) {
    currentTab = tab;
    activeFocusPane = FocusPane::TopNavBar;
    mediaScrollY = 0.0f;
    playlistRailScrollY = 0.0f;
    playlistDetailScrollY = 0.0f;
    historyScrollY = 0.0f;
    settingsScrollY = 0.0f;
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}

void App::SetSettingsCategory(SettingsCategory cat) {
    currentSettingsCat = cat;
    currentTab = TabView::Settings;
    settingsScrollY = 0.0f;
    hoveredSettingsRow = -1;
    if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
}

} // namespace Aperture
