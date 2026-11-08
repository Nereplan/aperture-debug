#if !defined(_WIN32)

#include "../FolderWatcher.h"
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace Aperture {

FolderWatcher::FolderWatcher() = default;

FolderWatcher::~FolderWatcher() {
    Stop();
}

void FolderWatcher::SetCallback(WatchChangeCallback cb) {
    std::lock_guard<std::mutex> lock(coalesceMutex);
    callback = std::move(cb);
}

bool FolderWatcher::Start() {
    if (isRunning.load()) return true;
    int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return false;
    hIOCP = reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
    stopRequested.store(false);
    workerDone.store(false, std::memory_order_release);
    isRunning.store(true);
    workerThread = std::thread(&FolderWatcher::WorkerLoop, this);
    return true;
}

void FolderWatcher::RequestStop() {
    if (!isRunning.load() && workerDone.load(std::memory_order_acquire)) return;
    stopRequested.store(true);
    isRunning.store(false);
}

void FolderWatcher::Stop() {
    RequestStop();
    if (workerThread.joinable()) {
        workerThread.join();
    }
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    if (fd > 0) {
        ::close(fd);
    }
    hIOCP = NULL;
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        watches.clear();
        retiredWatches.clear();
    }
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        pendingCoalesce.clear();
        rescanPending = false;
        rescanRoots.clear();
    }
}

uint64_t FolderWatcher::SetWatchedFoldersAsync(std::vector<std::wstring> folders) {
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(configurationMutex);
        pendingConfiguration = std::move(folders);
        gen = requestedConfiguration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    if (!isRunning.load()) {
        ApplyPendingConfiguration();
    }
    return gen;
}

bool FolderWatcher::AddWatch(const std::wstring& folderPath) {
    if (folderPath.empty()) return false;
    std::wstring normKey = NormalizePathKey(folderPath);
    std::lock_guard<std::mutex> lock(watchMutex);
    if (watches.find(normKey) != watches.end()) return true;
    auto dw = std::make_unique<DirectoryWatch>();
    dw->rootPath = folderPath;
    if (isRunning.load()) {
        ArmReadDirectoryChanges(dw.get());
    }
    watches[normKey] = std::move(dw);
    return true;
}

std::vector<std::wstring> FolderWatcher::GetWatchedFolders() const {
    std::lock_guard<std::mutex> lock(watchMutex);
    std::vector<std::wstring> out;
    out.reserve(watches.size());
    for (const auto& [k, v] : watches) {
        out.push_back(v->rootPath);
    }
    return out;
}

size_t FolderWatcher::GetRetiredWatchCount() const {
    std::lock_guard<std::mutex> lock(watchMutex);
    return retiredWatches.size();
}

void FolderWatcher::TriggerRescan(const std::wstring& rootFolder) {
    std::lock_guard<std::mutex> lock(coalesceMutex);
    rescanPending = true;
    rescanRoots.insert(rootFolder);
    if (pendingCoalesce.empty()) {
        oldestPendingTime = std::chrono::steady_clock::now();
    }
}

void FolderWatcher::EnqueueEvent(const WatchChangeItem& item) {
    if (item.path.empty()) return;
    std::wstring key = NormalizePathKey(item.path);
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(coalesceMutex);
    if (pendingCoalesce.empty() && !rescanPending) {
        oldestPendingTime = now;
    }

    if (item.action == WatchAction::Renamed && !item.oldPath.empty()) {
        std::wstring oldKey = NormalizePathKey(item.oldPath);
        auto oldIt = pendingCoalesce.find(oldKey);
        if (oldIt != pendingCoalesce.end()) {
            WatchAction prevAction = oldIt->second.action;
            std::wstring origOldPath = oldIt->second.oldPath;
            pendingCoalesce.erase(oldIt);
            if (prevAction == WatchAction::Added) {
                pendingCoalesce[key] = {WatchAction::Added, item.path, L"", item.rootFolder, now, now};
                return;
            }
            if (prevAction == WatchAction::Renamed && !origOldPath.empty()) {
                pendingCoalesce[key] = {WatchAction::Renamed, item.path, origOldPath, item.rootFolder, now, now};
                return;
            }
        }
    }

    auto it = pendingCoalesce.find(key);
    if (it == pendingCoalesce.end()) {
        pendingCoalesce[key] = {item.action, item.path, item.oldPath, item.rootFolder, now, now};
    } else {
        auto& entry = it->second;
        entry.lastSeen = now;
        if (entry.action == WatchAction::Added && item.action == WatchAction::Modified) {
            // Keep Added
        } else if (entry.action == WatchAction::Added && item.action == WatchAction::Removed) {
            pendingCoalesce.erase(it);
        } else if (entry.action == WatchAction::Removed && item.action == WatchAction::Added) {
            entry.action = WatchAction::Modified;
            entry.path = item.path;
        } else {
            entry.action = item.action;
            entry.path = item.path;
            if (!item.oldPath.empty()) entry.oldPath = item.oldPath;
        }
    }
}

void FolderWatcher::FlushCoalescedEvents(bool forceAll) {
    std::vector<WatchChangeBatch> batches;
    WatchChangeCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        cbCopy = callback;
        if (pendingCoalesce.empty() && !rescanPending) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - oldestPendingTime).count();
        if (!forceAll && elapsedMs < static_cast<int64_t>(coalesceWindowMs) && pendingCoalesce.size() < maxBatchSize) {
            return;
        }

        if (rescanPending) {
            for (const auto& root : rescanRoots) {
                WatchChangeBatch b;
                b.overflowOrRescan = true;
                b.rootFolder = root;
                batches.push_back(std::move(b));
            }
            rescanPending = false;
            rescanRoots.clear();
        }

        if (!pendingCoalesce.empty()) {
            std::unordered_map<std::wstring, WatchChangeBatch> byRoot;
            for (auto& [k, entry] : pendingCoalesce) {
                auto& b = byRoot[entry.rootFolder];
                b.rootFolder = entry.rootFolder;
                b.items.push_back({entry.action, entry.path, entry.oldPath, entry.rootFolder});
                if (b.items.size() > maxBatchSize) {
                    b.overflowOrRescan = true;
                }
            }
            for (auto& [r, b] : byRoot) {
                if (b.overflowOrRescan) b.items.clear();
                batches.push_back(std::move(b));
            }
            pendingCoalesce.clear();
        }
    }

    if (cbCopy) {
        for (const auto& b : batches) {
            cbCopy(b);
        }
    }
}

bool FolderWatcher::ArmReadDirectoryChanges(DirectoryWatch* dirWatch) {
    if (!dirWatch) return false;
    int inotifyFd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    if (inotifyFd <= 0) return false;
    std::string u8Root = LinuxWinPathToPosix(dirWatch->rootPath);
    uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
    int wd = ::inotify_add_watch(inotifyFd, u8Root.c_str(), mask);
    if (wd < 0) {
        dirWatch->active = false;
        dirWatch->retryAtTick = GetTickCount64() + 2000;
        return false;
    }
    dirWatch->hDirectory = reinterpret_cast<HANDLE>(static_cast<intptr_t>(wd));
    dirWatch->active = true;
    dirWatch->pendingRead = false;
    return true;
}

void FolderWatcher::ProcessNotificationBuffer(DirectoryWatch* /*dirWatch*/, DWORD /*bytesTransferred*/) {}

void FolderWatcher::ApplyPendingConfiguration() {
    std::optional<std::vector<std::wstring>> desired;
    uint64_t targetGen = 0;
    {
        std::lock_guard<std::mutex> lock(configurationMutex);
        if (!pendingConfiguration.has_value()) return;
        desired = std::move(pendingConfiguration);
        pendingConfiguration.reset();
        targetGen = requestedConfiguration.load(std::memory_order_acquire);
    }

    std::unordered_map<std::wstring, std::wstring> desiredMap;
    for (const auto& f : *desired) {
        if (!f.empty()) desiredMap[NormalizePathKey(f)] = f;
    }

    int inotifyFd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    std::lock_guard<std::mutex> lock(watchMutex);
    for (auto it = watches.begin(); it != watches.end();) {
        if (desiredMap.find(it->first) == desiredMap.end()) {
            int wd = static_cast<int>(reinterpret_cast<intptr_t>(it->second->hDirectory));
            if (inotifyFd > 0 && wd > 0) {
                ::inotify_rm_watch(inotifyFd, wd);
            }
            it = watches.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [normKey, origPath] : desiredMap) {
        if (watches.find(normKey) == watches.end()) {
            auto dw = std::make_unique<DirectoryWatch>();
            dw->rootPath = origPath;
            if (isRunning.load()) {
                ArmReadDirectoryChanges(dw.get());
            }
            watches[normKey] = std::move(dw);
        }
    }
    workerConfiguration = targetGen;
    appliedConfiguration.store(targetGen, std::memory_order_release);
}

void FolderWatcher::ArmInactiveWatches() {
    ULONGLONG now = GetTickCount64();
    if (now < nextInactiveSweep) return;
    nextInactiveSweep = now + 1000;
    std::lock_guard<std::mutex> lock(watchMutex);
    for (auto& [k, dw] : watches) {
        if (!dw->active && now >= dw->retryAtTick) {
            if (ArmReadDirectoryChanges(dw.get())) {
                TriggerRescan(dw->rootPath);
            }
        }
    }
}

void FolderWatcher::CancelDirectoryReads() {}
void FolderWatcher::ReclaimRetiredWatchLocked(DirectoryWatch* /*watch*/) {}
bool FolderWatcher::HasPendingReadsLocked() const { return false; }

void FolderWatcher::WorkerLoop() {
    int inotifyFd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (auto& [k, dw] : watches) {
            ArmReadDirectoryChanges(dw.get());
        }
    }

    alignas(struct inotify_event) char buf[kBufferSize];
    std::unordered_map<uint32_t, std::pair<std::wstring, std::wstring>> moveCookies;

    while (!stopRequested.load()) {
        ApplyPendingConfiguration();
        ArmInactiveWatches();

        if (inotifyFd > 0) {
            struct pollfd pfd {};
            pfd.fd = inotifyFd;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 50);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                ssize_t len = ::read(inotifyFd, buf, sizeof(buf));
                ssize_t offset = 0;
                while (offset < len) {
                    auto* ev = reinterpret_cast<struct inotify_event*>(buf + offset);
                    offset += sizeof(struct inotify_event) + ev->len;

                    std::wstring rootFolder;
                    {
                        std::lock_guard<std::mutex> lock(watchMutex);
                        for (const auto& [k, dw] : watches) {
                            if (static_cast<int>(reinterpret_cast<intptr_t>(dw->hDirectory)) == ev->wd) {
                                rootFolder = dw->rootPath;
                                break;
                            }
                        }
                    }
                    if (rootFolder.empty()) continue;
                    if (ev->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF)) {
                        TriggerRescan(rootFolder);
                        continue;
                    }
                    if (ev->len == 0 || !ev->name[0]) continue;
                    std::wstring relName = LinuxUtf8ToWide(ev->name);
                    std::wstring fullPath = rootFolder;
                    if (!fullPath.empty() && fullPath.back() != L'/' && fullPath.back() != L'\\') {
                        fullPath += L"/";
                    }
                    fullPath += relName;

                    if (ev->mask & IN_MOVED_FROM) {
                        moveCookies[ev->cookie] = {fullPath, rootFolder};
                    } else if (ev->mask & IN_MOVED_TO) {
                        auto it = moveCookies.find(ev->cookie);
                        if (it != moveCookies.end()) {
                            EnqueueEvent({WatchAction::Renamed, fullPath, it->second.first, rootFolder});
                            moveCookies.erase(it);
                        } else {
                            EnqueueEvent({WatchAction::Added, fullPath, L"", rootFolder});
                        }
                    } else if (ev->mask & IN_CREATE) {
                        EnqueueEvent({WatchAction::Added, fullPath, L"", rootFolder});
                    } else if (ev->mask & IN_DELETE) {
                        EnqueueEvent({WatchAction::Removed, fullPath, L"", rootFolder});
                    } else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                        EnqueueEvent({WatchAction::Modified, fullPath, L"", rootFolder});
                    }
                }
            }
        } else {
            Sleep(50);
        }

        FlushCoalescedEvents(false);
    }

    FlushCoalescedEvents(true);
    workerDone.store(true, std::memory_order_release);
}

} // namespace Aperture

#endif // !defined(_WIN32)
