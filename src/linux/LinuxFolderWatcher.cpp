#if !defined(_WIN32)

#include "../FolderWatcher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace Aperture {

namespace {

constexpr uint32_t kInotifyMask =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_DELETE_SELF | IN_MOVE_SELF | IN_EXCL_UNLINK;
constexpr int64_t kMoveCookieExpireMs = 400;

struct SubWatchInfo {
    std::wstring rootKey;
    std::wstring rootPath;
    std::wstring dirWidePath;
    bool isRoot = false;
};

struct MoveCookieInfo {
    std::wstring path;
    std::wstring rootFolder;
    std::wstring rootKey;
    bool isDir = false;
    std::chrono::steady_clock::time_point seenAt;
};

struct LinuxWatcherState {
    std::mutex mutex;
    std::atomic<int> wakeReadFd{-1};
    std::atomic<int> wakeWriteFd{-1};
    std::unordered_map<int, SubWatchInfo> wdMap;
    std::unordered_map<std::wstring, std::unordered_set<int>> rootToWds;
    std::unordered_map<uint32_t, MoveCookieInfo> moveCookies;
};

std::mutex g_watcherStatesMutex;
std::unordered_map<const FolderWatcher*, std::shared_ptr<LinuxWatcherState>> g_watcherStates;

std::shared_ptr<LinuxWatcherState> GetOrCreateWatcherState(const FolderWatcher* self) {
    std::lock_guard<std::mutex> lk(g_watcherStatesMutex);
    auto& ptr = g_watcherStates[self];
    if (!ptr) ptr = std::make_shared<LinuxWatcherState>();
    return ptr;
}

std::shared_ptr<LinuxWatcherState> FindWatcherState(const FolderWatcher* self) {
    std::lock_guard<std::mutex> lk(g_watcherStatesMutex);
    auto it = g_watcherStates.find(self);
    return (it != g_watcherStates.end()) ? it->second : nullptr;
}

void EraseWatcherState(const FolderWatcher* self) {
    std::lock_guard<std::mutex> lk(g_watcherStatesMutex);
    g_watcherStates.erase(self);
}

bool CreateWakePipe(int fds[2]) {
#if defined(__linux__)
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0) return true;
#endif
    if (::pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        int fl = ::fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) ::fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        int fdFl = ::fcntl(fds[i], F_GETFD, 0);
        if (fdFl >= 0) ::fcntl(fds[i], F_SETFD, fdFl | FD_CLOEXEC);
    }
    return true;
}

void CloseWakePipe(LinuxWatcherState* st) {
    if (!st) return;
    int rfd = st->wakeReadFd.exchange(-1, std::memory_order_acq_rel);
    int wfd = st->wakeWriteFd.exchange(-1, std::memory_order_acq_rel);
    if (rfd >= 0) ::close(rfd);
    if (wfd >= 0) ::close(wfd);
}

void DrainWakePipe(int rfd) {
    if (rfd < 0) return;
    uint8_t buf[64];
    while (::read(rfd, buf, sizeof(buf)) > 0) {}
}

std::wstring TrimTrailingSeparators(std::wstring path) {
    while (path.size() > 1 && (path.back() == L'/' || path.back() == L'\\')) {
        path.pop_back();
    }
    return path;
}

std::wstring JoinWideChild(const std::wstring& dir, const std::wstring& child) {
    std::wstring base = TrimTrailingSeparators(dir);
    if (base.empty()) return child;
    if (base == L"/") return L"/" + child;
    return base + L"/" + child;
}

std::wstring JoinWideRelative(const std::wstring& rootWide, const fs::path& relPath) {
    std::wstring rel = LinuxUtf8ToWide(relPath.generic_u8string());
    if (rel.empty() || rel == L".") return TrimTrailingSeparators(rootWide);
    return JoinWideChild(rootWide, rel);
}

bool IsSameOrSubWidePath(const std::wstring& candidate, const std::wstring& prefix) {
    std::wstring normCand = NormalizePathKey(candidate);
    std::wstring normPrefix = NormalizePathKey(prefix);
    if (normCand == normPrefix) return true;
    if (normCand.size() <= normPrefix.size()) return false;
    if (normCand.compare(0, normPrefix.size(), normPrefix) != 0) return false;
    wchar_t sep = normCand[normPrefix.size()];
    return (normPrefix == L"/" || sep == L'/' || sep == L'\\');
}

void RemoveRootWatchesLocked(LinuxWatcherState* st, int inotifyFd, const std::wstring& rootKey) {
    auto rIt = st->rootToWds.find(rootKey);
    if (rIt == st->rootToWds.end()) return;
    for (int wd : rIt->second) {
        if (inotifyFd > 0 && wd > 0) {
            ::inotify_rm_watch(inotifyFd, wd);
        }
        st->wdMap.erase(wd);
    }
    st->rootToWds.erase(rIt);
}

void RemoveSubtreeWatchesLocked(LinuxWatcherState* st, int inotifyFd, const std::wstring& rootKey, const std::wstring& dirPrefix) {
    auto rIt = st->rootToWds.find(rootKey);
    if (rIt == st->rootToWds.end()) return;
    std::vector<int> toRemove;
    for (int wd : rIt->second) {
        auto wIt = st->wdMap.find(wd);
        if (wIt != st->wdMap.end() && IsSameOrSubWidePath(wIt->second.dirWidePath, dirPrefix)) {
            toRemove.push_back(wd);
        }
    }
    for (int wd : toRemove) {
        if (inotifyFd > 0 && wd > 0) {
            ::inotify_rm_watch(inotifyFd, wd);
        }
        st->wdMap.erase(wd);
        rIt->second.erase(wd);
    }
    if (rIt->second.empty()) {
        st->rootToWds.erase(rIt);
    }
}

void AddSingleSubWatchLocked(LinuxWatcherState* st, int inotifyFd, const std::wstring& rootKey,
                             const std::wstring& rootPath, const std::wstring& dirWidePath,
                             const std::string& dirPosix, bool isRoot, int* outWd = nullptr) {
    if (inotifyFd <= 0) return;
    int wd = ::inotify_add_watch(inotifyFd, dirPosix.c_str(), kInotifyMask);
    if (wd < 0) return;
    if (outWd) *outWd = wd;
    st->wdMap[wd] = {rootKey, rootPath, TrimTrailingSeparators(dirWidePath), isRoot};
    st->rootToWds[rootKey].insert(wd);
}

void AddSubtreeWatchesLocked(LinuxWatcherState* st, int inotifyFd, const std::wstring& rootKey,
                             const std::wstring& rootPath, const std::wstring& baseWidePath,
                             std::vector<std::pair<std::wstring, bool>>* outDiscoveredEntries = nullptr) {
    if (inotifyFd <= 0) return;
    std::string basePosix = LinuxWinPathToPosix(baseWidePath);
    AddSingleSubWatchLocked(st, inotifyFd, rootKey, rootPath, baseWidePath, basePosix, false);

    std::error_code ec;
    fs::path rootFsPath = fs::u8path(basePosix);
    for (fs::recursive_directory_iterator it(rootFsPath, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code typeEc;
        bool isDir = it->is_directory(typeEc);
        fs::path rel = it->path().lexically_relative(rootFsPath);
        std::wstring wideEntry = JoinWideRelative(baseWidePath, rel);
        if (isDir) {
            AddSingleSubWatchLocked(st, inotifyFd, rootKey, rootPath, wideEntry, it->path().u8string(), false);
        }
        if (outDiscoveredEntries) {
            outDiscoveredEntries->emplace_back(std::move(wideEntry), isDir);
        }
    }
}

} // namespace

FolderWatcher::FolderWatcher() {
    GetOrCreateWatcherState(this);
}

FolderWatcher::~FolderWatcher() {
    Stop();
    EraseWatcherState(this);
}

void FolderWatcher::SetCallback(WatchChangeCallback cb) {
    std::lock_guard<std::mutex> lock(coalesceMutex);
    callback = std::move(cb);
}

bool FolderWatcher::Start() {
    if (isRunning.load()) return true;
    int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return false;
    auto st = GetOrCreateWatcherState(this);
    int pfds[2] = {-1, -1};
    if (!CreateWakePipe(pfds)) {
        ::close(fd);
        return false;
    }
    CloseWakePipe(st.get());
    st->wakeReadFd.store(pfds[0], std::memory_order_release);
    st->wakeWriteFd.store(pfds[1], std::memory_order_release);

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
    WakeWorker();
}

void FolderWatcher::Stop() {
    RequestStop();
    if (workerThread.joinable()) {
        workerThread.join();
    }
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        CancelDirectoryReads();
        retiredWatches.clear();
        watches.clear();
    }
    if (fd > 0) {
        ::close(fd);
    }
    hIOCP = NULL;
    if (auto st = FindWatcherState(this)) {
        std::lock_guard<std::mutex> slk(st->mutex);
        st->wdMap.clear();
        st->rootToWds.clear();
        st->moveCookies.clear();
        CloseWakePipe(st.get());
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
    } else {
        WakeWorker();
    }
    return gen;
}

bool FolderWatcher::AddWatch(const std::wstring& folderPath) {
    if (folderPath.empty()) return false;
    std::wstring normKey = NormalizePathKey(folderPath);
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        if (watches.find(normKey) != watches.end()) return true;
        auto dw = std::make_unique<DirectoryWatch>();
        dw->rootPath = folderPath;
        if (isRunning.load()) {
            ArmReadDirectoryChanges(dw.get());
        }
        watches[normKey] = std::move(dw);
    }
    WakeWorker();
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
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        rescanPending = true;
        rescanRoots.insert(rootFolder);
        if (pendingCoalesce.empty()) {
            oldestPendingTime = std::chrono::steady_clock::now();
        }
    }
    WakeWorker();
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
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(u8Root), ec)) {
        dirWatch->active = false;
        dirWatch->pendingRead = false;
        dirWatch->retryAtTick = GetTickCount64() + 2000;
        return false;
    }

    int rootWd = ::inotify_add_watch(inotifyFd, u8Root.c_str(), kInotifyMask);
    if (rootWd < 0) {
        dirWatch->active = false;
        dirWatch->pendingRead = false;
        dirWatch->retryAtTick = GetTickCount64() + 2000;
        return false;
    }

    if (auto st = FindWatcherState(this)) {
        std::wstring rootKey = NormalizePathKey(dirWatch->rootPath);
        std::wstring cleanRoot = TrimTrailingSeparators(dirWatch->rootPath);
        std::lock_guard<std::mutex> slk(st->mutex);
        RemoveRootWatchesLocked(st.get(), inotifyFd, rootKey);
        st->wdMap[rootWd] = {rootKey, dirWatch->rootPath, cleanRoot, true};
        st->rootToWds[rootKey].insert(rootWd);

        std::error_code recEc;
        fs::path rootFsPath = fs::u8path(u8Root);
        for (fs::recursive_directory_iterator it(rootFsPath, fs::directory_options::skip_permission_denied, recEc), end;
             !recEc && it != end; it.increment(recEc)) {
            std::error_code typeEc;
            if (!it->is_directory(typeEc)) continue;
            fs::path rel = it->path().lexically_relative(rootFsPath);
            std::wstring subWide = JoinWideRelative(cleanRoot, rel);
            AddSingleSubWatchLocked(st.get(), inotifyFd, rootKey, dirWatch->rootPath, subWide, it->path().u8string(), false);
        }
    }

    dirWatch->hDirectory = reinterpret_cast<HANDLE>(static_cast<intptr_t>(rootWd));
    dirWatch->active = true;
    dirWatch->pendingRead = true;
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
    auto st = FindWatcherState(this);
    std::lock_guard<std::mutex> lock(watchMutex);
    for (auto it = watches.begin(); it != watches.end();) {
        if (desiredMap.find(it->first) == desiredMap.end()) {
            if (st) {
                std::lock_guard<std::mutex> slk(st->mutex);
                RemoveRootWatchesLocked(st.get(), inotifyFd, it->first);
                for (auto cIt = st->moveCookies.begin(); cIt != st->moveCookies.end();) {
                    if (cIt->second.rootKey == it->first) cIt = st->moveCookies.erase(cIt);
                    else ++cIt;
                }
            } else {
                int wd = static_cast<int>(reinterpret_cast<intptr_t>(it->second->hDirectory));
                if (inotifyFd > 0 && wd > 0) ::inotify_rm_watch(inotifyFd, wd);
            }
            it->second->active = false;
            it->second->pendingRead = false;
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
    std::vector<std::wstring> rearmedRoots;
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (auto& [k, dw] : watches) {
            if (!dw->active && !dw->retired && now >= dw->retryAtTick) {
                if (ArmReadDirectoryChanges(dw.get())) {
                    rearmedRoots.push_back(dw->rootPath);
                }
            }
        }
    }
    for (const auto& root : rearmedRoots) {
        TriggerRescan(root);
    }
}

void FolderWatcher::WakeWorker() {
    auto st = FindWatcherState(this);
    if (!st) return;
    int wfd = st->wakeWriteFd.load(std::memory_order_acquire);
    if (wfd >= 0) {
        uint8_t one = 1;
        ssize_t unused = ::write(wfd, &one, sizeof(one));
        (void)unused;
    }
}

DWORD FolderWatcher::ComputeWaitTimeoutMs() {
    int64_t bestMs = -1;
    auto considerMs = [&bestMs](int64_t ms) {
        if (ms < 0) ms = 0;
        if (bestMs < 0 || ms < bestMs) bestMs = ms;
    };

    const auto nowSteady = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(coalesceMutex);
        if (!pendingCoalesce.empty() || rescanPending) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - oldestPendingTime).count();
            considerMs(static_cast<int64_t>(coalesceWindowMs) - elapsed);
        }
    }

    const ULONGLONG nowTick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (const auto& [k, dw] : watches) {
            if (!dw->active && !dw->retired) {
                int64_t rem = (dw->retryAtTick > nowTick) ? static_cast<int64_t>(dw->retryAtTick - nowTick) : 0;
                considerMs(rem);
            }
        }
    }

    if (auto st = FindWatcherState(this)) {
        std::lock_guard<std::mutex> slk(st->mutex);
        for (const auto& [cookie, info] : st->moveCookies) {
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(nowSteady - info.seenAt).count();
            considerMs(kMoveCookieExpireMs - age);
        }
    }

    if (bestMs < 0) return INFINITE;
    return static_cast<DWORD>(std::min<int64_t>(bestMs, INT_MAX));
}

void FolderWatcher::CancelDirectoryReads() {
    int inotifyFd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    auto st = FindWatcherState(this);
    for (auto& [k, dw] : watches) {
        if (st) {
            std::lock_guard<std::mutex> slk(st->mutex);
            RemoveRootWatchesLocked(st.get(), inotifyFd, k);
        } else {
            int wd = static_cast<int>(reinterpret_cast<intptr_t>(dw->hDirectory));
            if (inotifyFd > 0 && wd > 0) ::inotify_rm_watch(inotifyFd, wd);
        }
        dw->hDirectory = INVALID_HANDLE_VALUE;
        dw->active = false;
        dw->pendingRead = false;
    }
    for (auto it = retiredWatches.begin(); it != retiredWatches.end();) {
        (*it)->active = false;
        (*it)->pendingRead = false;
        (*it)->retired = true;
        it = retiredWatches.erase(it);
    }
}

void FolderWatcher::ReclaimRetiredWatchLocked(DirectoryWatch* watch) {
    if (!watch) return;
    watch->active = false;
    watch->pendingRead = false;
    watch->retired = true;
    retiredWatches.erase(
        std::remove_if(retiredWatches.begin(), retiredWatches.end(),
                       [watch](const std::unique_ptr<DirectoryWatch>& p) { return p.get() == watch || !p->pendingRead; }),
        retiredWatches.end());
}

bool FolderWatcher::HasPendingReadsLocked() const {
    if (!retiredWatches.empty()) return true;
    for (const auto& [k, dw] : watches) {
        if (dw->pendingRead) return true;
    }
    return false;
}

void FolderWatcher::WorkerLoop() {
    int inotifyFd = static_cast<int>(reinterpret_cast<intptr_t>(hIOCP));
    auto st = GetOrCreateWatcherState(this);
    {
        std::lock_guard<std::mutex> lock(watchMutex);
        for (auto& [k, dw] : watches) {
            ArmReadDirectoryChanges(dw.get());
        }
    }

    alignas(struct inotify_event) char buf[kBufferSize];

    while (!stopRequested.load()) {
        ApplyPendingConfiguration();
        ArmInactiveWatches();

        // Expire unmatched IN_MOVED_FROM cookies (moved outside watched tree)
        std::vector<WatchChangeItem> expiredRemovals;
        {
            auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> slk(st->mutex);
            for (auto it = st->moveCookies.begin(); it != st->moveCookies.end();) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.seenAt).count();
                if (age >= kMoveCookieExpireMs) {
                    expiredRemovals.push_back({WatchAction::Removed, it->second.path, L"", it->second.rootFolder});
                    if (it->second.isDir) {
                        RemoveSubtreeWatchesLocked(st.get(), inotifyFd, it->second.rootKey, it->second.path);
                    }
                    it = st->moveCookies.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& item : expiredRemovals) {
            EnqueueEvent(item);
        }

        FlushCoalescedEvents(false);
        if (stopRequested.load()) break;

        DWORD waitMs = ComputeWaitTimeoutMs();
        int timeout = (waitMs == INFINITE) ? -1 : static_cast<int>(std::min<DWORD>(waitMs, static_cast<DWORD>(INT_MAX)));
        int wakeReadFd = st->wakeReadFd.load(std::memory_order_acquire);

        struct pollfd pfds[2] = {};
        nfds_t nfds = 0;
        int inotifyIdx = -1;
        int wakeIdx = -1;
        if (inotifyFd > 0) {
            inotifyIdx = static_cast<int>(nfds);
            pfds[nfds].fd = inotifyFd;
            pfds[nfds].events = POLLIN;
            ++nfds;
        }
        if (wakeReadFd >= 0) {
            wakeIdx = static_cast<int>(nfds);
            pfds[nfds].fd = wakeReadFd;
            pfds[nfds].events = POLLIN;
            ++nfds;
        }
        if (nfds == 0) break;

        int pr = ::poll(pfds, nfds, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (wakeIdx >= 0 && (pfds[wakeIdx].revents & POLLIN)) {
            DrainWakePipe(wakeReadFd);
        }
        if (inotifyIdx >= 0 && (pfds[inotifyIdx].revents & POLLIN)) {
            while (!stopRequested.load()) {
                ssize_t len = ::read(inotifyFd, buf, sizeof(buf));
                if (len <= 0) {
                    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                        if (errno == EINTR) continue;
                    }
                    break;
                }
                ssize_t offset = 0;
                while (offset < len) {
                    auto* ev = reinterpret_cast<struct inotify_event*>(buf + offset);
                    offset += sizeof(struct inotify_event) + ev->len;

                    if (ev->mask & IN_Q_OVERFLOW) {
                        std::vector<std::wstring> roots;
                        {
                            std::lock_guard<std::mutex> lock(watchMutex);
                            for (const auto& [k, dw] : watches) roots.push_back(dw->rootPath);
                        }
                        for (const auto& r : roots) TriggerRescan(r);
                        continue;
                    }

                    SubWatchInfo info{};
                    bool foundWd = false;
                    {
                        std::lock_guard<std::mutex> slk(st->mutex);
                        auto wIt = st->wdMap.find(ev->wd);
                        if (wIt != st->wdMap.end()) {
                            info = wIt->second;
                            foundWd = true;
                        }
                        if (ev->mask & IN_IGNORED) {
                            if (foundWd) {
                                auto rIt = st->rootToWds.find(info.rootKey);
                                if (rIt != st->rootToWds.end()) {
                                    rIt->second.erase(ev->wd);
                                    if (rIt->second.empty()) st->rootToWds.erase(rIt);
                                }
                                st->wdMap.erase(ev->wd);
                            }
                        }
                    }
                    if (!foundWd) continue;

                    if (ev->mask & IN_IGNORED) continue;

                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        if (info.isRoot) {
                            {
                                std::lock_guard<std::mutex> slk(st->mutex);
                                RemoveRootWatchesLocked(st.get(), inotifyFd, info.rootKey);
                            }
                            {
                                std::lock_guard<std::mutex> lock(watchMutex);
                                auto it = watches.find(info.rootKey);
                                if (it != watches.end()) {
                                    it->second->active = false;
                                    it->second->pendingRead = false;
                                    it->second->retryAtTick = GetTickCount64() + 2000;
                                }
                            }
                            TriggerRescan(info.rootPath);
                        } else {
                            std::lock_guard<std::mutex> slk(st->mutex);
                            RemoveSubtreeWatchesLocked(st.get(), inotifyFd, info.rootKey, info.dirWidePath);
                        }
                        continue;
                    }

                    if (ev->len == 0 || !ev->name[0]) continue;
                    std::wstring relName = LinuxUtf8ToWide(ev->name);
                    std::wstring fullPath = JoinWideChild(info.dirWidePath, relName);
                    const bool isDir = (ev->mask & IN_ISDIR) != 0;

                    if (ev->mask & IN_MOVED_FROM) {
                        std::lock_guard<std::mutex> slk(st->mutex);
                        st->moveCookies[ev->cookie] = {
                            fullPath, info.rootPath, info.rootKey, isDir, std::chrono::steady_clock::now()};
                    } else if (ev->mask & IN_MOVED_TO) {
                        MoveCookieInfo oldInfo{};
                        bool matchedCookie = false;
                        {
                            std::lock_guard<std::mutex> slk(st->mutex);
                            auto cIt = st->moveCookies.find(ev->cookie);
                            if (cIt != st->moveCookies.end()) {
                                oldInfo = std::move(cIt->second);
                                st->moveCookies.erase(cIt);
                                matchedCookie = true;
                            }
                        }
                        if (matchedCookie) {
                            EnqueueEvent({WatchAction::Renamed, fullPath, oldInfo.path, info.rootPath});
                            if (isDir) {
                                std::lock_guard<std::mutex> slk(st->mutex);
                                if (oldInfo.rootKey == info.rootKey) {
                                    // Update wide paths of moved subtree watches in place
                                    std::wstring oldPrefix = TrimTrailingSeparators(oldInfo.path);
                                    std::wstring newPrefix = TrimTrailingSeparators(fullPath);
                                    auto rIt = st->rootToWds.find(info.rootKey);
                                    if (rIt != st->rootToWds.end()) {
                                        for (int wd : rIt->second) {
                                            auto wIt = st->wdMap.find(wd);
                                            if (wIt != st->wdMap.end() && IsSameOrSubWidePath(wIt->second.dirWidePath, oldPrefix)) {
                                                std::wstring tail = wIt->second.dirWidePath.substr(oldPrefix.size());
                                                wIt->second.dirWidePath = newPrefix + tail;
                                            }
                                        }
                                    }
                                } else {
                                    RemoveSubtreeWatchesLocked(st.get(), inotifyFd, oldInfo.rootKey, oldInfo.path);
                                    AddSubtreeWatchesLocked(st.get(), inotifyFd, info.rootKey, info.rootPath, fullPath);
                                }
                            }
                        } else {
                            EnqueueEvent({WatchAction::Added, fullPath, L"", info.rootPath});
                            if (isDir) {
                                std::vector<std::pair<std::wstring, bool>> discovered;
                                {
                                    std::lock_guard<std::mutex> slk(st->mutex);
                                    AddSubtreeWatchesLocked(st.get(), inotifyFd, info.rootKey, info.rootPath, fullPath, &discovered);
                                }
                                for (const auto& [entryPath, entryIsDir] : discovered) {
                                    (void)entryIsDir;
                                    EnqueueEvent({WatchAction::Added, entryPath, L"", info.rootPath});
                                }
                            }
                        }
                    } else if (ev->mask & IN_CREATE) {
                        EnqueueEvent({WatchAction::Added, fullPath, L"", info.rootPath});
                        if (isDir) {
                            std::vector<std::pair<std::wstring, bool>> discovered;
                            {
                                std::lock_guard<std::mutex> slk(st->mutex);
                                AddSubtreeWatchesLocked(st.get(), inotifyFd, info.rootKey, info.rootPath, fullPath, &discovered);
                            }
                            for (const auto& [entryPath, entryIsDir] : discovered) {
                                (void)entryIsDir;
                                EnqueueEvent({WatchAction::Added, entryPath, L"", info.rootPath});
                            }
                        }
                    } else if (ev->mask & IN_DELETE) {
                        if (isDir) {
                            std::lock_guard<std::mutex> slk(st->mutex);
                            RemoveSubtreeWatchesLocked(st.get(), inotifyFd, info.rootKey, fullPath);
                        }
                        EnqueueEvent({WatchAction::Removed, fullPath, L"", info.rootPath});
                    } else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                        if (!isDir) {
                            EnqueueEvent({WatchAction::Modified, fullPath, L"", info.rootPath});
                        }
                    }
                }
            }
        }
    }

    FlushCoalescedEvents(true);
    workerDone.store(true, std::memory_order_release);
}

} // namespace Aperture

#endif // !defined(_WIN32)
