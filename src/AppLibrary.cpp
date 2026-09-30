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


void App::StartLibraryLoad(LibraryGroupMode groupMode, bool preserveExisting) {
    if (libraryLoadInProgress) return;
    if (libraryLoadWorker.joinable()) libraryLoadWorker.join();
    libraryLoadCancelRequested.store(false, std::memory_order_release);
    libraryLoadInProgress = true;
    if (!preserveExisting) { libraryReady = false; libraryLoadFailed = false; librarySnapshotFrameRendered = false; }
    deferredLibraryReadyWorkPending = false;
    libraryLoadStatus = StorageStatus::Ok();
    const uint64_t generation = librarySnapshotMailbox.BeginGeneration();
    const std::wstring rootDir = GetExeDir();
    const HWND notifyWindow = hWnd;
    const uint64_t baseVersion = library.GetVersion();
    libraryLoadWorker = std::thread([this, generation, groupMode, rootDir, notifyWindow, preserveExisting, baseVersion]() {
        LibrarySnapshotBuildOptions options;
        options.generation = generation;
        options.groupMode = groupMode;
        options.rootDir = rootDir;
        options.preservesExisting = preserveExisting;
        options.baseLibraryVersion = baseVersion;
        options.cancelRequested = &libraryLoadCancelRequested;
        options.milestone = [](const wchar_t* phase) { Diagnostics::RecordStartupMilestone(phase, false); };
        auto snapshot = BuildLibraryStartupSnapshot(options);
        if (!snapshot || snapshot->canceled || libraryLoadCancelRequested.load(std::memory_order_acquire)) { return; }
        if (!librarySnapshotMailbox.Publish(std::move(snapshot))) return;
        Diagnostics::RecordStartupMilestone(L"library-snapshot-posted", false);
        PostMessageW(notifyWindow, kLibrarySnapshotReadyMessage, static_cast<WPARAM>(generation), 0);
    });
}


void App::FinishInitialLibraryLoad(uint64_t generation) {
    if (libraryLoadWorker.joinable()) libraryLoadWorker.join();
    std::unique_ptr<LibraryStartupSnapshot> snapshot = librarySnapshotMailbox.Take(generation);
    if (!snapshot || shutdownComplete) return;
    libraryLoadInProgress = false;
    libraryLoadStatus = snapshot->loadStatus;
    if (!snapshot->publishable || !snapshot->loadStatus.is_ok()) {
        libraryLoadFailed = !snapshot->preservesExisting;
        libraryReady = snapshot->preservesExisting && libraryReady;
        deferredLibraryCommands.clear();
        ReportStorageFailure(snapshot->loadStatus, L"media library startup");
        InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }
    if (snapshot->preservesExisting && library.GetVersion() != snapshot->baseLibraryVersion) {
        ShowOSD(IconType::Refresh, L"Library changed while refresh was running; kept the newer in-memory state", 4.0f);
        return;
    }
    library = std::move(snapshot->library);
    m_mediaQueryEngine.InvalidateCache();
    LibraryQueryOptions initialOptions;
    initialOptions.groupMode = snapshot->groupMode;
    initialOptions.sortField = LibrarySortField::Title;
    initialOptions.sortDirection = SortDirection::Ascending;
    const bool matchingMode = snapshot->groupMode == libraryGroupMode;
    const bool primed = matchingMode && m_mediaQueryEngine.PrimeCache( library, initialOptions, std::move(snapshot->initialProjection));
    if (primed) {
        m_cachedVisibleMediaIndices = std::move(snapshot->visibleIndices);
        m_cachedMediaLibraryVersion = library.GetVersion();
        m_cachedMediaGroupMode = libraryGroupMode;
        m_cachedMediaSortField = LibrarySortField::Title;
        m_cachedMediaSortDirection = SortDirection::Ascending;
    } else {
        m_cachedVisibleMediaIndices.clear();
        m_cachedMediaLibraryVersion = (std::numeric_limits<uint64_t>::max)();
        m_cachedMediaGroupMode = (LibraryGroupMode)-1;
        m_cachedMediaSortField = (LibrarySortField)-1;
        m_cachedMediaSortDirection = (SortDirection)-1;
    }
    InvalidateMediaLayoutCache();
    rowAnimStates.clear();
    selectedMediaIndices.clear();
    selectedMediaIndex = -1;
    activePlaylistId = library.playlists.empty() ? std::string() : library.playlists.front().id;
    libraryReady = true;
    libraryLoadFailed = false;
    deferredLibraryReadyWorkPending = true;
    Diagnostics::RecordStartupMilestone(L"library-snapshot-adopted", false);
    if (snapshot->preservesExisting) ShowOSD(IconType::Refresh, L"Library Refreshed");
    // Commands received while loading are applied only after the authoritative snapshot is live, so imports/progress updates can never
    // target the placeholder library.
    std::deque<PlaybackCommandLineOptions> commands;
    commands.swap(deferredLibraryCommands);
    for (const auto& command : commands) ExecuteCommandLineOptions(command);
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, nullptr, FALSE);
}


void App::RunDeferredLibraryReadyWork() {
    if (!deferredLibraryReadyWorkPending || !libraryReady) return;
    if (!librarySnapshotFrameRendered && !playbackSession.IsOpen()) return;
    deferredLibraryReadyWorkPending = false;
    if (settings.jumpListEnabled) jumpListController.Update(library.history);
    for (const auto& folder : library.GetWatchedFolders()) { folderWatcher.AddWatch(folder); }
    if (!library.GetWatchedFolders().empty()) folderWatcher.Start();
    library.RediscoverFolderChanges(true);
    ScheduleMetadataBackfill();
}


void App::CancelInitialLibraryLoad() {
    libraryLoadCancelRequested.store(true, std::memory_order_release);
    librarySnapshotMailbox.Cancel();
    if (libraryLoadWorker.joinable()) libraryLoadWorker.join();
    libraryLoadInProgress = false;
    deferredLibraryCommands.clear();
}


const LibraryQueryProjection& App::GetCurrentMediaProjection() {
    LibraryQueryOptions opt;
    opt.groupMode = libraryGroupMode;
    opt.sortField = (librarySortField == LibrarySortField::Manual && libraryGroupMode != LibraryGroupMode::Flat)
        ? LibrarySortField::Title : librarySortField;
    opt.sortDirection = librarySortDirection;
    opt.searchQuery = Text16ToWideString(mediaSearchInput.GetText());
    opt.pinLastWatched = !isLastWatchedCleared && opt.searchQuery.empty();
    // Shared snapshots (empty set stays canonical): no per-call deep copy of
    // the collapsed-group keys on this every-frame path.
    opt.collapsedGroups = opt.searchQuery.empty() ? CollapsedGroupsSnapshot() : EmptyCollapsedGroups();
    // Flat, series, and duplicates queries never consume manual folder order.
    // Copying it for every frame also makes the query cache compare every path.
    if (libraryGroupMode == LibraryGroupMode::Folder) opt.customFolderOrder = settings.customFolderOrder;
    // Single copy lives in the engine cache; App keeps only the compact
    // visible-index vector. Rebuild indices when the engine produced a new projection (version/mode change).
    const LibraryQueryProjection& proj = m_mediaQueryEngine.Query(library, opt);
    uint64_t libVer = library.GetVersion();
    if (m_cachedMediaLibraryVersion != libVer || m_cachedMediaGroupMode != libraryGroupMode || m_cachedMediaSortField != opt.sortField ||
        m_cachedMediaSortDirection != opt.sortDirection || m_cachedCollapsedGroupsGeneration != m_collapsedGroupsGeneration ||
        m_cachedLastWatchedCleared != isLastWatchedCleared || m_cachedMediaSearchQuery != opt.searchQuery ||
        m_cachedVisibleMediaIndices.size() != proj.items.size()) {
        m_cachedMediaLibraryVersion = libVer;
        m_cachedMediaGroupMode = libraryGroupMode;
        m_cachedMediaSortField = opt.sortField;
        m_cachedMediaSortDirection = opt.sortDirection;
        m_cachedCollapsedGroupsGeneration = m_collapsedGroupsGeneration;
        m_cachedLastWatchedCleared = isLastWatchedCleared;
        m_cachedMediaSearchQuery = opt.searchQuery;
        m_cachedVisibleMediaIndices.clear();
        m_cachedVisibleMediaIndices.reserve(proj.items.size());
        for (const auto& item : proj.items) { m_cachedVisibleMediaIndices.push_back(item.originalLibraryIndex); }
    }
    return proj;
}


const std::vector<size_t>& App::GetVisibleMediaIndices() { GetCurrentMediaProjection(); return m_cachedVisibleMediaIndices; }


void App::SetLibraryGroupMode(LibraryGroupMode mode) {
    if (libraryGroupMode == mode) return;
    libraryGroupMode = mode;
    if (mode == LibraryGroupMode::Flat) settings.libraryGroupMode = "flat";
    else if (mode == LibraryGroupMode::Folder) { settings.libraryGroupMode = "folder"; library.RediscoverFolderChanges(true); }
    else if (mode == LibraryGroupMode::Series) settings.libraryGroupMode = "series";
    else if (mode == LibraryGroupMode::Duplicates) settings.libraryGroupMode = "duplicates";
    settings.Save();
    m_cachedMediaLibraryVersion = (std::numeric_limits<uint64_t>::max)();
    m_mediaQueryEngine.InvalidateCache();
    InvalidateMediaLayoutCache();
    mediaScrollY = 0.0f;
    ClampScrollOffsets();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::SetLibrarySort(LibrarySortField field, SortDirection direction) {
    const std::string fieldSetting = LibrarySortSettingValue(field);
    const std::string directionSetting = direction == SortDirection::Descending ? "descending" : "ascending";
    if (librarySortField == field && librarySortDirection == direction &&
        settings.librarySortField == fieldSetting && settings.librarySortDirection == directionSetting) {
        return;
    }
    librarySortField = field;
    librarySortDirection = direction;
    settings.librarySortField = fieldSetting;
    settings.librarySortDirection = directionSetting;
    settings.Save();
    m_cachedMediaLibraryVersion = (std::numeric_limits<uint64_t>::max)();
    m_cachedMediaSortField = (LibrarySortField)-1;
    m_cachedMediaSortDirection = (SortDirection)-1;
    m_mediaQueryEngine.InvalidateCache();
    InvalidateMediaLayoutCache();
    mediaScrollY = 0.0f;
    ClampScrollOffsets();
    hubTooltipSignature = 0;
    MarkAccessibilityDirty();
    InvalidateRect(hWnd, NULL, FALSE);
}


std::wstring App::GetLibrarySortLabel() const {
    LibrarySortField effective = librarySortField;
    if (effective == LibrarySortField::Manual && libraryGroupMode != LibraryGroupMode::Flat) { effective = LibrarySortField::Title; }
    if (effective == LibrarySortField::Manual) return L"Manual";
    std::wstring label;
    switch (effective) {
        case LibrarySortField::DateAdded: label = L"Date Added"; break;
        case LibrarySortField::LastPlayed: label = L"Last Played"; break;
        case LibrarySortField::Duration: label = L"Duration"; break;
        case LibrarySortField::Progress: label = L"Progress"; break;
        case LibrarySortField::Title:
        default: label = L"Title"; break;
    }
    label += librarySortDirection == SortDirection::Descending ? L" \x2193" : L" \x2191";
    return label;
}


void App::InvalidateMediaLayoutCache(size_t fromIndex) {
    if (!m_mediaLayoutCache.dirty) {
        m_mediaLayoutCache.dirtyFromIndex = fromIndex;
    } else { m_mediaLayoutCache.dirtyFromIndex = std::min(m_mediaLayoutCache.dirtyFromIndex, fromIndex); }
    m_mediaLayoutCache.dirty = true;
}


void App::EnsureMediaLayoutCache() {
    const auto& proj = GetCurrentMediaProjection();
    const auto& visibleIndices = m_cachedVisibleMediaIndices;
    const size_t N = visibleIndices.size();
    const uint64_t libVer = library.GetVersion();
    const bool shapeChanged = m_mediaLayoutCache.libraryVersion != libVer || m_mediaLayoutCache.itemCount != N ||
        m_mediaLayoutCache.groupMode != libraryGroupMode || m_mediaLayoutCache.lastWatchedCleared != isLastWatchedCleared ||
        m_mediaLayoutCache.collapsedGroupsGeneration != m_collapsedGroupsGeneration || m_mediaLayoutCache.rowOffsetsDIP.size() != N + 1;
    if (!m_mediaLayoutCache.dirty && !shapeChanged) { return; }
    const size_t firstChanged = shapeChanged || rowAnimStates.size() != N
        ? 0 : std::min(m_mediaLayoutCache.dirtyFromIndex, N);
    m_mediaLayoutCache.rowOffsetsDIP.resize(N + 1);
    m_mediaLayoutCache.libraryVersion = libVer;
    m_mediaLayoutCache.itemCount = N;
    m_mediaLayoutCache.groupMode = libraryGroupMode;
    m_mediaLayoutCache.lastWatchedCleared = isLastWatchedCleared;
    m_mediaLayoutCache.collapsedGroupsGeneration = m_collapsedGroupsGeneration;
    m_mediaLayoutCache.dirty = false;
    m_mediaLayoutCache.dirtyFromIndex = N;
    if (rowAnimStates.size() != N) {
        rowAnimStates.resize(N);
        m_activeAnimRowIndices.clear();
        for (size_t i = 0; i < N; ++i) {
            bool isPinned = (!isLastWatchedCleared && i == 0 && libraryGroupMode == LibraryGroupMode::Flat &&
                (visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0));
            rowAnimStates[i].height.SnapTo(isPinned ? kMediaPinnedRestHeight : kMediaRestHeight);
            rowAnimStates[i].liftY.SnapTo(0.0f);
            rowAnimStates[i].previewOpacity.SnapTo(0.0f);
            rowAnimStates[i].hoverEnterTime = -1.0f;
            rowAnimStates[i].isHovering = false;
            rowAnimStates[i].inActiveAnimList = false;
        }
    }
    const float kGroupHeaderHDIP = 38.0f;
    const bool hasPinned = !isLastWatchedCleared && N > 0 && libraryGroupMode == LibraryGroupMode::Flat &&
        (visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0);
    float curYDIP = 0.0f;
    if (libraryGroupMode == LibraryGroupMode::Flat) {
        m_mediaLayoutCache.groupHeaderOffsetsDIP.clear();
        curYDIP = UpdateFlatMediaRowOffsets(m_mediaLayoutCache.rowOffsetsDIP, N, firstChanged, [&](size_t i) {
            const bool isPinned = hasPinned && i == 0;
            const float baseH = isPinned ? kMediaPinnedRestHeight : kMediaRestHeight;
            return (i < rowAnimStates.size() && rowAnimStates[i].height.current > 10.0f)
                ? rowAnimStates[i].height.current : baseH;
        });
    } else {
        m_mediaLayoutCache.groupHeaderOffsetsDIP.resize(proj.groups.size());
        size_t currentItemIdx = 0;
        for (size_t g = 0; g < proj.groups.size(); ++g) {
            const auto& grp = proj.groups[g];
            m_mediaLayoutCache.groupHeaderOffsetsDIP[g] = curYDIP;
            curYDIP += kGroupHeaderHDIP;
            if (!grp.isCollapsed) {
                for (size_t k = 0; k < grp.count && currentItemIdx < N; ++k, ++currentItemIdx) {
                    m_mediaLayoutCache.rowOffsetsDIP[currentItemIdx] = curYDIP;
                    const float baseH = kMediaRestHeight;
                    const float itemH = (currentItemIdx < rowAnimStates.size() && rowAnimStates[currentItemIdx].height.current > 10.0f)
                        ? rowAnimStates[currentItemIdx].height.current : baseH;
                    curYDIP += itemH + 14.0f;
                }
            }
        }
        m_mediaLayoutCache.rowOffsetsDIP[N] = curYDIP;
    }
    m_mediaLayoutCache.contentHeightDIP = curYDIP;
}


std::pair<size_t, size_t> App::GetVisibleMediaRowRange(float scrollYDIP, float viewportHDIP, size_t totalCount, int overscan) {
    EnsureMediaLayoutCache();
    if (totalCount == 0) return { 0, 0 };
    const auto& offsets = m_mediaLayoutCache.rowOffsetsDIP;
    auto itFirst = std::upper_bound(offsets.begin(), offsets.begin() + totalCount, scrollYDIP);
    size_t firstVisible = 0;
    if (itFirst != offsets.begin()) { firstVisible = (size_t)std::distance(offsets.begin(), itFirst) - 1; }
    float targetBottomDIP = scrollYDIP + viewportHDIP;
    auto itLast = std::upper_bound(offsets.begin(), offsets.begin() + totalCount, targetBottomDIP);
    size_t lastVisible = (size_t)std::distance(offsets.begin(), itLast);
    if (lastVisible < totalCount) { lastVisible++; }
    lastVisible = std::min(totalCount, lastVisible);
    size_t first = (firstVisible >= (size_t)overscan) ? (firstVisible - (size_t)overscan) : 0;
    size_t last = std::min(totalCount, lastVisible + (size_t)overscan);
    return { first, last };
}


int App::HitTestMediaRow(float yDIP, size_t totalCount) {
    EnsureMediaLayoutCache();
    if (totalCount == 0 || yDIP < 0.0f) return -1;
    const auto& offsets = m_mediaLayoutCache.rowOffsetsDIP;
    if (offsets.size() <= totalCount || yDIP >= offsets[totalCount]) return -1;
    auto it = std::upper_bound(offsets.begin(), offsets.begin() + totalCount, yDIP);
    if (it == offsets.begin()) return -1;
    size_t idx = (size_t)(std::distance(offsets.begin(), it) - 1);
    float topDIP = offsets[idx];
    float itemHDIP = GetMediaRowHeightDIP(idx);
    if (yDIP >= topDIP && yDIP <= topDIP + itemHDIP) { return (int)idx; }
    return -1;
}


int App::HitTestMediaGroupHeader(float yDIP, size_t groupCount) {
    EnsureMediaLayoutCache();
    if (groupCount == 0 || yDIP < 0.0f) return -1;
    const auto& offsets = m_mediaLayoutCache.groupHeaderOffsetsDIP;
    const float kGroupHeaderHDIP = 38.0f;
    for (size_t g = 0; g < offsets.size() && g < groupCount; ++g) {
        if (yDIP >= offsets[g] && yDIP < offsets[g] + kGroupHeaderHDIP) { return (int)g; }
    }
    return -1;
}


float App::GetMediaRowTopDIP(size_t index) {
    EnsureMediaLayoutCache();
    const auto& visibleIndices = GetVisibleMediaIndices();
    if (index >= visibleIndices.size()) return 0.0f;
    return m_mediaLayoutCache.rowOffsetsDIP[index];
}


float App::GetMediaRowHeightDIP(size_t index) {
    EnsureMediaLayoutCache();
    const auto& visibleIndices = GetVisibleMediaIndices();
    if (index >= visibleIndices.size()) return 0.0f;
    const bool isPinned = (!isLastWatchedCleared && index == 0 && libraryGroupMode == LibraryGroupMode::Flat &&
        (visibleIndices[0] < library.mediaItems.size() && library.mediaItems[visibleIndices[0]].lastWatchedTimestamp > 0));
    return (index < rowAnimStates.size() && rowAnimStates[index].height.current > 10.0f) ? rowAnimStates[index].height.current
        : (isPinned ? kMediaPinnedRestHeight : kMediaRestHeight);
}


void App::EnqueueActiveRowAnimation(size_t index) {
    if (index >= rowAnimStates.size()) return;
    if (!rowAnimStates[index].inActiveAnimList) { rowAnimStates[index].inActiveAnimList = true; m_activeAnimRowIndices.push_back(index); }
}


void App::HandleFolderWatchEvents() {
    if (shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    std::vector<WatchChangeBatch> batches;
    { std::lock_guard<std::mutex> lock(pendingWatchMutex); batches.swap(pendingWatchBatches); }
    if (batches.empty()) return;
    bool libraryModified = false;
    for (const auto& batch : batches) {
        WatchReconcileResult res = library.ReconcileWatchBatch(batch, true);
        if (res.addedCount > 0 || res.updatedCount > 0 || res.removedCount > 0 || res.renamedCount > 0 || res.rescanCount > 0)
            libraryModified = true;
    }
    if (libraryModified) {
        InvalidateMediaLayoutCache();
        InvalidateRect(hWnd, NULL, FALSE);
        if (!shutdownComplete && !playerCallbacksDisabled.load(std::memory_order_acquire)) { ScheduleMetadataBackfill(); }
        AnnounceStatus(L"Library updated from watched folder");
    }
}


void App::AddWatchedFolderPrompt() {
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
    dialog->SetTitle(L"Select a folder to watch for media files");
    const HRESULT shown = dialog->Show(hWnd);
    if (shown != S_OK) { dialog->Release(); PopAndRestoreFocus(TransientSurfaceType::SystemDialog); return; }
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
    const std::wstring watchedPath = selectedPath;
    CoTaskMemFree(selectedPath);
    selectedItem->Release();
    dialog->Release();
    if (library.AddWatchedFolder(watchedPath)) {
        folderWatcher.AddWatch(watchedPath);
        // Start on demand: Initialize() leaves the watcher stopped when the profile has no watched folders so an idle hub keeps one fewer
        // thread/IOCP.
        if (!folderWatcher.IsRunning()) { folderWatcher.Start(); }
        library.ReconcileRescan(watchedPath);
        InvalidateMediaLayoutCache();
        InvalidateRect(hWnd, NULL, FALSE);
        ShowOSD(IconType::FolderPlaylist, L"Added Watched Folder: " + GetFileNameFromPath(watchedPath));
        ScheduleMetadataBackfill();
    } else { ShowOSD(IconType::Checkmark, L"Folder is already being watched"); }
    PopAndRestoreFocus(TransientSurfaceType::SystemDialog);
}


void App::RescanAllWatchedFolders() {
    const auto& folders = library.GetWatchedFolders();
    if (folders.empty()) { ShowOSD(IconType::VolumeMuted, L"No watched folders configured"); return; }
    for (const auto& f : folders) { library.ReconcileRescan(f, true); }
    InvalidateMediaLayoutCache();
    InvalidateRect(hWnd, NULL, FALSE);
    ScheduleMetadataBackfill();
    ShowOSD(IconType::Refresh, L"Watched Folders Rescanned");
}


void App::TogglePrivateMode() {
    const bool enabled = !playbackSession.IsPrivateMode();
    playbackSession.SetPrivateMode(enabled);
    previewPlayer.SetPrivateMode(enabled);
    hubExtractors[0].SetPrivateMode(enabled);
    hubExtractors[1].SetPrivateMode(enabled);
    renderer.SetThumbnailPrivateMode(enabled);
    ShowOSD(IconType::SettingsGear, enabled ? L"Private session enabled" : L"Private session disabled");
    InvalidateRect(hWnd, nullptr, FALSE);
}


double App::HubExtractStartSecForPath(const std::wstring& path) {
    for (const auto& item : library.mediaItems) {
        if (item.filePath == path) {
            if (item.lastPosSec > 0.0) return item.lastPosSec;
            if (item.durationSec > 5.0) return std::min(item.durationSec * 0.1, 10.0);
            return 0.0;
        }
    }
    return 0.0;
}


bool App::IsHubExtractVideoPath(const std::wstring& path) {
    static const wchar_t* kVideoExts[] = {
        L".mp4", L".mkv", L".avi", L".mov", L".webm", L".wmv", L".flv", L".m4v", L".3gp", L".ogv", L".ts", L".vob",
        L".mpg", L".mpeg", L".m2ts", L".divx", L".asf", L".rm", L".rmvb", L".3g2", L".f4v", L".m2v", L".mpe", L".mpv", };
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    for (const wchar_t* v : kVideoExts) { if (ext == v) return true; }
    // Audio and unknown types have no video frame; keep the icon.
    return false;
}


void App::CollectVisibleVideoPaths() {
    const uint64_t now = GetTickCount64();
    if (now - lastVisibleCollectMs < 100) return;
    lastVisibleCollectMs = now;
    cachedVisibleVideoPaths.clear();
    RECT rcClient{};
    if (hWnd) GetClientRect(hWnd, &rcClient);
    const float dp = std::max(0.5f, renderer.GetDpiScale());
    const float winH = static_cast<float>(rcClient.bottom - rcClient.top);
    const float viewportHDIP = (winH > 0.0f) ? (winH / dp) : 1000.0f;
    if (currentTab == TabView::Media) {
        EnsureMediaLayoutCache();
        const auto& visible = GetVisibleMediaIndices();
        float scrollYDIP = mediaScrollY / dp;
        // Overscan 6 rows above and below for seamless scrolling
        auto [first, last] = GetVisibleMediaRowRange(scrollYDIP, viewportHDIP, visible.size(), 6);
        for (size_t i = first; i < last && i < visible.size(); ++i) {
            size_t idx = visible[i];
            if (idx < library.mediaItems.size()) { cachedVisibleVideoPaths.push_back(library.mediaItems[idx].filePath); }
        }
    } else if (currentTab == TabView::Playlists) {
        const Playlist* pl = library.FindPlaylist(activePlaylistId);
        if (pl) {
            std::vector<std::wstring> itemPaths = library.GetPlaylistPaths(*pl);
            const auto& visible = GetVisiblePlaylistIndices();
            float epItemH = 62.0f, scrollYDIP = playlistDetailScrollY / dp;
            int firstRow = std::max<int>(0, static_cast<int>(std::floor((scrollYDIP - epItemH) / epItemH)) - 4);
            int lastRow = std::min<int>(static_cast<int>(visible.size()), static_cast<int>(std::ceil((scrollYDIP + viewportHDIP) / epItemH)) + 6);
            for (int i = firstRow; i < lastRow && i < (int)visible.size(); ++i) {
                size_t idx = visible[i];
                if (idx < itemPaths.size()) { cachedVisibleVideoPaths.push_back(itemPaths[idx]); }
            }
        }
    }
}


bool App::HubExtractPathStillWanted(const std::wstring& path) {
    if (path.empty()) return false;
    for (const auto& visible : cachedVisibleVideoPaths) { if (visible == path) return true; }
    return false;
}


bool App::DriveHubExtraction() {
    // Two dedicated extractor instances run fully parallel to hover,
    // seekbar, and playback decoding; they only need the player closed so
    // background I/O never contends with active media.
    if (playbackSession.IsOpen()) return false;
    CollectVisibleVideoPaths();
    bool drew = false;
    const double nowS = NowSeconds();
    for (int i = 0; i < 2; ++i) {
        MpvHubExtractor& ex = hubExtractors[i];
        if (ex.IsActive()) {
            hubExtractorLastBusy[i] = nowS;
            ex.Pump();
            PreviewThumbnailFrame done;
            if (ex.TakeResult(done)) {
                if (!done.bgraPixels.empty() && renderer.InjectThumbnail(done.filePath, (UINT)done.width,
                                             (UINT)done.height, done.bgraPixels)) {
                    drew = true;
                }
                // Freed handle immediately takes the next candidate.
                std::wstring next;
                if (renderer.DequeueHubExtractCandidate(cachedVisibleVideoPaths, next) && IsHubExtractVideoPath(next))
                    ex.Begin(next, HubExtractStartSecForPath(next));
            } else {
                // User scrolled on while a now off-screen file extracts: yield this handle to something visible.
                const std::wstring stale = ex.GetPath();
                if (!stale.empty() && !HubExtractPathStillWanted(stale)) {
                    std::wstring better;
                    if (renderer.DequeueHubExtractCandidate(cachedVisibleVideoPaths, better) &&
                        better != stale && IsHubExtractVideoPath(better)) {
                        ex.Cancel();
                        ex.Begin(better, HubExtractStartSecForPath(better));
                    }
                }
            }
        } else {
            // Idle handle: visible-first, else oldest background fill.
            std::wstring candidate;
            if (renderer.DequeueHubExtractCandidate(cachedVisibleVideoPaths, candidate) && IsHubExtractVideoPath(candidate)) {
                hubExtractorLastBusy[i] = nowS;
                ex.Begin(candidate, HubExtractStartSecForPath(candidate));
            }
        }
    }
    // Ownership rule: an extractor that has neither run nor found work for
    // 10s releases its mpv handle (tens of MiB); Begin() rebuilds on demand.
    for (int i = 0; i < 2; ++i) {
        if (!hubExtractors[i].IsActive() && hubExtractors[i].IsInitialized() && nowS - hubExtractorLastBusy[i] > 10.0)
            hubExtractors[i].Shutdown();
    }
    return drew;
}


void App::MaybePrefetchVisibleThumbnails() {
    if (currentTab != TabView::Media && currentTab != TabView::Playlists) return;
    CollectVisibleVideoPaths();
    // Hash the visible set instead of concatenating a wstring signature, so
    // steady-state hub frames allocate nothing here.
    const size_t visibleCount = cachedVisibleVideoPaths.size();
    uint64_t sig = Fnv1a64(&visibleCount, sizeof(visibleCount));
    for (const auto& path : cachedVisibleVideoPaths) {
        sig ^= Fnv1a64(path.data(), path.size() * sizeof(wchar_t));
        sig *= 1099511628211ULL;
    }
    if (sig == lastPrefetchSig) return;
    lastPrefetchSig = sig;
    renderer.PrefetchThumbnails(cachedVisibleVideoPaths);
}

} // namespace Aperture
