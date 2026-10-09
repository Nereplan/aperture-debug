#include "App.h"
#include "Win32IoCancellation.h"
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


void App::OpenFileDialog() { OpenFileDialogForTarget("", true); }


void App::OpenFileDialogForPlaylist(const std::string& playlistId) {
    if (playlistId.empty()) return;
    Playlist* playlist = library.FindPlaylist(playlistId);
    if (!playlist || playlist->IsSmart()) return;
    OpenFileDialogForTarget(playlistId, false);
}


void App::OpenFileDialogForTarget(const std::string& targetPlaylistId, bool launchSingle) {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        ShowOSD(IconType::VolumeMuted, L"Could not open file picker");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    const COMDLG_FILTERSPEC filters[] = {
        { L"Media Files", L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.m4v;*.3gp;*.ogv;*.ts;*.vob;*.mpg;*.mpeg;*.m2ts;*.divx;*.asf;*.rm;*.rmvb;*.3g2;*.f4v;*.m2v;*.mpe;*.mpv;*.m4p;*.mp3;*.wav;*.ogg;*.flac;*.aac;*.m4a;*.opus;*.wma;*.alac;*.aiff;*.pcm;*.mka" },
        { L"All Files", L"*.*" }
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(targetPlaylistId.empty() ? L"Open media files" : L"Add media to playlist");
    const HRESULT shown = dialog->Show(hWnd);
    if (shown == S_OK) {
        IShellItemArray* results = nullptr;
        std::vector<std::wstring> selectedFiles;
        if (SUCCEEDED(dialog->GetResults(&results)) && results) {
            DWORD count = 0;
            if (SUCCEEDED(results->GetCount(&count))) {
                selectedFiles.reserve(count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    PWSTR selectedPath = nullptr;
                    if (SUCCEEDED(results->GetItemAt(i, &item)) && item &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) && selectedPath) {
                        selectedFiles.push_back(Ipc::ResolveFullPathLong(selectedPath));
                    }
                    if (selectedPath) CoTaskMemFree(selectedPath);
                    if (item) item->Release();
                }
            }
            results->Release();
        }
        if (!selectedFiles.empty()) { StartImportJob(selectedFiles, targetPlaylistId, launchSingle && selectedFiles.size() == 1); }
    }
    dialog->Release();
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::OpenFolderDialog() {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        ShowOSD(IconType::VolumeMuted, L"Could not open folder picker");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Open a folder of media");
    const HRESULT shown = dialog->Show(hWnd);
    if (shown != S_OK) {
        dialog->Release();
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    IShellItem* selectedItem = nullptr;
    PWSTR selectedPath = nullptr;
    if (FAILED(dialog->GetResult(&selectedItem)) || !selectedItem ||
        FAILED(selectedItem->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath)) || !selectedPath) {
        if (selectedItem) selectedItem->Release();
        dialog->Release();
        ShowOSD(IconType::VolumeMuted, L"Could not read the selected folder");
        PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
        return;
    }
    const std::wstring folderPath = Ipc::ResolveFullPathLong(selectedPath);
    CoTaskMemFree(selectedPath);
    selectedItem->Release();
    dialog->Release();
    // ImportPaths recursively accepts only player-compatible media; every
    // other file inside the folder is ignored and reported in the summary.
    StartImportJob({ folderPath }, "", false);
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::OpenSubtitleFileDialog() {
    PushFocusSnapshot(TransientSurfaceType::SystemDialog);
    std::vector<wchar_t> szFile(Ipc::MAX_IPC_PATH_CHARS, L'\0');
    OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile.data();
    ofn.nMaxFile = static_cast<DWORD>(szFile.size());
    ofn.lpstrFilter = L"Subtitle Files (*.srt;*.vtt;*.ass;*.ssa)\0*.srt;*.vtt;*.ass;*.ssa\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (DelaySafeGetOpenFileNameW(&ofn)) {
        std::wstring subtitlePath = Ipc::ResolveFullPathLong(szFile.data());
        bool loaded = false;
        if (playbackSession.IsOpen()) {
            // Once mpv is active, let it own every supported external subtitle format so tracks are refreshed and styled consistently. Do
            // not clear the custom fallback until the backend accepts the file.
            loaded = player.LoadExternalSubtitle(subtitlePath);
            if (loaded) subtitles.Clear();
        } else {
            // Before playback starts the lightweight parser remains useful as the pre-player fallback subtitle source.
            loaded = subtitles.LoadSubtitleFile(subtitlePath);
        }
        if (loaded) { ShowOSD(IconType::Subtitles, L"Loaded Subtitle: " + GetFileNameFromPath(subtitlePath)); }
    }
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}

void App::OpenOnlineSubtitlesDialog() {
    float dp = overlayRenderer.GetDpiScale();
    RECT rc;
    GetClientRect(hWnd, &rc);
    float x = ((rc.right - rc.left) - 300.0f * dp) / 2.0f;
    float y = ((rc.bottom - rc.top) - 240.0f * dp) / 2.0f;
    OpenOnlineSubtitlesContextMenu(std::max(8.0f * dp, x), std::max(8.0f * dp, y));
}


void App::OpenInExplorer(const std::wstring& filePath) {
    if (filePath.empty()) return;
    std::wstring param = L"/select,\"" + filePath + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", param.c_str(), NULL, SW_SHOWNORMAL);
}

bool App::TakeScreenshot() {
    std::lock_guard<std::mutex> lock(screenshotMutex);
    if (shutdownComplete) return false;
    if (!playbackSession.IsOpen()) { ShowOSD(IconType::VolumeMuted, L"Screenshot unavailable: no media loaded", 2.0f); return false; }
    std::wstring mediaPath = playbackSession.GetCurrentFilePath();
    if (mediaPath.empty()) { mediaPath = player.GetCurrentFilePath(); }
    if (mediaPath.empty()) { ShowOSD(IconType::VolumeMuted, L"Screenshot unavailable: no media loaded", 2.0f); return false; }
    std::wstring root = GetExeDir();
    if (root.empty()) {
        StorageStatus err = StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Application root directory path is empty");
        ReportStorageFailure(err, L"Screenshots");
        return false;
    }
    // 1. Ensure screenshots directory is created only on first capture
    StorageStatus dirStatus = EnsureScreenshotsDirectoryCreated(root);
    if (!dirStatus.is_ok()) { ReportStorageFailure(dirStatus, L"Screenshots"); return false; }
    // 2. Generate collision-safe deterministic screenshot file path
    std::wstring targetPath = GenerateScreenshotFilePath(mediaPath, root);
    if (targetPath.empty() || !IsScreenshotPathSafe(targetPath, root)) {
        AppError err = AppError::Storage( static_cast<int>(StorageErrorCode::InvalidPath), ERROR_ACCESS_DENIED,
            L"Save failed: Invalid or unsafe screenshot path.", L"Path escape or invalid screenshot path detected: " + targetPath,
            targetPath);
        ReportAppError(err);
        return false;
    }
    // 3. Delegate decoded video frame capture to player
    StorageStatus captureStatus = player.TakeScreenshot(targetPath);
    if (!captureStatus.is_ok()) { ReportStorageFailure(captureStatus, L"Screenshots"); return false; }
    // 4. Update last screenshot path and project root-relative path to user
    lastScreenshotPath = targetPath;
    std::wstring relPath = ToRelocatablePath(targetPath, root);
    ShowOSD(IconType::Checkmark, L"Screenshot saved: " + relPath, 3.0f);
    RequestRender();
    return true;
}

bool App::RegisterFileAssociations() {
    // Portable containment: registering Open-With verbs requires writing to HKCU\Software\Classes, outside the application folder. Strict
    // portable mode refuses all durable writes outside the root, so registration is disabled. Media is opened via in-app picker, drag/drop,
    // or CLI.
    return false;
}


// Thumbnail cache keys are one-way hashes, so removing a media item must
// regenerate its candidate keys and drop each cached file. The hub entry is
// size-independent (one per file); seekbar entries are per-second buckets.
void App::PurgeThumbnailCacheForFile(const std::wstring& filePath, double durationSec) {
    PurgeThumbnailDiskCacheForFile(filePath, durationSec);
}


void App::PruneOrphanedHubThumbnailCache() {
    std::vector<std::wstring> livePaths;
    livePaths.reserve(library.mediaItems.size());
    for (const auto& item : library.mediaItems) { livePaths.push_back(item.filePath); }
    PruneOrphanedHubThumbnailDiskCache(livePaths);
}


void App::RemoveSelectedFolders() {
    if (selectedFolderKeys.empty()) return;
    if (libraryGroupMode == LibraryGroupMode::Series) {
        const auto& proj = GetCurrentMediaProjection(true);
        std::vector<std::string> idsToRemove;
        size_t seriesCount = 0;
        for (const auto& g : proj.groups) {
            if (selectedFolderKeys.count(g.key) > 0) {
                seriesCount++;
                for (size_t i = 0; i < g.count; ++i) {
                    size_t pIdx = g.startIndex + i;
                    if (pIdx < proj.indices.size()) { idsToRemove.push_back(library.mediaItems[proj.indices[pIdx]].id); }
                }
            }
        }
        size_t removed = 0;
        if (!idsToRemove.empty()) { removed = library.RemoveMedia(idsToRemove); }
        selectedFolderKeys.clear();
        m_mediaQueryEngine.InvalidateCache();
        InvalidateMediaLayoutCache();
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        std::wstring msg = L"Removed " + std::to_wstring(seriesCount) + L" series (" + std::to_wstring(removed) + L" episodes)";
        ShowOSD(IconType::Trash, msg);
        AnnounceStatus(msg);
        return;
    }
    std::vector<std::wstring> foldersToRemove(selectedFolderKeys.begin(), selectedFolderKeys.end());
    size_t removed = library.RemoveFolders(foldersToRemove, true);
    selectedFolderKeys.clear();
    m_mediaQueryEngine.InvalidateCache();
    InvalidateMediaLayoutCache();
    ClampScrollOffsets();
    InvalidateRect(hWnd, NULL, FALSE);
    std::wstring msg = L"Removed " + std::to_wstring(foldersToRemove.size()) + L" folder(s) (" + std::to_wstring(removed) + L" videos)";
    ShowOSD(IconType::Trash, msg);
    AnnounceStatus(msg);
}


void App::RunFolderRediscovery(bool showFeedback) {
    if (!libraryReady || shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    folderRediscoveryFeedback |= showFeedback;
    if (folderRediscoveryActive) { folderRediscoveryAgain = true; return; }
    try {
        auto snapshot = FolderRediscoverySnapshot::Capture(library);
        folderRediscoveryActive = folderRediscoveryJob.Start([snapshot = std::move(snapshot)](const auto& cancellation) {
            Win32IoCancellation interrupt(cancellation);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            return PrepareFolderRediscovery(snapshot, cancellation);
        }) != 0;
        if (folderRediscoveryActive && showFeedback) ShowOSD(IconType::Refresh, L"Looking for moved folders");
    } catch (...) {
        folderRediscoveryActive = false;
        folderRediscoveryFeedback = false;
        ReportStorageFailure(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_NOT_ENOUGH_MEMORY,
            L"", "Could not prepare folder rediscovery"), L"folder rediscovery");
    }
}


void App::FinishFolderRediscovery() {
    if (!folderRediscoveryActive || shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    auto completion = folderRediscoveryJob.TakeResult();
    if (!completion) return;
    folderRediscoveryActive = false;
    if (!completion->result) {
        folderRediscoveryFeedback = folderRediscoveryAgain = false;
        ReportStorageFailure(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_READ_FAULT,
            L"", "Folder rediscovery worker failed"), L"folder rediscovery");
        return;
    }
    const auto previousWatches = library.watchedFolders;
    const auto res = library.ApplyFolderRediscovery(*completion->result, true);
    if (!res.status.is_ok()) ReportStorageFailure(res.status, L"folder rediscovery");
    if (res.renamedFoldersCount > 0 || res.mergedItemsCount > 0 || res.relocatedItemsCount > 0 || res.removedGhostCount > 0) {
        folderWatcher.SetWatchedFoldersAsync(library.GetWatchedFolders());
        for (const auto& rename : res.renamedFolders) {
            const auto oldKey = NormalizePathKey(rename.first);
            if (std::any_of(previousWatches.begin(), previousWatches.end(), [&](const auto& root) { return NormalizePathKey(root) == oldKey; })) {
                watchReconcileWorker.Queue({true, rename.second, {}});
            }
        }
        m_mediaQueryEngine.InvalidateCache();
        InvalidateMediaLayoutCache();
        ClampScrollOffsets();
        InvalidateRect(hWnd, NULL, FALSE);
        ScheduleMetadataBackfill();
        std::wstring msg = L"Rediscovered " + std::to_wstring(res.renamedFoldersCount) + L" renamed folder(s)";
        if (res.mergedItemsCount > 0) { msg += L" (" + std::to_wstring(res.mergedItemsCount) + L" items reconciled)"; }
        ShowOSD(IconType::Checkmark, msg);
        AnnounceStatus(msg);
    } else if (folderRediscoveryFeedback && res.status.is_ok() && !res.needsRescan && !folderRediscoveryAgain) {
        ShowOSD(IconType::Checkmark, L"No folder changes found");
    }
    const bool again = folderRediscoveryAgain || res.needsRescan;
    folderRediscoveryAgain = false;
    if (again) RunFolderRediscovery(false);
    else folderRediscoveryFeedback = false;
}

} // namespace Aperture
