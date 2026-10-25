#include "App.h"
#include "AppInternals.h"
#include "AppLayout.h"
#include "CollectionAlgorithms.h"
#include "IpcProtocol.h"
#include "LibraryQuery.h"
#include "MediaProbeIsolation.h"
#include "MediaImportProbeCache.h"
#include "OnlineSubtitles.h"
#include "Reorder.h"
#include "Updater.h"
#include "ChannelPolicy.h"
#include "Localization.h"
#include "SettingsLayout.h"
#include "Presentation.h"
#include "PlaylistIO.h"
#include "WindowGeometry.h"
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <imm.h>
#include <numeric>

namespace Aperture {


void App::StartImportJob(const std::vector<std::wstring>& paths, const std::string& targetPlaylistId, bool launchFirst,
                         const std::wstring& createdPlaylistName) {
    if (paths.empty()) return;
    if (importJobActive) { ShowOSD(IconType::SettingsGear, L"An import is already in progress"); return; }
    if (importWorker.joinable()) importWorker.join();
    importCancelRequested.store(false, std::memory_order_release);
    importDone.store(false, std::memory_order_release);
    importProcessed.store(0, std::memory_order_release);
    importTotal.store(0, std::memory_order_release);
    importResult = {};
    stagedImportItems.clear();
    stagedImportBaselines.clear();
    importTargetPlaylistId = targetPlaylistId;
    importCreatedPlaylistName = createdPlaylistName;
    importLaunchFirst = launchFirst;
    importStartPlaybackGeneration = playbackRequestGeneration;
    importFirstPlayback.Reset(importStartPlaybackGeneration, launchFirst);
    importEarlyPlaybackPath.clear();
    importCancelHovered = false;
    importJobActive = true;
    try {
    auto metadataSnapshot = MediaImportProbeCache::Capture(library);
    importWorker = std::thread([this, paths, launchFirst, metadataSnapshot = std::move(metadataSnapshot)]() mutable {
        struct CompletionGuard {
            std::atomic<bool>& done;
            ~CompletionGuard() { done.store(true, std::memory_order_release); }
        } completed{ importDone };
        try {
            MediaImportProbeCache probeCache(std::move(metadataSnapshot));
            // Source readers live in a reusable helper process. Cancel interrupts
            // a stalled codec without terminating this App-owning C++ thread.
            ScopedMediaProbeIsolation probes([this]() { return importCancelRequested.load(std::memory_order_acquire); });
            // The worker owns this snapshot exclusively. It never reads or writes App::library, and persistence is deferred to the UI
            // thread after the worker has joined.
            MediaLibrary staged;
            bool firstPublished = false;
            auto progress = [this, launchFirst, &firstPublished](size_t processed, size_t total, const std::wstring& path) {
                importProcessed.store(processed, std::memory_order_release);
                importTotal.store(total, std::memory_order_release);
                // A nonempty path with a known total is from the sorted,
                // accepted probe pass, not unordered directory discovery.
                if (launchFirst && !firstPublished && total && !path.empty()) {
                    firstPublished = importFirstPlayback.Offer(processed, total, path);
                }
                return !importCancelRequested.load(std::memory_order_acquire);
            };
            MediaImportResult result = staged.ImportPaths(paths, progress, false, ImportConflictPolicy::KeepExisting, &probeCache);
            std::vector<MediaItem> acceptedItems;
            std::vector<std::optional<MetadataBackfillBaseline>> baselines;
            acceptedItems.reserve(result.acceptedPaths.size());
            baselines.reserve(result.acceptedPaths.size());
            for (const auto& path : result.acceptedPaths) if (MediaItem* item = staged.FindByPath(path)) {
                acceptedItems.push_back(std::move(*item));
                const auto* baseline = probeCache.Find(path);
                baselines.push_back(baseline ? std::optional<MetadataBackfillBaseline>(*baseline) : std::nullopt);
            }
            importResult = std::move(result);
            stagedImportItems = std::move(acceptedItems);
            stagedImportBaselines = std::move(baselines);
        } catch (...) {
            importResult = {};
            importResult.outcome = importCancelRequested.load(std::memory_order_acquire) ? LongOperationOutcome::Canceled
                : LongOperationOutcome::Failed;
            if (importResult.outcome == LongOperationOutcome::Failed) {
                // Error reporting itself must not throw out of the worker.
                importResult.status.success = false;
                importResult.status.code = StorageErrorCode::FileReadFailed;
                try { importResult.status.message = "Media import worker failed"; } catch (...) {}
            }
        }
        // CompletionGuard publishes after the helper's bounded teardown,
        // including constructor/allocation failures.
    }); } catch (...) {
        // A thread-construction failure must still have a completion for the
        // normal UI pump, rather than leaving an active job without an owner.
        importResult.outcome = LongOperationOutcome::Failed;
        importResult.status.success = false;
        importResult.status.code = StorageErrorCode::FileReadFailed;
        importResult.status.win32Error = ERROR_NOT_ENOUGH_MEMORY;
        importDone.store(true, std::memory_order_release);
    }
}


void App::PumpPendingDropImports() {
    if (importJobActive || pendingDropImports.empty()) return;
    PendingDropImport next = std::move(pendingDropImports.front());
    pendingDropImports.pop_front();
    StartImportJob(next.paths, next.targetPlaylistId, false, next.createdPlaylistName);
}

void App::PumpImportFirstPlayback() {
    if (!importJobActive || !importLaunchFirst || !libraryReady || closeRequested ||
        importDone.load(std::memory_order_acquire) || importCancelRequested.load(std::memory_order_acquire)) return;
    std::wstring path = importFirstPlayback.Take(playbackRequestGeneration);
    if (path.empty()) return;
    importEarlyPlaybackPath = path;
    importLaunchFirst = false;
    OpenAndPlayMedia(path, 0.0, false);
    importPlaybackGeneration = playbackRequestGeneration;
}


void App::FinishImportJob() {
    if (!importJobActive || !importDone.load(std::memory_order_acquire)) return;
    // Import completion updates library/UI state and is valid before the player is initialized. playerCallbacksDisabled is a callback
    // lifetime fence, not a general-purpose app readiness flag.
    if (shutdownComplete || importCancelRequested.load(std::memory_order_acquire)) {
        if (importWorker.joinable()) importWorker.join();
        stagedImportItems.clear();
        stagedImportBaselines.clear();
        importResult = {};
        importJobActive = false;
        importDone.store(false, std::memory_order_release);
        importCancelHovered = false;
        pendingDropImports.clear();
        if (hWnd && !shutdownComplete) InvalidateRect(hWnd, nullptr, FALSE);
        return;
    }
    if (importWorker.joinable()) importWorker.join();
    const auto& acceptedPaths = importResult.acceptedPaths;
    const auto merged = library.MergeImportedMedia(std::move(stagedImportItems), stagedImportBaselines);
    stagedImportItems.clear(); stagedImportBaselines.clear();
    bool playlistChanged = false;
    if (!importTargetPlaylistId.empty() && !acceptedPaths.empty()) {
        if (Playlist* playlist = library.FindPlaylist(importTargetPlaylistId)) {
            playlistChanged = PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(playlist->itemPaths, acceptedPaths) > 0;
            if (playlistChanged) library.NotifyPlaylistEdited();
        }
    }
    if ((merged.addedCount || merged.updatedCount || playlistChanged) && !SaveLibrary()) { importResult.outcome = LongOperationOutcome::Failed; }
    const bool canceled = importResult.outcome == LongOperationOutcome::Canceled || importCancelRequested.load(std::memory_order_acquire);
    const bool failed = importResult.outcome == LongOperationOutcome::Failed;
    importJobActive = false;
    importPopoverOpen = false;
    importPillHovered = false;
    importCancelHovered = false;
    importDone.store(false, std::memory_order_release);
    const size_t skippedUnsupported = importResult.skippedUnsupported, skippedMissing = importResult.skippedMissing;
    const size_t imported = acceptedPaths.size();
    if (failed) {
        std::wstring summary = imported == 0 ? L"Import failed — no files were added"
            : L"Import persistence failed — " + std::to_wstring(imported) + L" files are loaded only for this session";
        ShowOSD(IconType::VolumeMuted, summary);
    } else {
        std::wstring summary = canceled ? L"Import canceled"
            : (!importCreatedPlaylistName.empty() ? L"Created playlist \x201C" + importCreatedPlaylistName + L"\x201D" : L"Import complete");
        summary += L" — " + std::to_wstring(imported) + L" imported";
        if (skippedUnsupported > 0) { summary += L", " + std::to_wstring(skippedUnsupported) + L" unsupported"; }
        if (skippedMissing > 0) { summary += L", " + std::to_wstring(skippedMissing) + L" missing"; }
        ShowOSD(imported > 0 ? IconType::FolderPlaylist : IconType::VolumeMuted, summary);
    }
    if (!shutdownComplete && (merged.addedCount || merged.updatedCount)) ScheduleMetadataBackfill();
    if (importLaunchFirst && importStartPlaybackGeneration == playbackRequestGeneration && !acceptedPaths.empty()) {
        if (acceptedPaths.size() > 1) {
            playbackSession.OpenQueue(acceptedPaths, 0);
        } else { playbackSession.OpenDirect(acceptedPaths.front(), 0.0); }
        OpenAndPlayMedia(acceptedPaths.front(), 0.0, acceptedPaths.size() > 1);
    } else if (!canceled && !failed && !importEarlyPlaybackPath.empty() && acceptedPaths.size() > 1 &&
               importPlaybackGeneration == playbackRequestGeneration &&
               playbackSession.IsOpen() && !playbackSession.HasQueue() && player.GetCurrentFilePath() == importEarlyPlaybackPath &&
               acceptedPaths.front() == importEarlyPlaybackPath) {
        // Extend the queue without seeking/reopening the first movie. A user
        // who changed or closed playback while importing keeps that choice.
        playbackSession.SetQueue(acceptedPaths, 0);
    }
    importEarlyPlaybackPath.clear();
    if (startupPlayerPrepared && !pendingPlaybackOpen) {
        // A failed/canceled import may never consume the startup engine.
        player.Shutdown(); playerInitialized = false; playerInitAttempted = false; startupPlayerPrepared = false;
    }
    // Chained folder drops (one playlist per folder) start their next import only after the previous merge lands.
    PumpPendingDropImports();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::CancelImportJob() {
    importFirstPlayback.Cancel();
    if (!importJobActive && !importWorker.joinable()) return;
    importCancelRequested.store(true, std::memory_order_release);
    if (importWorker.joinable()) {
        // Interrupt a synchronous filesystem read as well. MF calls are
        // independently interrupted by the helper's cancellation poll.
        CancelSynchronousIo(importWorker.native_handle());
        if (importDone.load(std::memory_order_acquire)) {
            importWorker.join();
            importJobActive = false;
            importDone.store(false, std::memory_order_release);
            stagedImportItems.clear();
            stagedImportBaselines.clear();
            importResult = {};
        }
    } else { importJobActive = false; }
    importCancelHovered = false;
    pendingDropImports.clear();
}


void App::ScheduleMetadataBackfill(const MetadataBackfillOptions& options) {
    if (backfillJobActive) return;
    backfillOptions = options;
    if (backfillOptions.maxItems == 0) backfillOptions.maxItems = 32;
    backfillCursor.Reset();
    backfillLastSaveTick = GetTickCount64();
    StartNextMetadataBackfillBatch();
}


void App::StartNextMetadataBackfillBatch() {
    if (backfillJobActive || shutdownComplete || playerCallbacksDisabled.load(std::memory_order_acquire)) return;
    const MetadataBackfillOptions effectiveOptions = backfillOptions;
    std::vector<MediaItem> snapshot;
    try {
        snapshot = backfillCursor.Take(library, effectiveOptions);
        if (snapshot.empty()) return;
        backfillBaseline.clear();
        backfillBaseline.reserve(snapshot.size());
        for (const auto& item : snapshot) backfillBaseline.push_back(MetadataBackfillBaseline::Capture(item));
    } catch (...) {
        backfillCursor.Reset();
        backfillBaseline.clear();
        ReportStorageFailure(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_NOT_ENOUGH_MEMORY,
            L"", "Could not prepare metadata refresh"), L"metadata backfill");
        return;
    }
    if (backfillWorker.joinable()) backfillWorker.join();
    backfillCancelRequested.store(false, std::memory_order_release);
    backfillDone.store(false, std::memory_order_release);
    backfillProcessed.store(0, std::memory_order_release);
    backfillTotal.store(0, std::memory_order_release);
    backfillResult = {};
    stagedBackfillItems.clear();
    backfillJobActive = true;
    // The cursor copies at most one batch and visits each stable catalog row
    // once across the entire pass. The worker never reads the live library.
    try { backfillWorker = std::thread([this, stagedSnapshot = std::move(snapshot), effectiveOptions]() mutable {
        struct CompletionGuard {
            std::atomic<bool>& done;
            HWND window;
            ~CompletionGuard() {
                done.store(true, std::memory_order_release);
                if (window) PostMessageW(window, kBackfillEventMessage, 0, 0);
            }
        } completed{ backfillDone, hWnd };
        try {
            ScopedMediaProbeIsolation probes([this]() { return backfillCancelRequested.load(std::memory_order_acquire); });
            MediaLibrary staged;
            staged.mediaItems = std::move(stagedSnapshot);
            staged.RebuildPathIndex();
            auto progress = [this](size_t processed, size_t total, const std::wstring&) {
                backfillProcessed.store(processed, std::memory_order_release);
                backfillTotal.store(total, std::memory_order_release);
                return !backfillCancelRequested.load(std::memory_order_acquire);
            };
            MetadataBackfillResult result = staged.BackfillMissingMetadata(effectiveOptions, progress, false);
            backfillResult = std::move(result);
            stagedBackfillItems = std::move(staged.mediaItems);
        } catch (...) {
            backfillResult = {};
            backfillResult.outcome = backfillCancelRequested.load(std::memory_order_acquire) ? LongOperationOutcome::Canceled
                : LongOperationOutcome::Failed;
            if (backfillResult.outcome == LongOperationOutcome::Failed) {
                backfillResult.status.success = false;
                backfillResult.status.code = StorageErrorCode::FileReadFailed;
                try { backfillResult.status.message = "Metadata backfill worker failed"; } catch (...) {}
            }
        }
    }); } catch (...) {
        backfillResult.outcome = LongOperationOutcome::Failed;
        backfillResult.status.success = false;
        backfillResult.status.code = StorageErrorCode::FileReadFailed;
        backfillResult.status.win32Error = ERROR_NOT_ENOUGH_MEMORY;
        backfillDone.store(true, std::memory_order_release);
    }
}


void App::FinishBackfillJob() {
    if (!backfillJobActive || !backfillDone.load(std::memory_order_acquire)) return;
    // Metadata completion has the same lifecycle as import completion and must not depend on whether an mpv player exists yet.
    if (shutdownComplete || backfillCancelRequested.load(std::memory_order_acquire)) {
        if (backfillWorker.joinable()) backfillWorker.join();
        stagedBackfillItems.clear();
        backfillBaseline.clear();
        backfillResult = {};
        backfillJobActive = false;
        backfillDone.store(false, std::memory_order_release);
        return;
    }
    if (backfillWorker.joinable()) backfillWorker.join();
    const auto stagedItems = std::move(stagedBackfillItems);
    const auto result = backfillResult;
    stagedBackfillItems.clear();
    backfillJobActive = false;
    backfillDone.store(false, std::memory_order_release);
    const auto previousCatalog = library.GetRevisions().catalog;
    const auto merged = library.MergeMetadataBackfill(backfillBaseline, stagedItems);
    backfillBaseline.clear();
    backfillCursor.AcceptOwnChanges(previousCatalog, library.GetRevisions().catalog);
    backfillPassDirty |= merged.changedCount > 0;
    if (merged.viewChanged) {
        InvalidateMediaLayoutCache();
        if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
    }
    if (!result.status.is_ok()) { ReportStorageFailure(result.status, L"metadata backfill"); }
    if (result.outcome == LongOperationOutcome::Completed && !backfillCancelRequested.load(std::memory_order_acquire) && !shutdownComplete) {
        StartNextMetadataBackfillBatch();
    }
    // Checkpoint a long pass, and always save its final batch. Retry-only
    // changes are durable too, even though they do not rebuild any views.
    if (backfillPassDirty && (!backfillJobActive || GetTickCount64() - backfillLastSaveTick >= 5000)) {
        if (SaveLibrary()) { backfillPassDirty = false; backfillLastSaveTick = GetTickCount64(); }
    }
    if (!backfillJobActive && merged.changedCount) AnnounceStatus(L"Metadata backfill completed");
}


void App::CancelBackfillJob() {
    if (!backfillJobActive && !backfillWorker.joinable()) return;
    backfillCancelRequested.store(true, std::memory_order_release);
    if (backfillWorker.joinable()) {
        CancelSynchronousIo(backfillWorker.native_handle());
        if (backfillDone.load(std::memory_order_acquire)) {
            backfillWorker.join();
            backfillJobActive = false;
            backfillDone.store(false, std::memory_order_release);
            stagedBackfillItems.clear();
            backfillBaseline.clear();
            backfillResult = {};
        }
    } else { backfillJobActive = false; }
}

} // namespace Aperture
