#include "FolderWatcher.h"
#include <algorithm>
#include <iostream>

namespace Aperture {

FolderWatcher::FolderWatcher() = default;

FolderWatcher::~FolderWatcher() { Stop(); }

void FolderWatcher::SetCallback(WatchChangeCallback cb) { std::lock_guard<std::mutex> lock(coalesceMutex); callback = std::move(cb); }

bool FolderWatcher::Start() {
    if (isRunning.load()) return true;
    stopRequested.store(false);
    hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!hIOCP) { return false; }
    isRunning.store(true);
    workerThread = std::thread(&FolderWatcher::WorkerLoop, this);
    // Arm any existing watches
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (auto& pair : watches) {
            if (pair.second && !pair.second->active) { ArmReadDirectoryChanges(pair.second.get()); }
        }
    }
    return true;
}

void FolderWatcher::Stop() {
    if (!isRunning.exchange(false)) { return; }
    stopRequested.store(true);
    if (hIOCP) { PostQueuedCompletionStatus(hIOCP, 0, kStopCompletionKey, NULL); }
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (auto& pair : watches) {
            if (pair.second && pair.second->hDirectory != INVALID_HANDLE_VALUE) {
                CancelIoEx(pair.second->hDirectory, &pair.second->overlapped);
                CloseHandle(pair.second->hDirectory);
                pair.second->hDirectory = INVALID_HANDLE_VALUE;
                pair.second->active = false;
            }
        }
    }
    if (workerThread.joinable()) { workerThread.join(); }
    { std::lock_guard<std::mutex> lock(watchMutex); retiredWatches.clear(); }
    if (hIOCP) { CloseHandle(hIOCP); hIOCP = NULL; }
    // Flush any pending coalesced events
    FlushCoalescedEvents(true);
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
    if (isRunning.load()) { ArmReadDirectoryChanges(pWatch); }
    return true;
}

bool FolderWatcher::RemoveWatch(const std::wstring& folderPath) {
    if (folderPath.empty()) return false;
    std::wstring resolved = ResolveAppPath(folderPath);
    std::wstring key = NormalizePathKey(resolved);
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(watchMutex);
    auto it = watches.find(key);
    if (it == watches.end()) return false;
    if (it->second->hDirectory != INVALID_HANDLE_VALUE) {
        it->second->retired = true;
        CancelIoEx(it->second->hDirectory, &it->second->overlapped);
        CloseHandle(it->second->hDirectory);
        it->second->hDirectory = INVALID_HANDLE_VALUE;
        it->second->active = false;
    }
    if (isRunning.load()) { retiredWatches.push_back(std::move(it->second)); }
    watches.erase(it);
    return true;
}

void FolderWatcher::ClearWatches() {
    std::lock_guard<std::mutex> lock(watchMutex);
    for (auto& pair : watches) {
        if (!pair.second) continue;
        if (pair.second->hDirectory != INVALID_HANDLE_VALUE) {
            pair.second->retired = true;
            CancelIoEx(pair.second->hDirectory, &pair.second->overlapped);
            CloseHandle(pair.second->hDirectory);
            pair.second->hDirectory = INVALID_HANDLE_VALUE;
            pair.second->active = false;
        }
        if (isRunning.load()) { retiredWatches.push_back(std::move(pair.second)); }
    }
    watches.clear();
}

std::vector<std::wstring> FolderWatcher::GetWatchedFolders() const {
    std::lock_guard<std::mutex> lock(watchMutex);
    std::vector<std::wstring> result;
    result.reserve(watches.size());
    for (const auto& pair : watches) { result.push_back(pair.second->rootPath); }
    return result;
}

bool FolderWatcher::ArmReadDirectoryChanges(DirectoryWatch* dirWatch) {
    if (!dirWatch || dirWatch->retired || !hIOCP) return false;
    if (dirWatch->hDirectory == INVALID_HANDLE_VALUE) {
        // Attempt to open directory handle
        DWORD attr = GetFileAttributesW(dirWatch->rootPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            // Missing/unavailable root - do not block or crash startup; leave inactive
            dirWatch->active = false;
            return false;
        }
        dirWatch->hDirectory = CreateFileW( dirWatch->rootPath.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL );
        if (dirWatch->hDirectory == INVALID_HANDLE_VALUE) { dirWatch->active = false; return false; }
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
        dirWatch->active = false;
        return false;
    }
    dirWatch->active = true;
    return true;
}

void FolderWatcher::WorkerLoop() {
    std::wstring pendingOldRename, pendingOldRenameRoot;
    while (isRunning.load() && !stopRequested.load()) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED pOverlapped = nullptr;
        const DWORD waitTimeoutMs = 50; // 50ms wait to allow timely event coalescing flush
        BOOL ok = GetQueuedCompletionStatus( hIOCP, &bytesTransferred, &completionKey, &pOverlapped, waitTimeoutMs );
        if (stopRequested.load()) { break; }
        if (!ok) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                // Timeout elapsed; check and flush any coalesced events
                FlushCoalescedEvents(false);
                continue;
            }
            if (completionKey == kStopCompletionKey) { break; }
            if (completionKey != 0) {
                DirectoryWatch* dirWatch = reinterpret_cast<DirectoryWatch*>(completionKey);
                std::lock_guard<std::mutex> lock(watchMutex);
                if (!dirWatch->retired) {
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
        if (completionKey == kStopCompletionKey) { break; }
        if (completionKey != 0 && pOverlapped) {
            DirectoryWatch* dirWatch = reinterpret_cast<DirectoryWatch*>(completionKey);
            std::lock_guard<std::mutex> lock(watchMutex);
            if (!dirWatch->retired) {
                if (bytesTransferred == 0) {
                    // Zero bytes on IOCP notification indicates notification buffer overflow
                    TriggerRescan(dirWatch->rootPath);
                } else { ProcessNotificationBuffer(dirWatch, bytesTransferred); }
                // Re-arm for subsequent directory changes
                if (isRunning.load() && !stopRequested.load()) { ArmReadDirectoryChanges(dirWatch); }
            }
        }
        FlushCoalescedEvents(false);
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
    std::lock_guard<std::mutex> lock(coalesceMutex);
    rescanPending = true;
    rescanRoots.insert(rootFolder);
}

} // namespace Aperture
