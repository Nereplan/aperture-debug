#include "FolderWatcher.h"
#include <algorithm>
#include <iostream>

namespace Aperture {

FolderWatcher::FolderWatcher() = default;

FolderWatcher::~FolderWatcher() { Stop(); }

void FolderWatcher::SetCallback(WatchChangeCallback cb) { std::lock_guard<std::mutex> lock(coalesceMutex); callback = std::move(cb); }

bool FolderWatcher::Start() {
    if (isRunning.load()) return true;
    if (workerThread.joinable()) {
        if (!workerDone.load(std::memory_order_acquire)) return false;
        workerThread.join();
        if (hIOCP) { CloseHandle(hIOCP); hIOCP = NULL; }
    }
    stopRequested.store(false);
    nextInactiveSweep = 0;
    hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!hIOCP) { return false; }
    isRunning.store(true);
    workerDone.store(false, std::memory_order_release);
    try { workerThread = std::thread(&FolderWatcher::WorkerLoop, this); }
    catch (...) {
        isRunning.store(false); workerDone.store(true, std::memory_order_release);
        CloseHandle(hIOCP); hIOCP = NULL; return false;
    }
    return true;
}

uint64_t FolderWatcher::SetWatchedFoldersAsync(std::vector<std::wstring> folders) {
    uint64_t generation;
    const bool empty = folders.empty();
    {
        std::lock_guard<std::mutex> lock(configurationMutex);
        pendingConfiguration = std::move(folders);
        generation = requestedConfiguration.fetch_add(1) + 1;
    }
    // Do not create a worker for an initially empty profile.
    if (empty && !workerThread.joinable()) {
        // With no worker, these watches cannot own pending I/O. Retire an old
        // stopped configuration and consume the empty request immediately.
        { std::lock_guard<std::mutex> lock(watchMutex); watches.clear(); retiredWatches.clear(); }
        { std::lock_guard<std::mutex> lock(configurationMutex);
          pendingConfiguration.reset(); workerConfiguration = generation;
          appliedConfiguration.store(generation, std::memory_order_release); }
        return generation;
    }
    const bool wasRunning = isRunning.load();
    if (!Start()) return 0;
    // Interrupt a supported synchronous directory open for a superseded list.
    if (wasRunning && workerThread.joinable()) CancelSynchronousIo(workerThread.native_handle());
    if (hIOCP) PostQueuedCompletionStatus(hIOCP, 0, kStopCompletionKey, nullptr);
    return generation;
}

void FolderWatcher::RequestStop() {
    isRunning.store(false);
    stopRequested.store(true);
    if (workerThread.joinable()) CancelSynchronousIo(workerThread.native_handle());
    if (hIOCP) PostQueuedCompletionStatus(hIOCP, 0, kStopCompletionKey, nullptr);
}

void FolderWatcher::CancelDirectoryReads() {
    std::lock_guard<std::mutex> lock(watchMutex);
    const auto cancel = [](auto& watch) {
        if (watch && watch->hDirectory != INVALID_HANDLE_VALUE) {
            CancelIoEx(watch->hDirectory, &watch->overlapped);
            CloseHandle(watch->hDirectory);
            watch->hDirectory = INVALID_HANDLE_VALUE;
            watch->active = false; watch->retryAtTick = 0;
        }
    };
    for (auto& pair : watches) cancel(pair.second);
    for (auto& watch : retiredWatches) cancel(watch);
}

void FolderWatcher::Stop() {
    RequestStop();
    if (workerThread.joinable()) { workerThread.join(); }
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        retiredWatches.clear();
    }
    if (hIOCP) { CloseHandle(hIOCP); hIOCP = NULL; }
    // Flush any pending coalesced events
    FlushCoalescedEvents(true);
}

void FolderWatcher::ApplyPendingConfiguration() {
    std::optional<std::vector<std::wstring>> desired;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!pendingConfiguration) return;
        desired = std::move(pendingConfiguration); pendingConfiguration.reset();
        generation = requestedConfiguration.load();
    }
    std::unordered_map<std::wstring, std::wstring> roots;
    for (const auto& path : *desired) {
        if (stopRequested.load() || generation != requestedConfiguration.load()) return;
        if (path.empty()) continue;
        auto resolved = ResolveAppPath(path); auto key = NormalizePathKey(resolved);
        if (!key.empty()) roots.emplace(std::move(key), std::move(resolved));
    }
    std::lock_guard<std::mutex> lock(watchMutex);
    retiredWatches.reserve(retiredWatches.size() + watches.size());
    for (auto it = watches.begin(); it != watches.end();) {
        if (roots.count(it->first)) { ++it; continue; }
        auto& watch = it->second;
        watch->retired = true;
        if (watch->hDirectory != INVALID_HANDLE_VALUE) {
            CancelIoEx(watch->hDirectory, &watch->overlapped);
            CloseHandle(watch->hDirectory); watch->hDirectory = INVALID_HANDLE_VALUE; watch->active = false;
        }
        if (watch->pendingRead) retiredWatches.push_back(std::move(watch));
        it = watches.erase(it);
    }
    for (auto& root : roots) {
        auto found = watches.find(root.first);
        if (found != watches.end()) { found->second->retryAtTick = 0; continue; }
        auto watch = std::make_unique<DirectoryWatch>();
        watch->rootPath = std::move(root.second); watch->buffer.resize(kBufferSize);
        watches.emplace(std::move(root.first), std::move(watch));
    }
    workerConfiguration = generation;
    nextInactiveSweep = 0;
}

void FolderWatcher::ArmInactiveWatches() {
    if (GetTickCount64() < nextInactiveSweep) return;
    std::lock_guard<std::mutex> lock(watchMutex);
    for (auto& pair : watches) {
        if (stopRequested.load() || workerConfiguration != requestedConfiguration.load()) return;
        auto& watch = *pair.second;
        if (watch.active || watch.pendingRead || GetTickCount64() < watch.retryAtTick) continue;
        watch.retryAtTick = GetTickCount64() + 2000;
        if (ArmReadDirectoryChanges(&watch)) TriggerRescan(watch.rootPath);
    }
    nextInactiveSweep = GetTickCount64() + 2000;
    if (!stopRequested.load()) appliedConfiguration.store(workerConfiguration, std::memory_order_release);
}

bool FolderWatcher::AddWatch(const std::wstring& folderPath) {
    if (folderPath.empty()) return false;
    std::wstring resolved = ResolveAppPath(folderPath);
    std::wstring key = NormalizePathKey(resolved);
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(watchMutex);
    auto it = watches.find(key);
    if (it != watches.end()) {
        if (!it->second->active && isRunning.load()) { ArmReadDirectoryChanges(it->second.get()); }
        return true;
    }
    auto watch = std::make_unique<DirectoryWatch>();
    watch->rootPath = resolved;
    watch->buffer.resize(kBufferSize);
    DirectoryWatch* pWatch = watch.get();
    watches.emplace(key, std::move(watch));
    if (isRunning.load() && !ArmReadDirectoryChanges(pWatch)) {
        nextInactiveSweep = 0;
        WakeWorker();
    }
    return true;
}

std::vector<std::wstring> FolderWatcher::GetWatchedFolders() const {
    std::lock_guard<std::mutex> lock(watchMutex);
    std::vector<std::wstring> result;
    result.reserve(watches.size());
    for (const auto& pair : watches) { result.push_back(pair.second->rootPath); }
    return result;
}

size_t FolderWatcher::GetRetiredWatchCount() const {
    std::lock_guard<std::mutex> lock(watchMutex); return retiredWatches.size();
}
bool FolderWatcher::HasPendingReadsLocked() const {
    for (const auto& pair : watches) if (pair.second->pendingRead) return true;
    for (const auto& watch : retiredWatches) if (watch->pendingRead) return true;
    return false;
}
void FolderWatcher::ReclaimRetiredWatchLocked(DirectoryWatch* watch) {
    const auto it = std::find_if(retiredWatches.begin(), retiredWatches.end(),
        [watch](const auto& entry) { return entry.get() == watch; });
    if (it != retiredWatches.end()) retiredWatches.erase(it);
}
bool FolderWatcher::ArmReadDirectoryChanges(DirectoryWatch* dirWatch) {
    if (!dirWatch || dirWatch->retired || dirWatch->pendingRead || !hIOCP || stopRequested.load()) return false;
    if (dirWatch->hDirectory == INVALID_HANDLE_VALUE) {
        // Attempt to open directory handle
        DWORD attr = GetFileAttributesW(dirWatch->rootPath.c_str());
        if (stopRequested.load()) return false;
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            // Missing/unavailable root - do not block or crash startup; leave inactive
            dirWatch->active = false;
            return false;
        }
        dirWatch->hDirectory = CreateFileW( dirWatch->rootPath.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL );
        if (dirWatch->hDirectory == INVALID_HANDLE_VALUE) { dirWatch->active = false; return false; }
        if (stopRequested.load()) {
            CloseHandle(dirWatch->hDirectory); dirWatch->hDirectory = INVALID_HANDLE_VALUE; return false;
        }
        if (CreateIoCompletionPort(dirWatch->hDirectory, hIOCP, reinterpret_cast<ULONG_PTR>(dirWatch), 0) == NULL) {
            CloseHandle(dirWatch->hDirectory);
            dirWatch->hDirectory = INVALID_HANDLE_VALUE;
            dirWatch->active = false;
            return false;
        }
    }
    ZeroMemory(&dirWatch->overlapped, sizeof(dirWatch->overlapped));
    const DWORD notifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;
    BOOL success = ReadDirectoryChangesW( dirWatch->hDirectory, dirWatch->buffer.data(), static_cast<DWORD>(dirWatch->buffer.size()),
        TRUE, // recursive
        notifyFilter, NULL, &dirWatch->overlapped, NULL );
    if (!success) {
        DWORD err = GetLastError();
        if (err == ERROR_NOTIFY_ENUM_DIR) {
            // Buffer overflow during arming
            TriggerRescan(dirWatch->rootPath);
        }
        // No read was queued. Reopen on the next retry rather than retaining
        // an unusable handle after a drive or network share reconnects.
        CloseHandle(dirWatch->hDirectory);
        dirWatch->hDirectory = INVALID_HANDLE_VALUE;
        dirWatch->active = false;
        return false;
    }
    dirWatch->active = true;
    dirWatch->pendingRead = true;
    return true;
}

void FolderWatcher::WakeWorker() {
    if (hIOCP && isRunning.load()) PostQueuedCompletionStatus(hIOCP, 0, kWakeCompletionKey, nullptr);
}

DWORD FolderWatcher::ComputeWaitTimeoutMs() {
    if (stopRequested.load() || requestedConfiguration.load() != workerConfiguration) return 0;
    DWORD timeoutMs = INFINITE;
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        if (rescanPending) return 0;
        if (!pendingCoalesce.empty()) {
            if (pendingCoalesce.size() >= maxBatchSize) return 0;
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - oldestPendingTime).count();
            if (elapsedMs >= static_cast<long long>(coalesceWindowMs)) return 0;
            timeoutMs = static_cast<DWORD>(std::max<long long>(1, static_cast<long long>(coalesceWindowMs) - elapsedMs));
        }
    }
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        const ULONGLONG now = GetTickCount64();
        for (const auto& pair : watches) {
            const auto& watch = *pair.second;
            if (watch.active || watch.pendingRead || watch.retired) continue;
            const ULONGLONG retryAt = std::max(nextInactiveSweep, watch.retryAtTick);
            const ULONGLONG remaining = retryAt > now ? (retryAt - now) : 0;
            const DWORD clamped = remaining > INFINITE - 1 ? (INFINITE - 1) : static_cast<DWORD>(remaining);
            timeoutMs = std::min(timeoutMs, clamped);
        }
    }
    return timeoutMs;
}

void FolderWatcher::WorkerLoop() {
    workerThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    struct Completion {
        FolderWatcher& owner;
        ~Completion() {
            owner.workerThreadId.store(0, std::memory_order_release);
            owner.isRunning.store(false);
            owner.workerDone.store(true, std::memory_order_release);
        }
    } completion{*this};
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
      try {
        if (stopRequested.load()) {
            CancelDirectoryReads();
            std::lock_guard<std::mutex> lock(watchMutex);
            if (!HasPendingReadsLocked()) break;
        } else { ApplyPendingConfiguration(); ArmInactiveWatches(); }
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED pOverlapped = nullptr;
        const DWORD waitTimeoutMs = ComputeWaitTimeoutMs();
        BOOL ok = GetQueuedCompletionStatus( hIOCP, &bytesTransferred, &completionKey, &pOverlapped, waitTimeoutMs );
        if (!ok) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                // Timeout elapsed; check and flush any coalesced events
                FlushCoalescedEvents(false);
                continue;
            }
            if (completionKey == kStopCompletionKey) continue;
            if (completionKey == kWakeCompletionKey) { FlushCoalescedEvents(false); continue; }
            if (completionKey != 0 && pOverlapped) {
                DirectoryWatch* dirWatch = reinterpret_cast<DirectoryWatch*>(completionKey);
                std::lock_guard<std::mutex> lock(watchMutex);
                dirWatch->pendingRead = false;
                dirWatch->active = false;
                if (dirWatch->retired) {
                    ReclaimRetiredWatchLocked(dirWatch);
                } else {
                    if (err == ERROR_NOTIFY_ENUM_DIR || err == ERROR_MORE_DATA) {
                        // Buffer overflow: trigger full rescan for this watched root
                        TriggerRescan(dirWatch->rootPath);
                        if (isRunning.load() && !stopRequested.load()) { ArmReadDirectoryChanges(dirWatch); }
                    } else if (err == ERROR_OPERATION_ABORTED) {
                        // Watch was cancelled or stopped
                    } else {
                        // Other error; attempt to re-arm
                        if (isRunning.load() && !stopRequested.load()) { ArmReadDirectoryChanges(dirWatch); }
                    }
                }
            }
            FlushCoalescedEvents(false);
            continue;
        }
        if (completionKey == kStopCompletionKey) continue;
        if (completionKey == kWakeCompletionKey) { FlushCoalescedEvents(false); continue; }
        if (completionKey != 0 && pOverlapped) {
            DirectoryWatch* dirWatch = reinterpret_cast<DirectoryWatch*>(completionKey);
            std::lock_guard<std::mutex> lock(watchMutex);
            dirWatch->pendingRead = false;
            dirWatch->active = false;
            if (dirWatch->retired) {
                ReclaimRetiredWatchLocked(dirWatch);
            } else {
                if (bytesTransferred == 0) {
                    // Zero bytes on IOCP notification indicates notification buffer overflow
                    TriggerRescan(dirWatch->rootPath);
                } else { ProcessNotificationBuffer(dirWatch, bytesTransferred); }
                // Re-arm for subsequent directory changes
                if (isRunning.load() && !stopRequested.load()) { ArmReadDirectoryChanges(dirWatch); }
            }
        }
        FlushCoalescedEvents(false);
      } catch (...) {
        // Stop accepting work but drain cancellation completions before their
        // OVERLAPPED storage can be released by Stop/destruction.
        isRunning.store(false); stopRequested.store(true);
      }
    }
}

void FolderWatcher::ProcessNotificationBuffer(DirectoryWatch* dirWatch, DWORD bytesTransferred) {
    if (!dirWatch || bytesTransferred == 0) return;
    BYTE* ptr = dirWatch->buffer.data();
    std::wstring pendingOldRename;
    while (ptr < dirWatch->buffer.data() + bytesTransferred) {
        PFILE_NOTIFY_INFORMATION pNotify = reinterpret_cast<PFILE_NOTIFY_INFORMATION>(ptr);
        std::wstring relName(pNotify->FileName, pNotify->FileNameLength / sizeof(wchar_t));
        std::wstring fullPath = dirWatch->rootPath;
        if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') { fullPath += L'\\'; }
        fullPath += relName;
        WatchChangeItem item;
        item.rootFolder = dirWatch->rootPath;
        item.path = fullPath;
        switch (pNotify->Action) {
        case FILE_ACTION_ADDED:
            item.action = WatchAction::Added;
            EnqueueEvent(item);
            break;
        case FILE_ACTION_REMOVED:
            item.action = WatchAction::Removed;
            EnqueueEvent(item);
            break;
        case FILE_ACTION_MODIFIED:
            item.action = WatchAction::Modified;
            EnqueueEvent(item);
            break;
        case FILE_ACTION_RENAMED_OLD_NAME: pendingOldRename = fullPath; break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            item.action = WatchAction::Renamed;
            item.oldPath = pendingOldRename;
            pendingOldRename.clear();
            EnqueueEvent(item);
            break;
        default:
            item.action = WatchAction::Modified;
            EnqueueEvent(item);
            break;
        }
        if (pNotify->NextEntryOffset == 0) break;
        ptr += pNotify->NextEntryOffset;
    }
    if (!pendingOldRename.empty()) {
        // If we had an OLD_NAME without a paired NEW_NAME, treat as Removed
        WatchChangeItem item;
        item.action = WatchAction::Removed;
        item.path = pendingOldRename;
        item.rootFolder = dirWatch->rootPath;
        EnqueueEvent(item);
    }
}

void FolderWatcher::EnqueueEvent(const WatchChangeItem& item) {
    bool shouldFlush = false;
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        const auto now = std::chrono::steady_clock::now();
        if (pendingCoalesce.empty()) { oldestPendingTime = now; }
        std::wstring key = NormalizePathKey(item.path);
        auto it = pendingCoalesce.find(key);
        if (it == pendingCoalesce.end()) {
            CoalesceEntry entry;
            entry.action = item.action;
            entry.path = item.path;
            entry.oldPath = item.oldPath;
            entry.rootFolder = item.rootFolder;
            entry.firstSeen = now;
            entry.lastSeen = now;
            pendingCoalesce.emplace(key, std::move(entry));
        } else {
            // Coalesce / deduplicate transitions
            it->second.lastSeen = now;
            if (it->second.action == WatchAction::Added) {
                if (item.action == WatchAction::Removed) {
                    // Added then Removed within window -> drop completely
                    pendingCoalesce.erase(it);
                    return;
                } else if (item.action == WatchAction::Modified) {
                    // Added then Modified -> remains Added
                } else if (item.action == WatchAction::Renamed) {
                    // Added then Renamed -> Added at new path
                    CoalesceEntry newEntry = it->second;
                    newEntry.path = item.path;
                    pendingCoalesce.erase(it);
                    pendingCoalesce.emplace(NormalizePathKey(item.path), std::move(newEntry));
                    return;
                }
            } else if (it->second.action == WatchAction::Removed) {
                if (item.action == WatchAction::Added) {
                    // Removed then Added -> Modified
                    it->second.action = WatchAction::Modified;
                }
            } else if (it->second.action == WatchAction::Modified) {
                if (item.action == WatchAction::Removed) {
                    it->second.action = WatchAction::Removed;
                } else if (item.action == WatchAction::Renamed) {
                    it->second.action = WatchAction::Renamed;
                    it->second.oldPath = item.oldPath;
                    it->second.path = item.path;
                }
            } else if (it->second.action == WatchAction::Renamed) {
                if (item.action == WatchAction::Removed) {
                    it->second.action = WatchAction::Removed;
                } else if (item.action == WatchAction::Modified) {
                    // Renamed then Modified -> remains Renamed
                }
            }
        }
        if (pendingCoalesce.size() >= maxBatchSize) { shouldFlush = true; }
    }
    if (shouldFlush) { FlushCoalescedEvents(true); }
    else if (GetCurrentThreadId() != workerThreadId.load(std::memory_order_acquire)) { WakeWorker(); }
}

void FolderWatcher::FlushCoalescedEvents(bool forceAll) {
    WatchChangeCallback cb;
    std::vector<WatchChangeBatch> batchesToDispatch;
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        if (!callback) return;
        cb = callback;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - oldestPendingTime).count();
        // Check if any rescan is pending
        if (rescanPending) {
            for (const auto& root : rescanRoots) {
                WatchChangeBatch b;
                b.overflowOrRescan = true;
                b.rootFolder = root;
                batchesToDispatch.push_back(std::move(b));
            }
            rescanRoots.clear();
            rescanPending = false;
        }
        if (pendingCoalesce.empty()) {
            // Nothing more to flush
        } else if (forceAll || elapsedMs >= coalesceWindowMs || pendingCoalesce.size() >= maxBatchSize) {
            // Group items into batches by root folder or single batch
            std::unordered_map<std::wstring, WatchChangeBatch> batchMap;
            for (auto& pair : pendingCoalesce) {
                WatchChangeItem item;
                item.action = pair.second.action;
                item.path = pair.second.path;
                item.oldPath = pair.second.oldPath;
                item.rootFolder = pair.second.rootFolder;
                std::wstring rootKey = NormalizePathKey(item.rootFolder);
                auto& b = batchMap[rootKey];
                b.rootFolder = item.rootFolder;
                b.items.push_back(std::move(item));
            }
            for (auto& pair : batchMap) { batchesToDispatch.push_back(std::move(pair.second)); }
            pendingCoalesce.clear();
        }
    }
    if (cb) {
        for (const auto& batch : batchesToDispatch) { cb(batch); }
    }
}

void FolderWatcher::TriggerRescan(const std::wstring& rootFolder) {
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        rescanPending = true;
        rescanRoots.insert(rootFolder);
    }
    if (GetCurrentThreadId() != workerThreadId.load(std::memory_order_acquire)) WakeWorker();
}

} // namespace Aperture
