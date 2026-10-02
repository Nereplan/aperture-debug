#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

namespace Aperture {

enum class WatchAction { Added, Removed, Modified, Renamed, RescanRequired };

struct WatchChangeItem {
    WatchAction action = WatchAction::Modified;
    std::wstring path;       // Target/new path
    std::wstring oldPath;    // Previous path for Renamed
    std::wstring rootFolder; // Watched root directory
};

struct WatchChangeBatch { bool overflowOrRescan = false; std::wstring rootFolder; std::vector<WatchChangeItem> items; };

using WatchChangeCallback = std::function<void(const WatchChangeBatch& batch)>;

class FolderWatcher {
public:
    FolderWatcher();
    ~FolderWatcher();
    // Configure the callback invoked when change batches are ready (invoked on worker thread)
    void SetCallback(WatchChangeCallback cb);
    // Lifecycle control
    bool Start();
    void Stop();
    bool IsRunning() const { return isRunning.load(); }
    // Dynamic watch list management
    bool AddWatch(const std::wstring& folderPath);
    bool RemoveWatch(const std::wstring& folderPath);
    void ClearWatches();
    std::vector<std::wstring> GetWatchedFolders() const;
    size_t GetRetiredWatchCount() const;
    // Coalescing configuration
    void SetCoalesceWindowMs(uint32_t ms) { coalesceWindowMs = ms; }
    void SetMaxBatchSize(size_t maxItems) { maxBatchSize = maxItems; }
    // Manual / testing triggers
    void TriggerRescan(const std::wstring& rootFolder);
    void EnqueueEvent(const WatchChangeItem& item);
    void FlushCoalescedEvents(bool forceAll = false);

private:
    struct DirectoryWatch {
        std::wstring rootPath;
        HANDLE hDirectory = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped = {};
        std::vector<BYTE> buffer;
        bool active = false, retired = false, pendingRead = false;
    };
    bool ArmReadDirectoryChanges(DirectoryWatch* dirWatch);
    void ProcessNotificationBuffer(DirectoryWatch* dirWatch, DWORD bytesTransferred);
    void WorkerLoop();
    void ReclaimRetiredWatchLocked(DirectoryWatch* watch);
    bool HasPendingReadsLocked() const;
    mutable std::mutex watchMutex;
    std::unordered_map<std::wstring, std::unique_ptr<DirectoryWatch>> watches;
    // Cancelled OVERLAPPED completions retain their completion-key pointer.
    // Keep removed watches alive only until their last completion is consumed.
    std::vector<std::unique_ptr<DirectoryWatch>> retiredWatches;
    std::atomic<bool> isRunning{ false };
    std::atomic<bool> stopRequested{ false };
    HANDLE hIOCP = NULL;
    std::thread workerThread;
    WatchChangeCallback callback;
    uint32_t coalesceWindowMs = 200;
    size_t maxBatchSize = 256;
    struct CoalesceEntry {
        WatchAction action;
        std::wstring path, oldPath, rootFolder;
        std::chrono::steady_clock::time_point firstSeen;
        std::chrono::steady_clock::time_point lastSeen;
    };
    std::mutex coalesceMutex;
    std::unordered_map<std::wstring, CoalesceEntry> pendingCoalesce;
    std::chrono::steady_clock::time_point oldestPendingTime;
    bool rescanPending = false;
    std::unordered_set<std::wstring> rescanRoots;
    static constexpr ULONG_PTR kStopCompletionKey = 0xDEADBEEF;
    static constexpr DWORD kBufferSize = 64 * 1024; // 64 KB buffer for change notifications
};

} // namespace Aperture
