#pragma once

#include "MediaLibrary.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace Aperture {

// Only file identity crosses the worker boundary. Playback, overrides,
// bookmarks and other user-owned state remain exclusively in the live model.
struct WatchFileBaseline {
    std::string id;
    std::wstring path;
    uint64_t size = 0, fileIndex = 0;
    uint32_t volumeSerialNumber = 0;
};

struct PreparedWatchChange {
    WatchAction action = WatchAction::Modified;
    WatchFileBaseline baseline;
    std::wstring path;
    uint64_t size = 0;
    WindowsFileIdentity identity;
    bool metadataMayHaveChanged = false;
};

struct PreparedWatchDelta {
    uint64_t generation = 0;
    std::vector<std::wstring> roots;
    std::vector<PreparedWatchChange> changes;
    std::vector<StorageStatus> errors;
    size_t rescanCount = 0;
    bool canceled = false;
};

inline bool WatchPathWithin(const std::wstring& key, const std::wstring& rootKey) {
    return !rootKey.empty() && key.size() >= rootKey.size() && key.compare(0, rootKey.size(), rootKey) == 0 &&
        (key.size() == rootKey.size() || rootKey.back() == L'\\' || key[rootKey.size()] == L'\\');
}

// A bounded event mailbox. It degrades an event storm into a root rescan,
// and an excess of roots into one rescan-all request, rather than retaining
// an unbounded history while a slow network directory is being enumerated.
class WatchChangeQueue {
public:
    void Push(const WatchChangeBatch& batch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allRoots_) return;
        const auto rootKey = NormalizePathKey(batch.rootFolder);
        if (rootKey.empty()) return;
        auto found = roots_.find(rootKey);
        if (found == roots_.end()) {
            if (roots_.size() >= kMaxRoots) { roots_.clear(); eventCount_ = 0; allRoots_ = true; return; }
            found = roots_.emplace(rootKey, WatchChangeBatch{}).first;
            found->second.rootFolder = batch.rootFolder;
        }
        auto& pending = found->second;
        if (pending.overflowOrRescan) return;
        if (batch.overflowOrRescan || batch.items.size() > kMaxEvents - eventCount_) {
            eventCount_ -= pending.items.size();
            pending.items.clear();
            pending.overflowOrRescan = true;
        } else {
            pending.items.insert(pending.items.end(), batch.items.begin(), batch.items.end());
            eventCount_ += batch.items.size();
        }
    }
    std::vector<WatchChangeBatch> Take(const std::vector<std::wstring>& watchedRoots) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WatchChangeBatch> result;
        for (const auto& root : watchedRoots) {
            if (allRoots_) result.push_back({true, root, {}});
            else {
                auto found = roots_.find(NormalizePathKey(root));
                if (found != roots_.end()) result.push_back(std::move(found->second));
            }
        }
        roots_.clear(); eventCount_ = 0; allRoots_ = false;
        return result;
    }
    void Clear() { std::lock_guard<std::mutex> lock(mutex_); roots_.clear(); eventCount_ = 0; allRoots_ = false; }
    bool HasPending() const { std::lock_guard<std::mutex> lock(mutex_); return allRoots_ || !roots_.empty(); }
private:
    static constexpr size_t kMaxEvents = 1024, kMaxRoots = 64;
    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, WatchChangeBatch> roots_;
    size_t eventCount_ = 0;
    bool allRoots_ = false;
};

class WatchReconcileWorker {
public:
    ~WatchReconcileWorker() { StopAndJoin(); }
    void Queue(const WatchChangeBatch& batch) { pending_.Push(batch); }
    std::vector<WatchChangeBatch> TakePending(const std::vector<std::wstring>& roots) { return pending_.Take(roots); }
    bool IsRunning() const { return worker_.joinable() && !done_.load(std::memory_order_acquire); }
    bool HasCompleted() const { return worker_.joinable() && done_.load(std::memory_order_acquire); }
    bool HasPending() const { return pending_.HasPending(); }
    void Cancel() {
        ++generation_;
        cancel_.store(true, std::memory_order_release);
        pending_.Clear();
        if (worker_.joinable()) CancelSynchronousIo(worker_.native_handle());
    }
    void StopAndJoin() { Cancel(); if (worker_.joinable()) worker_.join(); completed_.reset(); }
    std::unique_ptr<PreparedWatchDelta> TakeCompleted() {
        if (!worker_.joinable() || !done_.load(std::memory_order_acquire)) return {};
        worker_.join();
        auto result = std::move(completed_);
        if (!result || result->canceled || result->generation != generation_) return {};
        return result;
    }
    void Start(std::vector<WatchChangeBatch> batches, std::vector<WatchFileBaseline> baseline, HWND window, UINT message) {
        if (worker_.joinable()) worker_.join();
        cancel_.store(false, std::memory_order_release);
        done_.store(false, std::memory_order_release);
        const uint64_t generation = generation_;
        try {
            worker_ = std::thread([this, batches = std::move(batches), baseline = std::move(baseline), generation, window, message]() mutable {
                struct Completion {
                    std::atomic<bool>& done; HWND window; UINT message;
                    ~Completion() { done.store(true, std::memory_order_release); if (window) PostMessageW(window, message, 0, 0); }
                } completion{done_, window, message};
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                try { completed_ = std::make_unique<PreparedWatchDelta>(Prepare(batches, std::move(baseline), cancel_)); }
                catch (...) {
                    // Retain the request, so a transient allocation failure
                    // cannot silently lose filesystem changes.
                    for (const auto& batch : batches) pending_.Push({true, batch.rootFolder, {}});
                }
                if (completed_) completed_->generation = generation;
            });
        } catch (...) {
            done_.store(true, std::memory_order_release);
            // The moved capture may have consumed batches. A bounded all-root
            // retry is supplied by the caller when Start fails.
            throw;
        }
    }
    static std::vector<WatchFileBaseline> Capture(const MediaLibrary& library, const std::vector<WatchChangeBatch>& batches) {
        std::vector<std::wstring> roots;
        for (const auto& batch : batches) roots.push_back(NormalizePathKey(batch.rootFolder));
        std::vector<WatchFileBaseline> result;
        for (const auto& item : library.mediaItems) {
            const auto key = NormalizePathKey(item.filePath);
            if (std::any_of(roots.begin(), roots.end(), [&](const auto& root) { return WatchPathWithin(key, root); }))
                result.push_back({item.id, item.filePath, item.fileSizeBytes, item.fileIndex, item.volumeSerialNumber});
        }
        return result;
    }
    static PreparedWatchDelta Prepare(const std::vector<WatchChangeBatch>& batches, std::vector<WatchFileBaseline> baseline,
                                      const std::atomic<bool>& cancel) {
        PreparedWatchDelta result;
        std::unordered_map<std::wstring, size_t> byPath;
        for (size_t i = 0; i < baseline.size(); ++i) byPath[NormalizePathKey(baseline[i].path)] = i;
        auto canceled = [&]() { if (cancel.load(std::memory_order_acquire)) result.canceled = true; return result.canceled; };
        auto observe = [&](const std::wstring& path, uint64_t size, bool modified = false) {
            const auto found = byPath.find(NormalizePathKey(path));
            if (found != byPath.end() && baseline[found->second].size == size && !modified) return;
            PreparedWatchChange change;
            change.path = path; change.size = size;
            if (found != byPath.end()) change.baseline = baseline[found->second];
            else change.action = WatchAction::Added;
            change.identity = GetWindowsFileIdentity(path);
            change.metadataMayHaveChanged = modified;
            result.changes.push_back(std::move(change));
            if (found != byPath.end()) {
                auto& existing = baseline[found->second];
                existing.size = size;
                if (result.changes.back().identity.valid) {
                    existing.fileIndex = result.changes.back().identity.fileIndex;
                    existing.volumeSerialNumber = result.changes.back().identity.volumeSerialNumber;
                }
            } else {
                const auto& added = result.changes.back();
                byPath[NormalizePathKey(path)] = baseline.size();
                baseline.push_back({{}, path, size, added.identity.fileIndex, added.identity.volumeSerialNumber});
            }
        };
        auto remove = [&](const std::wstring& path, const std::wstring& root) {
            if (QueryPathAvailability(root) != PathAvailability::Present) return;
            const auto key = NormalizePathKey(path);
            const auto markRemoved = [&](WatchFileBaseline& item) {
                if (QueryPathAvailability(item.path) != PathAvailability::Missing) return;
                PreparedWatchChange change;
                change.action = WatchAction::Removed; change.baseline = item;
                result.changes.push_back(std::move(change));
                byPath.erase(NormalizePathKey(item.path));
                item.path.clear();
            };
            const auto exact = byPath.find(key);
            if (exact != byPath.end()) { markRemoved(baseline[exact->second]); return; }
            for (auto& item : baseline) {
                if (canceled()) return;
                if (!item.path.empty() && WatchPathWithin(NormalizePathKey(item.path), key)) markRemoved(item);
            }
        };
        auto scan = [&](const std::wstring& root) {
            ++result.rescanCount;
            const size_t baselineCount = baseline.size(), firstScanChange = result.changes.size();
            DWORD rootAttributes = GetFileAttributesW(root.c_str());
            if (rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                result.errors.push_back(StorageStatus::Error(StorageErrorCode::FileReadFailed,
                    rootAttributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY, root,
                    "Watched folder is unavailable; records retained"));
                return;
            }
            bool complete = true;
            std::unordered_set<std::wstring> files;
            std::vector<std::wstring> directories{root};
            while (!directories.empty() && !canceled()) {
                std::wstring directory = std::move(directories.back()); directories.pop_back();
                if (!directory.empty() && directory.back() != L'\\') directory += L'\\';
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileExW((directory + L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                    FIND_FIRST_EX_LARGE_FETCH);
                if (find == INVALID_HANDLE_VALUE) { if (GetLastError() != ERROR_FILE_NOT_FOUND) complete = false; continue; }
                struct FindCloser { HANDLE handle; ~FindCloser() { FindClose(handle); } } closer{find};
                do {
                    if (canceled()) break;
                    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                    const auto path = directory + data.cFileName;
                    // Never follow junctions out of the watched tree. Missing
                    // checks below still retain reachable excluded paths.
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) directories.push_back(path);
                    else if (MediaLibrary::IsSupportedMediaExtension(path)) {
                        files.insert(NormalizePathKey(path));
                        observe(path, (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow);
                    }
                } while (FindNextFileW(find, &data));
                if (!result.canceled && GetLastError() != ERROR_NO_MORE_FILES) complete = false;
            }
            if (canceled()) return;
            rootAttributes = GetFileAttributesW(root.c_str());
            complete = complete && rootAttributes != INVALID_FILE_ATTRIBUTES && (rootAttributes & FILE_ATTRIBUTE_DIRECTORY);
            if (!complete) {
                result.errors.push_back(StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_READ_FAULT, root,
                    "Watched folder scan was incomplete; removals deferred"));
                return;
            }
            const auto rootKey = NormalizePathKey(root);
            // An overflow may hide the original rename notification. Reconnect
            // only unique file identities; hard-link aliases remain separate.
            struct FileKey {
                uint64_t index; uint32_t volume;
                bool operator==(const FileKey& other) const { return index == other.index && volume == other.volume; }
            };
            struct FileHash {
                size_t operator()(const FileKey& key) const {
                    return std::hash<uint64_t>{}(key.index) ^ (std::hash<uint32_t>{}(key.volume) << 1);
                }
            };
            std::unordered_map<FileKey, size_t, FileHash> originalCounts, additions;
            for (size_t i = 0; i < baselineCount; ++i) {
                const auto& item = baseline[i];
                if (!item.path.empty() && item.fileIndex) ++originalCounts[{item.fileIndex, item.volumeSerialNumber}];
            }
            for (size_t i = firstScanChange; i < result.changes.size(); ++i) {
                const auto& change = result.changes[i];
                if (change.action != WatchAction::Added || !change.identity.valid || !change.identity.fileIndex) continue;
                const auto inserted = additions.emplace(FileKey{change.identity.fileIndex, change.identity.volumeSerialNumber}, i);
                if (!inserted.second) inserted.first->second = SIZE_MAX;
            }
            for (size_t i = 0; i < baselineCount; ++i) {
                auto& item = baseline[i];
                if (canceled()) return;
                const auto key = NormalizePathKey(item.path);
                if (WatchPathWithin(key, rootKey) && !files.count(key) && QueryPathAvailability(item.path) == PathAvailability::Missing) {
                    const FileKey identity{item.fileIndex, item.volumeSerialNumber};
                    const auto added = additions.find(identity);
                    if (!item.id.empty() && item.fileIndex && originalCounts[identity] == 1 &&
                        added != additions.end() && added->second != SIZE_MAX) {
                        auto& change = result.changes[added->second];
                        change.action = WatchAction::Renamed;
                        change.baseline = item;
                        auto& observed = baseline[byPath.at(NormalizePathKey(change.path))];
                        observed.id = item.id;
                        byPath.erase(key);
                        item.path.clear();
                        continue;
                    }
                    PreparedWatchChange change;
                    change.action = WatchAction::Removed; change.baseline = item;
                    result.changes.push_back(std::move(change));
                    byPath.erase(key);
                    item.path.clear();
                }
            }
        };
        for (const auto& batch : batches) {
            if (canceled()) break;
            result.roots.push_back(batch.rootFolder);
            if (batch.overflowOrRescan) { scan(batch.rootFolder); continue; }
            bool needsScan = false;
            for (const auto& event : batch.items) {
                if (canceled()) break;
                if (event.action == WatchAction::RescanRequired) { needsScan = true; continue; }
                if (event.action == WatchAction::Removed) { remove(event.path, batch.rootFolder); continue; }
                if (event.action == WatchAction::Renamed) {
                    const auto oldKey = NormalizePathKey(event.oldPath);
                    bool renamed = false;
                    for (size_t i = 0; i < baseline.size() && !canceled(); ++i) {
                        auto& item = baseline[i];
                        const auto itemKey = NormalizePathKey(item.path);
                        if (!WatchPathWithin(itemKey, oldKey)) continue;
                        auto newPath = ResolveAppPath(event.path);
                        auto suffix = ResolveAppPath(item.path).substr(oldKey.size());
                        while (!suffix.empty() && suffix.front() == L'\\') suffix.erase(0, 1);
                        if (!suffix.empty()) {
                            if (!newPath.empty() && newPath.back() != L'\\') newPath += L'\\';
                            newPath += suffix;
                        }
                        WIN32_FILE_ATTRIBUTE_DATA data{};
                        if (!MediaLibrary::IsSupportedMediaExtension(newPath) ||
                            !GetFileAttributesExW(newPath.c_str(), GetFileExInfoStandard, &data) ||
                            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { needsScan = true; continue; }
                        PreparedWatchChange change;
                        change.action = WatchAction::Renamed; change.baseline = item; change.path = newPath;
                        change.size = (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
                        change.identity = GetWindowsFileIdentity(newPath);
                        result.changes.push_back(change);
                        byPath.erase(itemKey);
                        item.path = newPath; item.size = change.size;
                        if (change.identity.valid) { item.fileIndex = change.identity.fileIndex; item.volumeSerialNumber = change.identity.volumeSerialNumber; }
                        byPath[NormalizePathKey(newPath)] = i;
                        renamed = true;
                    }
                    if (renamed) continue;
                }
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (!GetFileAttributesExW(event.path.c_str(), GetFileExInfoStandard, &data)) continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { needsScan = true; continue; }
                if (MediaLibrary::IsSupportedMediaExtension(event.path))
                    observe(event.path, (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow, event.action == WatchAction::Modified);
            }
            if (needsScan && !canceled()) scan(batch.rootFolder);
        }
        return result;
    }
private:
    WatchChangeQueue pending_;
    std::thread worker_;
    std::atomic<bool> cancel_{false}, done_{true};
    uint64_t generation_ = 1; // UI thread only; stamped into immutable results.
    std::unique_ptr<PreparedWatchDelta> completed_;
};

} // namespace Aperture
