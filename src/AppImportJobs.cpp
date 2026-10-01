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
    importTargetPlaylistId = targetPlaylistId;
    importCreatedPlaylistName = createdPlaylistName;
    importLaunchFirst = launchFirst;
    importCancelHovered = false;
    importJobActive = true;
    importWorker = std::thread([this, paths]() {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Lazy MF: hold for the whole import job so probes share one
        // startup; nested per-probe acquires only bump the refcount.
        MediaFoundation::ScopedAcquire mfJobGuard;
        try {
            // The worker owns this snapshot exclusively. It never reads or writes App::library, and persistence is deferred to the UI
            // thread after the worker has joined.
            MediaLibrary staged;
            auto progress = [this](size_t processed, size_t total, const std::wstring&) {
                importProcessed.store(processed, std::memory_order_release);
                importTotal.store(total, std::memory_order_release);
                return !importCancelRequested.load(std::memory_order_acquire);
            };
            MediaImportResult result = staged.ImportPaths(paths, progress, false);
            std::vector<MediaItem> acceptedItems;
            acceptedItems.reserve(result.acceptedPaths.size());
            for (const auto& path : result.acceptedPaths) { if (MediaItem* item = staged.FindByPath(path)) acceptedItems.push_back(*item); }
            importResult = std::move(result);
            stagedImportItems = std::move(acceptedItems);
        } catch (...) {
            importResult = {};
            importResult.outcome = importCancelRequested.load(std::memory_order_acquire) ? LongOperationOutcome::Canceled
                : LongOperationOutcome::Failed;
            if (importResult.outcome == LongOperationOutcome::Failed)
                importResult.status = StorageStatus::Error( StorageErrorCode::FileReadFailed, 0, L"", "Media import worker failed");
        }
        if (SUCCEEDED(coHr)) { CoUninitialize(); }
        importDone.store(true, std::memory_order_release);
    });
}


void App::PumpPendingDropImports() {
    if (importJobActive || pendingDropImports.empty()) return;
    PendingDropImport next = std::move(pendingDropImports.front());
    pendingDropImports.pop_front();
    StartImportJob(next.paths, next.targetPlaylistId, false, next.createdPlaylistName);
}


void App::FinishImportJob() {
    if (!importJobActive || !importDone.load(std::memory_order_acquire)) return;
    // Import completion updates library/UI state and is valid before the player is initialized. playerCallbacksDisabled is a callback
    // lifetime fence, not a general-purpose app readiness flag.
    if (shutdownComplete || importCancelRequested.load(std::memory_order_acquire)) {
        pendingDropImports.clear();
        return;
    }
    if (importWorker.joinable()) importWorker.join();
    const auto& acceptedPaths = importResult.acceptedPaths;
    auto acceptedItems = std::move(stagedImportItems);
    std::vector<MediaItem> newItems;
    newItems.reserve(acceptedItems.size());
    bool anyChanged = false;
    for (auto& item : acceptedItems) {
        MediaItem* existing = library.FindByPath(item.filePath);
        if (existing) {
            // Keep live progress/status and user metadata; refresh filesystem
            // facts from the isolated import snapshot only.
            if (existing->fileSizeBytes != item.fileSizeBytes || (item.durationSec > 0.0 && existing->durationSec != item.durationSec) ||
                existing->videoWidth != item.videoWidth || existing->videoHeight != item.videoHeight) {
                existing->fileSizeBytes = item.fileSizeBytes;
                existing->durationSec = item.durationSec > 0.0 ? item.durationSec : existing->durationSec;
                existing->videoWidth = item.videoWidth;
                existing->videoHeight = item.videoHeight;
                anyChanged = true;
            }
        } else { newItems.push_back(std::move(item)); }
    }
    if (!newItems.empty()) {
        library.mediaItems.insert(library.mediaItems.begin(), std::make_move_iterator(newItems.begin()),
                                  std::make_move_iterator(newItems.end()));
        anyChanged = true;
    }
    library.RebuildPathIndex();
    library.InvalidatePlaylistStatsCache();
    if (anyChanged) { library.NotifyPlaylistChanged(); }
    if (!importTargetPlaylistId.empty() && !acceptedPaths.empty()) {
        if (Playlist* playlist = library.FindPlaylist(importTargetPlaylistId)) {
            PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(playlist->itemPaths, acceptedPaths);
            library.NotifyPlaylistChanged();
        }
    }
    if (!acceptedPaths.empty() && !SaveLibrary()) { importResult.outcome = LongOperationOutcome::Failed; }
    const bool canceled = importResult.outcome == LongOperationOutcome::Canceled || importCancelRequested.load(std::memory_order_acquire);
    const bool failed = importResult.outcome == LongOperationOutcome::Failed;
    importJobActive = false;
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
    if (!shutdownComplete) { ScheduleMetadataBackfill(); }
    if (importLaunchFirst && !acceptedPaths.empty()) {
        if (acceptedPaths.size() > 1) {
            playbackSession.OpenQueue(acceptedPaths, 0);
        } else { playbackSession.OpenDirect(acceptedPaths.front(), 0.0); }
        OpenAndPlayMedia(acceptedPaths.front(), 0.0, acceptedPaths.size() > 1);
    }
    // Chained folder drops (one playlist per folder) start their next import only after the previous merge lands.
    PumpPendingDropImports();
    InvalidateRect(hWnd, NULL, FALSE);
}


void App::CancelImportJob() {
    if (!importJobActive && !importWorker.joinable()) return;
    importCancelRequested.store(true, std::memory_order_release);
    if (importWorker.joinable()) importWorker.join();
    importJobActive = false;
    importDone.store(false, std::memory_order_release);
    importCancelHovered = false;
    pendingDropImports.clear();
}


bool App::HasEligibleBackfillItems(const MetadataBackfillOptions& options) const {
    const int64_t now = options.nowTimestamp > 0 ? options.nowTimestamp : GetCurrentTimestampSec();
    for (const auto& item : library.mediaItems) {
        if (item.filePath.empty()) continue;
        const bool needsBackfill = (item.durationSec <= 0.0) || item.metadataProbeFailed;
        if (!needsBackfill) continue;
        if (options.forceRetry) return true;
        if (options.maxAttempts > 0 && item.probeAttemptCount >= options.maxAttempts) continue;
        if (item.lastProbeAttemptTimestamp > 0) {
            uint32_t attempts = item.probeAttemptCount > 0 ? item.probeAttemptCount : 1;
            int shift = std::min<int>((int)attempts - 1, 30);
            int64_t backoff = options.baseBackoffSec * (1LL << shift);
            if (backoff > options.maxBackoffSec || backoff < 0) { backoff = options.maxBackoffSec; }
            if (now >= item.lastProbeAttemptTimestamp + backoff) { return true; }
        } else { return true; }
    }
    return false;
}


void App::ScheduleMetadataBackfill(const MetadataBackfillOptions& options) {
    if (backfillJobActive) return;
    MetadataBackfillOptions effectiveOptions = options;
    if (effectiveOptions.maxItems == 0) { effectiveOptions.maxItems = 32; }
    if (!HasEligibleBackfillItems(effectiveOptions)) return;
    if (backfillWorker.joinable()) backfillWorker.join();
    backfillCancelRequested.store(false, std::memory_order_release);
    backfillDone.store(false, std::memory_order_release);
    backfillProcessed.store(0, std::memory_order_release);
    backfillTotal.store(0, std::memory_order_release);
    backfillResult = {};
    stagedBackfillItems.clear();
    backfillJobActive = true;
    // Copy at most maxItems eligible records, never the full library. The old path copied the entire mediaItems vector (strings, JSON,
    // bookmarks) on the UI thread and rebuilt a full index only to probe <=32 files — tens of MiB transient at 25k items. Eligibility
    // mirrors
    // MediaLibrary::BackfillMissingMetadata so the worker sees the same set.
    std::vector<MediaItem> snapshot;
    {
        const int64_t now = effectiveOptions.nowTimestamp > 0 ? effectiveOptions.nowTimestamp : GetCurrentTimestampSec();
        const size_t cap = effectiveOptions.maxItems > 0 ? effectiveOptions.maxItems : 32;
        snapshot.reserve(std::min<size_t>(cap, library.mediaItems.size()));
        for (const auto& item : library.mediaItems) {
            if (snapshot.size() >= cap) break;
            if (item.filePath.empty()) continue;
            const bool needsBackfill = (item.durationSec <= 0.0) || item.metadataProbeFailed;
            if (!needsBackfill) continue;
            bool canAttempt = false;
            if (effectiveOptions.forceRetry) {
                canAttempt = true;
            } else if (effectiveOptions.maxAttempts > 0 && item.probeAttemptCount >= effectiveOptions.maxAttempts) {
                continue;
            } else if (item.lastProbeAttemptTimestamp > 0) {
                uint32_t attempts = item.probeAttemptCount > 0 ? item.probeAttemptCount : 1;
                int shift = std::min<int>((int)attempts - 1, 30);
                int64_t backoff = effectiveOptions.baseBackoffSec * (1LL << shift);
                if (backoff > effectiveOptions.maxBackoffSec || backoff < 0) { backoff = effectiveOptions.maxBackoffSec; }
                if (now >= item.lastProbeAttemptTimestamp + backoff) { canAttempt = true; }
            } else { canAttempt = true; }
            if (canAttempt) { snapshot.push_back(item); }
        }
    }
    if (snapshot.empty()) {
        // Lost a race with HasEligibleBackfillItems (state changed between check and collect). Do not leave a dangling active job.
        backfillJobActive = false;
        return;
    }
    backfillWorker = std::thread([this, stagedSnapshot = std::move(snapshot), effectiveOptions]() mutable {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Lazy MF: hold for the whole backfill job; per-probe acquires nest.
        MediaFoundation::ScopedAcquire mfJobGuard;
        try {
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
            if (backfillResult.outcome == LongOperationOutcome::Failed)
                backfillResult.status = StorageStatus::Error( StorageErrorCode::FileReadFailed, 0, L"", "Metadata backfill worker failed");
        }
        if (SUCCEEDED(coHr)) { CoUninitialize(); }
        backfillDone.store(true, std::memory_order_release);
        if (hWnd) { PostMessageW(hWnd, kBackfillEventMessage, 0, 0); }
    });
}


void App::FinishBackfillJob() {
    if (!backfillJobActive || !backfillDone.load(std::memory_order_acquire)) return;
    // Metadata completion has the same lifecycle as import completion and must not depend on whether an mpv player exists yet.
    if (shutdownComplete || backfillCancelRequested.load(std::memory_order_acquire)) return;
    if (backfillWorker.joinable()) backfillWorker.join();
    const auto stagedItems = std::move(stagedBackfillItems);
    const auto result = backfillResult;
    stagedBackfillItems.clear();
    backfillJobActive = false;
    backfillDone.store(false, std::memory_order_release);
    bool anyChanged = false;
    for (const auto& stagedItem : stagedItems) {
        MediaItem* existing = library.FindByPath(stagedItem.filePath);
        if (!existing && !stagedItem.id.empty()) { existing = library.FindById(stagedItem.id); }
        if (existing) {
            if (existing->durationSec != stagedItem.durationSec || existing->videoWidth != stagedItem.videoWidth ||
                existing->videoHeight != stagedItem.videoHeight || existing->fileSizeBytes != stagedItem.fileSizeBytes ||
                existing->fileIndex != stagedItem.fileIndex || existing->volumeSerialNumber != stagedItem.volumeSerialNumber ||
                existing->metadataProbeFailed != stagedItem.metadataProbeFailed ||
                existing->probeAttemptCount != stagedItem.probeAttemptCount ||
                existing->lastProbeAttemptTimestamp != stagedItem.lastProbeAttemptTimestamp) {
                existing->durationSec = stagedItem.durationSec;
                existing->videoWidth = stagedItem.videoWidth;
                existing->videoHeight = stagedItem.videoHeight;
                existing->fileSizeBytes = stagedItem.fileSizeBytes;
                existing->fileIndex = stagedItem.fileIndex;
                existing->volumeSerialNumber = stagedItem.volumeSerialNumber;
                existing->metadataProbeFailed = stagedItem.metadataProbeFailed;
                existing->probeAttemptCount = stagedItem.probeAttemptCount;
                existing->lastProbeAttemptTimestamp = stagedItem.lastProbeAttemptTimestamp;
                if (existing->durationSec > 0.0 && existing->lastPosSec > existing->durationSec)
                    existing->lastPosSec = existing->durationSec;
                anyChanged = true;
            }
        }
    }
    if (anyChanged) {
        library.RebuildPathIndex();
        library.InvalidatePlaylistStatsCache();
        library.NotifyPlaylistChanged();
        InvalidateMediaLayoutCache();
        // A chained continuation saves on its own final batch; saving per
        // batch serialized the whole backfill into repeated full JSON writes.
        const bool chainsAnotherBatch = result.outcome == LongOperationOutcome::Completed &&
            !backfillCancelRequested.load(std::memory_order_acquire) && !shutdownComplete && HasEligibleBackfillItems();
        if (!chainsAnotherBatch) { SaveLibrary(); }
        if (hWnd) InvalidateRect(hWnd, NULL, FALSE);
        AnnounceStatus(L"Metadata backfill completed");
    }
    if (!result.status.is_ok()) { ReportStorageFailure(result.status, L"metadata backfill"); }
    if (result.outcome == LongOperationOutcome::Completed && !backfillCancelRequested.load(std::memory_order_acquire) && !shutdownComplete) {
        if (HasEligibleBackfillItems()) { ScheduleMetadataBackfill(); }
    }
}


void App::CancelBackfillJob() {
    if (!backfillJobActive && !backfillWorker.joinable()) return;
    backfillCancelRequested.store(true, std::memory_order_release);
    if (backfillWorker.joinable()) backfillWorker.join();
    backfillJobActive = false;
    backfillDone.store(false, std::memory_order_release);
}

} // namespace Aperture
