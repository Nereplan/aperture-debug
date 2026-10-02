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

// Rail folder drops name new playlists after the dropped folder; dedupe
// against existing playlists so repeated drops read "Bleach", "Bleach (2)", ...
static std::string MakeUniquePlaylistName(const MediaLibrary& library, std::string base) {
    if (base.empty()) base = "New Playlist";
    auto exists = [&library](const std::string& name) {
        for (const auto& pl : library.playlists) { if (pl.name == name) return true; }
        return false;
    };
    if (!exists(base)) return base;
    for (int i = 2;; ++i) {
        std::string candidate = base + " (" + std::to_string(i) + ")";
        if (!exists(candidate)) return candidate;
    }
}


void App::OnOleDragEnter(IDataObject* pDataObj, POINTL pt) {
    isDragActive = true;
    // DragOver carries no IDataObject, so classify the payload once here: any
    // dropped folder switches the playlist rail into "create playlist" mode.
    oleDragPayloadHasFolder = false;
    if (pDataObj) {
        FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg;
        if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
            HDROP hDrop = (HDROP)stg.hGlobal;
            if (hDrop) {
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                for (UINT i = 0; i < count && !oleDragPayloadHasFolder; ++i) {
                    UINT reqLen = DragQueryFileW(hDrop, i, NULL, 0);
                    if (reqLen > 0) {
                        std::vector<wchar_t> pathBuf(reqLen + 1, L'\0');
                        if (DragQueryFileW(hDrop, i, pathBuf.data(), reqLen + 1)) { oleDragPayloadHasFolder = DirectoryExists(pathBuf.data()); }
                    }
                }
            }
            ReleaseStgMedium(&stg);
        }
    }
    OnOleDragOver(pt);
}


void App::OnOleDragOver(POINTL pt) {
    if (!hWnd) return;
    POINT ptClient = { pt.x, pt.y };
    ScreenToClient(hWnd, &ptClient);
    float x = (float)ptClient.x;
    float y = (float)ptClient.y;
    float dp = renderer.GetDpiScale();
    RECT rc;
    GetClientRect(hWnd, &rc);
    float winW = (float)(rc.right - rc.left);
    float winH = (float)(rc.bottom - rc.top);
    D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
    if (currentTab == TabView::Playlists && !playbackSession.IsOpen()) {
        float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
        D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
        D2D1_RECT_F detailBounds = D2D1::RectF(viewBounds.left + railW + 16.0f * dp, viewBounds.top, viewBounds.right, viewBounds.bottom);
        // A folder payload over the rail previews "create playlist from folder";
        // individual rail items only highlight for plain file payloads.
        isRailFolderDropActive = PtInRectF(railBounds, x, y) && oleDragPayloadHasFolder;
        hoveredPlaylistRailIndex = -1;
        if (!isRailFolderDropActive) {
            float itemH = 52.0f * dp, curY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
            for (size_t i = 0; i < library.playlists.size(); ++i) {
                if (x >= railBounds.left + 8.0f * dp && x <= railBounds.right - 8.0f * dp && y >= curY && y <= curY + itemH) {
                    hoveredPlaylistRailIndex = (int)i;
                    break;
                }
                curY += itemH + 6.0f * dp;
            }
        }
        // If in detail bounds, activate drop target banner
        isPlaylistDropTargetActive = PtInRectF(detailBounds, x, y);
    } else {
        isPlaylistDropTargetActive = false;
        isRailFolderDropActive = false;
        hoveredPlaylistRailIndex = -1;
    }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::OnOleDragLeave() {
    isDragActive = false;
    isPlaylistDropTargetActive = false;
    isRailFolderDropActive = false;
    oleDragPayloadHasFolder = false;
    hoveredPlaylistRailIndex = -1;
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::OnOleDrop(IDataObject* pDataObj, POINTL pt) {
    isDragActive = false;
    isPlaylistDropTargetActive = false;
    isRailFolderDropActive = false;
    oleDragPayloadHasFolder = false;
    hoveredPlaylistRailIndex = -1;
    if (!pDataObj) return;
    POINT ptClient = { pt.x, pt.y };
    ScreenToClient(hWnd, &ptClient);
    bool handledFiles = false;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg;
    if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
        HDROP hDrop = (HDROP)stg.hGlobal;
        if (hDrop) {
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            std::vector<std::wstring> droppedList;
            for (UINT i = 0; i < count; ++i) {
                UINT reqLen = DragQueryFileW(hDrop, i, NULL, 0);
                if (reqLen > 0) {
                    std::vector<wchar_t> pathBuf(reqLen + 1, L'\0');
                    if (DragQueryFileW(hDrop, i, pathBuf.data(), reqLen + 1))
                        droppedList.push_back(Ipc::ResolveFullPathLong(pathBuf.data()));
                }
            }
            if (!droppedList.empty()) { ProcessDroppedFiles(droppedList, ptClient); handledFiles = true; }
        }
        ReleaseStgMedium(&stg);
    }
    if (!handledFiles) {
        DroppedUrlResult dropResult = NetworkStreamValidator::ExtractAndValidateFromDataObject(pDataObj);
        if (dropResult.hasUrl) {
            if (dropResult.parsedUrl.isValid) {
                if (playbackSession.CanPersistRecentStreams(networkStreamModel.IsRecentStreamsEnabled())) {
                    networkStreamModel.AddRecentStream(dropResult.parsedUrl.rawUrl);
                    networkStreamModel.SaveRecentStreams(GetExeDir());
                }
                OpenAndPlayMedia(dropResult.parsedUrl.rawUrl);
            } else {
                std::wstring errMsg = dropResult.safeErrorMessage.empty()
                    ? NetworkStreamValidator::GetSafeErrorMessage(dropResult.parsedUrl.error, dropResult.extractedUrl)
                    : dropResult.safeErrorMessage;
                ShowOSD(IconType::VolumeMuted, errMsg, 4.0f);
            }
        }
    }
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::HandleDroppedFiles(HDROP hDrop) {
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    if (count == 0) { DragFinish(hDrop); return; }
    POINT dropPt = { 0, 0 };
    DragQueryPoint(hDrop, &dropPt);
    std::vector<std::wstring> droppedList;
    for (UINT i = 0; i < count; ++i) {
        UINT reqLen = DragQueryFileW(hDrop, i, NULL, 0);
        if (reqLen > 0) {
            std::vector<wchar_t> pathBuf(reqLen + 1, L'\0');
            if (DragQueryFileW(hDrop, i, pathBuf.data(), reqLen + 1)) { droppedList.push_back(Ipc::ResolveFullPathLong(pathBuf.data())); }
        }
    }
    DragFinish(hDrop);
    if (!droppedList.empty()) { ProcessDroppedFiles(droppedList, dropPt); }
}


void App::ProcessDroppedFiles(const std::vector<std::wstring>& droppedList, POINT dropPt) {
    if (droppedList.empty()) return;
    // Check if a supported subtitle was dropped while playing. Let mpv own
    // the active external track; only clear the custom fallback after a
    // successful backend load, and only show success OSD for a true result.
    if (playbackSession.IsOpen() && IsSupportedSubtitlePath(droppedList[0])) {
        bool loaded = player.LoadExternalSubtitle(droppedList[0]);
        if (loaded) { subtitles.Clear(); ShowOSD(IconType::Subtitles, L"Loaded Subtitle: " + GetFileNameFromPath(droppedList[0])); }
        return;
    }
    // Split folders from files. Folders are expanded by ImportPaths' recursive
    // scan, which accepts only player-compatible media and ignores the rest.
    std::vector<std::wstring> folderPaths, filePaths;
    for (const auto& path : droppedList) {
        if (DirectoryExists(path)) { folderPaths.push_back(path); } else { filePaths.push_back(path); }
    }
    float dp = renderer.GetDpiScale();
    float x = (float)dropPt.x;
    float y = (float)dropPt.y;
    // If on Playlists tab, intelligently route drop into targeted playlist
    if (!playbackSession.IsOpen() && currentTab == TabView::Playlists) {
        RECT rc;
        GetClientRect(hWnd, &rc);
        float winW = (float)(rc.right - rc.left);
        float winH = (float)(rc.bottom - rc.top);
        D2D1_RECT_F viewBounds = Presentation::HubView(winW, winH, dp);
        float railW = std::min(260.0f * dp, std::max(120.0f * dp, RectWidth(viewBounds) * 0.42f));
        D2D1_RECT_F railBounds = D2D1::RectF(viewBounds.left, viewBounds.top, viewBounds.left + railW, viewBounds.bottom);
        // Folder onto the rail: create one playlist per folder, named after the
        // folder, and queue its compatible videos into that playlist. Loose
        // files riding along in the same drop land in the active playlist (or
        // the first created one when it cannot take them).
        if (PtInRectF(railBounds, x, y) && !folderPaths.empty()) {
            std::string firstCreatedId;
            for (const auto& folder : folderPaths) {
                std::wstring baseName = GetFileNameFromPath(folder);
                if (baseName.empty()) baseName = L"New Playlist";
                const std::string uniqueName = MakeUniquePlaylistName(library, Utf16ToUtf8(baseName));
                Playlist* created = library.CreatePlaylist(uniqueName);
                if (!created) continue;
                if (firstCreatedId.empty()) firstCreatedId = created->id;
                pendingDropImports.push_back({ { folder }, created->id, Utf8ToUtf16(uniqueName) });
            }
            if (!firstCreatedId.empty()) {
                if (!filePaths.empty()) {
                    std::string looseTargetId = activePlaylistId;
                    Playlist* loosePl = library.FindPlaylist(looseTargetId);
                    if (!loosePl || loosePl->IsSmart()) looseTargetId = firstCreatedId;
                    pendingDropImports.push_back({ filePaths, looseTargetId, L"" });
                }
                ActivateCreatedPlaylist(firstCreatedId);
                PumpPendingDropImports();
                InvalidateRect(hWnd, NULL, FALSE);
                return;
            }
        }
        // Files (or folders) onto the open playlist detail: expand everything
        // into the targeted playlist without touching its name.
        std::string targetPlId = activePlaylistId;
        float itemH = 52.0f * dp, curY = railBounds.top + 48.0f * dp - 2.0f * dp - playlistRailScrollY;
        for (size_t i = 0; i < library.playlists.size(); ++i) {
            if (x >= railBounds.left && x <= railBounds.right && y >= curY && y <= curY + itemH) {
                targetPlId = library.playlists[i].id;
                break;
            }
            curY += itemH + 6.0f * dp;
        }
        // If no playlists exist yet, create one
        if (library.playlists.empty()) {
            Playlist newPl;
            newPl.id = "pl_favorites";
            newPl.name = "Favorites";
            library.playlists.push_back(newPl);
            targetPlId = newPl.id;
        }
        Playlist* targetPl = library.FindPlaylist(targetPlId);
        if (!targetPl && !library.playlists.empty()) { targetPl = &library.playlists[0]; targetPlId = targetPl->id; }
        if (targetPl) {
            activePlaylistId = targetPlId;
            // The worker discovers only supported, existing media and returns the exact accepted paths. FinishImportJob performs the
            // playlist merge, so unsupported files never become ghost entries.
            StartImportJob(droppedList, targetPlId, false);
            return;
        }
    }
    // Media library or other tabs: folders import their compatible videos into
    // the library; only a single dropped media FILE starts playback.
    StartImportJob(droppedList, "", droppedList.size() == 1 && folderPaths.empty());
}


void App::ArmCardDrag(DragType type, int index, float x, float y) {
    CancelCardDrag();
    const auto vm = BuildHubRenderViewModel();
    dragSourceRect = D2D1::RectF(x, y, x + 1, y + 1);
    if (type == DragType::MediaList) {
        const auto& visible = GetVisibleMediaIndices();
        for (size_t i : visible) dragPathSnapshot.push_back(library.mediaItems[i].filePath);
        if (index >= 0 && index < static_cast<int>(visible.size()))
            for (const auto& card : vm.mediaTab.visibleCards)
                if (card.originalIndex == visible[index]) dragSourceRect = card.itemRect;
    } else if (type == DragType::PlaylistRail) {
        for (const auto& pl : library.playlists) dragRailSnapshot.push_back(pl.id);
        if (index >= 0 && index < static_cast<int>(library.playlists.size()))
            for (const auto& card : vm.playlistsTab.rail.items)
                if (card.id == library.playlists[index].id) dragSourceRect = card.itemRect;
    } else if (type == DragType::PlaylistItem) {
        ClearPlaylistCardHover();
        dragPlaylistId = activePlaylistId;
        if (auto* pl = library.FindPlaylist(activePlaylistId)) dragPathSnapshot = pl->itemPaths;
        for (const auto& card : vm.playlistsTab.detail.visibleEpisodes)
            if (static_cast<int>(card.originalIndex) == index) dragSourceRect = card.itemRect;
    }
    pendingDragType = type;
    dragFromIndex = dragToIndex = index;
    dragStartX = dragCurrentX = x;
    dragStartY = dragCurrentY = y;
    SetCapture(hWnd);
}


void App::CancelCardDrag() {
    activeDragType = pendingDragType = DragType::None;
    dragFromIndex = dragToIndex = -1;
    isPlaylistsNavDropHovered = false;
    dragPathSnapshot.clear();
    dragPlaylistId.clear();
    dragRailSnapshot.clear();
    dragOffsets.clear();
    if (GetCapture() == hWnd) ReleaseCapture();
    InvalidateRect(hWnd, nullptr, FALSE);
}


D2D1_POINT_2F App::CardDragOffset(int index, float x, float y) {
    auto& offset = dragOffsets[index];
    offset.second = D2D1::Point2F(x, y);
    if (IsReducedMotionEnabled()) offset.first = offset.second;
    return offset.first;
}

} // namespace Aperture
