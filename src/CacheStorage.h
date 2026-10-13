#pragma once

#include <string_view>

// Disposable on-disk cache subsystem: budgets, atomic cache IO, scanning, and eviction.

#include "AppIncludes.h"
#include "PathFileHelpers.h"
#include "StorageStatus.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Aperture {

enum class CacheClass { Thumbnails = 0, Shaders, Subtitles, Generic };

inline const wchar_t* GetCacheClassSubDirName(CacheClass c) {
    switch (c) {
        case CacheClass::Thumbnails: return L"thumbnails";
        case CacheClass::Shaders: return L"shaders";
        case CacheClass::Subtitles: return L"subtitles";
        case CacheClass::Generic:
        default: return L"generic";
    }
}

inline std::wstring GetCacheDir(const std::wstring& root = GetExeDir()) {
    std::wstring dir = root + L"\\cache";
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { SHCreateDirectoryExW(NULL, dir.c_str(), NULL); }
    return dir;
}

inline std::wstring GetCacheDir(CacheClass c, const std::wstring& root = GetExeDir()) {
    std::wstring baseDir = GetCacheDir(root);
    std::wstring subDir = baseDir + L"\\" + GetCacheClassSubDirName(c);
    DWORD attr = GetFileAttributesW(subDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { SHCreateDirectoryExW(NULL, subDir.c_str(), NULL); }
    return subDir;
}

struct CacheBudgetConfig {
    // Normalized ceilings: the old 512 MiB total / 384 MiB class sum let a portable folder grow far beyond its working set. 160 MiB total
    // matches the enabled-class sum and still covers the thumbnail working set when thumbnails are stored compressed (see plan section
    // 5.4).
    uint64_t totalCeilingBytes = 160ULL * 1024ULL * 1024ULL;      // 160 MB
    uint64_t thumbnailBudgetBytes = 64ULL * 1024ULL * 1024ULL;   // 64 MB
    uint64_t shaderBudgetBytes = 64ULL * 1024ULL * 1024ULL;      // 64 MB
    uint64_t subtitleBudgetBytes = 16ULL * 1024ULL * 1024ULL;     // 16 MB
    uint64_t genericBudgetBytes = 16ULL * 1024ULL * 1024ULL;      // 16 MB
    uint64_t GetBudget(CacheClass c) const {
        switch (c) {
            case CacheClass::Thumbnails: return thumbnailBudgetBytes;
            case CacheClass::Shaders: return shaderBudgetBytes;
            case CacheClass::Subtitles: return subtitleBudgetBytes;
            case CacheClass::Generic:
            default: return genericBudgetBytes;
        }
    }
    void SetBudget(CacheClass c, uint64_t bytes) {
        switch (c) {
            case CacheClass::Thumbnails: thumbnailBudgetBytes = bytes; break;
            case CacheClass::Shaders: shaderBudgetBytes = bytes; break;
            case CacheClass::Subtitles: subtitleBudgetBytes = bytes; break;
            case CacheClass::Generic:
            default: genericBudgetBytes = bytes; break;
        }
    }
    bool IsEnabled(CacheClass c) const { return totalCeilingBytes > 0 && GetBudget(c) > 0; }
};

namespace Detail {
    inline std::mutex& GetCacheBudgetMutex() { static std::mutex mtx; return mtx; }
    inline CacheBudgetConfig& GetGlobalCacheBudgetsInternal() { static CacheBudgetConfig config; return config; }
}

inline CacheBudgetConfig GetGlobalCacheBudgets() {
    std::lock_guard<std::mutex> lock(Detail::GetCacheBudgetMutex());
    return Detail::GetGlobalCacheBudgetsInternal();
}

inline void SetGlobalCacheBudgets(const CacheBudgetConfig& config) {
    std::lock_guard<std::mutex> lock(Detail::GetCacheBudgetMutex());
    Detail::GetGlobalCacheBudgetsInternal() = config;
}

inline StorageStatus CheckCacheDirectoryWritability(const std::wstring& root = GetExeDir(), std::optional<CacheClass> c = std::nullopt) {
    if (root.empty()) { return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Root path is empty"); }
    std::wstring dir = c.has_value() ? (root + L"\\cache\\" + GetCacheClassSubDirName(*c)) : (root + L"\\cache");
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, dir, "Failed to create cache directory");
        }
        attr = GetFileAttributesW(dir.c_str());
    }
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, dir, "Cache directory has read-only attribute");
    std::wstring probe = dir + L"\\.writetest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE hFile = CreateFileW(probe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, err, dir, "Cache directory is read-only or not writable");
    }
    CloseHandle(hFile);
    return StorageStatus::Ok(dir);
}

struct CacheFileInfo {
    std::wstring path;
    uint64_t sizeBytes = 0;
    FILETIME lastAccessTime{};
    FILETIME lastWriteTime{};
    FILETIME creationTime{};
    CacheClass cacheClass = CacheClass::Generic;
};

struct CacheClassStats { uint64_t totalBytes = 0; size_t fileCount = 0; };

struct CacheStats { uint64_t totalBytes = 0; size_t totalFiles = 0; std::map<CacheClass, CacheClassStats> classStats; };

inline uint64_t FileTimeToUint64(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

inline bool CompareCacheFilesOldestFirst(const CacheFileInfo& a, const CacheFileInfo& b) {
    uint64_t accessA = FileTimeToUint64(a.lastAccessTime);
    uint64_t accessB = FileTimeToUint64(b.lastAccessTime);
    uint64_t writeA = FileTimeToUint64(a.lastWriteTime);
    uint64_t writeB = FileTimeToUint64(b.lastWriteTime);
    // Select each file's age independently so missing access timestamps still
    // form a strict ordering (required by both sorting and heap selection).
    if (!accessA) accessA = writeA;
    if (!accessB) accessB = writeB;
    if (accessA != accessB) return accessA < accessB;
    if (writeA != writeB) { return writeA < writeB; }
    uint64_t createA = FileTimeToUint64(a.creationTime);
    uint64_t createB = FileTimeToUint64(b.creationTime);
    if (createA != createB) { return createA < createB; }
    return a.path < b.path;
}

inline CacheClass ClassifyCachePath(const std::wstring& path, const std::wstring& root = GetExeDir()) {
    std::wstring cacheDir = ResolveAppPath(root + L"\\cache", root);
    std::wstring p = ResolveAppPath(path, root);
    std::wstring thumbsDir = cacheDir + L"\\thumbnails";
    std::wstring shadersDir = cacheDir + L"\\shaders";
    std::wstring subsDir = cacheDir + L"\\subtitles";
    if (IsPathUnderRoot(p, thumbsDir)) return CacheClass::Thumbnails;
    if (IsPathUnderRoot(p, shadersDir)) return CacheClass::Shaders;
    if (IsPathUnderRoot(p, subsDir)) return CacheClass::Subtitles;
    return CacheClass::Generic;
}

template<typename Visitor>
inline void VisitCacheFiles(const std::wstring& root, std::optional<CacheClass> filterClass, size_t maxScanEntries, Visitor&& visit) {
    size_t count = 0;
    std::wstring baseDir = GetFullPathDynamic(root + L"\\cache");
    if (filterClass.has_value()) { baseDir = baseDir + L"\\" + GetCacheClassSubDirName(*filterClass); }
    DWORD attr = GetFileAttributesW(baseDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { return; }
    std::vector<std::wstring> dirQueue;
    dirQueue.push_back(baseDir);
    while (!dirQueue.empty() && count < maxScanEntries) {
        std::wstring curDir = std::move(dirQueue.back());
        dirQueue.pop_back();
        std::wstring searchPattern = curDir + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        struct FindGuard { HANDLE value; ~FindGuard() { FindClose(value); } } guard{hFind};
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring fullPath = curDir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirQueue.push_back(fullPath); } else {
                if (IsCachePathSafe(fullPath, root)) {
                    CacheFileInfo info;
                    info.path = std::move(fullPath);
                    info.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                    info.lastAccessTime = fd.ftLastAccessTime;
                    info.lastWriteTime = fd.ftLastWriteTime;
                    info.creationTime = fd.ftCreationTime;
                    info.cacheClass = ClassifyCachePath(info.path, root);
                    visit(std::move(info));
                    ++count;
                    if (count >= maxScanEntries) break;
                }
            }
        } while (FindNextFileW(hFind, &fd));
    }
}

inline std::vector<CacheFileInfo> ScanCacheFiles(const std::wstring& root = GetExeDir(), std::optional<CacheClass> filterClass = std::nullopt, size_t maxScanEntries = SIZE_MAX) {
    std::vector<CacheFileInfo> results;
    VisitCacheFiles(root, filterClass, maxScanEntries, [&](CacheFileInfo file) { results.push_back(std::move(file)); });
    return results;
}

inline CacheStats QueryCacheStats(const std::wstring& root = GetExeDir(), size_t maxScanEntries = SIZE_MAX) {
    CacheStats stats;
    VisitCacheFiles(root, std::nullopt, maxScanEntries, [&](const CacheFileInfo& f) {
        stats.totalBytes += f.sizeBytes;
        stats.totalFiles++;
        stats.classStats[f.cacheClass].totalBytes += f.sizeBytes;
        stats.classStats[f.cacheClass].fileCount++;
    });
    return stats;
}

struct EvictionResult {
    size_t filesAttempted = 0, filesEvicted = 0, filesLocked = 0;
    uint64_t bytesEvicted = 0;
    StorageStatus status = StorageStatus::Ok();
};


namespace Detail {
    // Serializes accounting and mutations, including background migration. Reads
    // and decoding remain independent. External changes are reconciled every 30s
    // and before eviction; direct app deletions invalidate the index immediately.
    inline std::recursive_mutex& CacheIOMutex() { static std::recursive_mutex mutex; return mutex; }
    struct CachePathLess {
        using is_transparent = void;
        bool operator()(const CacheFileInfo& a, const CacheFileInfo& b) const { return (*this)(a, b.path); }
        bool operator()(const CacheFileInfo& a, const std::wstring& b) const { return _wcsicmp(a.path.c_str(), b.c_str()) < 0; }
        bool operator()(const std::wstring& a, const CacheFileInfo& b) const { return _wcsicmp(a.c_str(), b.path.c_str()) < 0; }
    };
    struct CacheIndex {
        // One owned path per file, also used for heterogeneous path lookup.
        std::set<CacheFileInfo, CachePathLess> files;
        uint64_t total = 0, hub = 0, seek = 0, scannedAt = 0;
        std::map<CacheClass, uint64_t> classes;
        bool valid = false;
    };
    inline std::map<std::wstring, CacheIndex>& CacheIndexes() { static std::map<std::wstring, CacheIndex> indexes; return indexes; }
    inline std::wstring CacheRootKey(const std::wstring& root) {
        auto key = GetFullPathDynamic(root);
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
        return key;
    }
    inline bool IsSeekThumbnail(const std::wstring& path) {
        const auto pos = path.find_last_of(L"\\/");
        const auto name = std::wstring_view(path).substr(pos == std::wstring::npos ? 0 : pos + 1);
        if (name.size() != 26 || _wcsnicmp(name.data(), L"thumb_", 6) != 0 || _wcsicmp(name.data() + 22, L".bin") != 0) return false;
        return std::all_of(name.begin() + 6, name.begin() + 22, [](wchar_t c) { return iswxdigit(c) != 0; });
    }
    inline void RemoveIndexFile(CacheIndex& index, const std::wstring& path) {
        auto it = index.files.find(path); if (it == index.files.end()) return;
        index.total -= it->sizeBytes; index.classes[it->cacheClass] -= it->sizeBytes;
        if (it->cacheClass == CacheClass::Thumbnails) (IsSeekThumbnail(it->path) ? index.seek : index.hub) -= it->sizeBytes;
        index.files.erase(it);
    }
    inline void AddIndexFile(CacheIndex& index, CacheFileInfo file) {
        // insert does not consume file when an equivalent path already exists.
        // The normal scan/write path needs only one tree search; replacement
        // is uncommon and must first retire the old accounting.
        auto inserted = index.files.insert(std::move(file));
        if (!inserted.second) {
            RemoveIndexFile(index, file.path);
            inserted = index.files.insert(std::move(file));
        }
        const auto& stored = *inserted.first;
        index.total += stored.sizeBytes; index.classes[stored.cacheClass] += stored.sizeBytes;
        if (stored.cacheClass == CacheClass::Thumbnails) (IsSeekThumbnail(stored.path) ? index.seek : index.hub) += stored.sizeBytes;
    }
    inline CacheIndex& IndexFor(const std::wstring& root, bool refresh = false) {
        auto& indexes = CacheIndexes(); const auto key = CacheRootKey(root);
        // The app uses one root; bound memory when test tools use many roots.
        if (indexes.find(key) == indexes.end() && indexes.size() >= 16) indexes.erase(indexes.begin());
        auto& index = indexes[key]; const auto now = GetTickCount64();
        if (refresh || !index.valid || now - index.scannedAt >= 30000) {
            index = CacheIndex{};
            VisitCacheFiles(root, std::nullopt, SIZE_MAX, [&](CacheFileInfo file) { AddIndexFile(index, std::move(file)); });
            index.scannedAt = now; index.valid = true;
        }
        return index;
    }
    inline uint64_t ThumbnailTierBudget(const CacheBudgetConfig& config, bool seek) {
        const auto preview = config.thumbnailBudgetBytes / 4;
        return seek ? preview : config.thumbnailBudgetBytes - preview;
    }
    inline EvictionResult EvictIndexed(CacheIndex& index, const CacheBudgetConfig& config, const std::wstring& root,
            std::optional<CacheClass> target, uint64_t incoming, const std::wstring& replacing = L"", bool tiered = false) {
        EvictionResult result;
        const bool seek = IsSeekThumbnail(replacing);
        const auto existing = index.files.find(replacing);
        const uint64_t oldSize = existing == index.files.end() ? 0 : existing->sizeBytes;
        const auto exceeds = [&]() {
            if (index.total - oldSize + incoming > config.totalCeilingBytes) return true;
            if (target && index.classes[*target] - oldSize + incoming > config.GetBudget(*target)) return true;
            return tiered && ((seek ? index.seek : index.hub) - oldSize + incoming > ThumbnailTierBudget(config, seek));
        };
        if (!exceeds()) return result;
        // Build a min-heap only for the current phase. Removing k old files
        // takes O(n + k log n), without sorting all n paths on every write.
        std::vector<const CacheFileInfo*> candidates;
        candidates.reserve(index.files.size());
        const auto newer = [](const auto* a, const auto* b) { return CompareCacheFilesOldestFirst(*b, *a); };
        for (int phase = 0; phase < 2 && exceeds(); ++phase) {
            if (phase == 0 && !target) continue;
            if (phase == 1 && index.total - oldSize + incoming <= config.totalCeilingBytes) break;
            candidates.clear();
            for (const auto& file : index.files) {
                if (_wcsicmp(file.path.c_str(), replacing.c_str()) == 0) continue;
                if (phase == 0 && (file.cacheClass != *target || (tiered && IsSeekThumbnail(file.path) != seek))) continue;
                if (phase == 1 && tiered && file.cacheClass == CacheClass::Thumbnails) continue;
                candidates.push_back(&file);
            }
            std::make_heap(candidates.begin(), candidates.end(), newer);
            while (!candidates.empty() && exceeds()) {
                if (phase == 1 && index.total - oldSize + incoming <= config.totalCeilingBytes) break;
                std::pop_heap(candidates.begin(), candidates.end(), newer);
                const auto& file = *candidates.back();
                candidates.pop_back(); // No erased pointer remains in the heap.
                ++result.filesAttempted;
                if (!IsCachePathSafe(file.path, root)) continue;
                DWORD attributes = GetFileAttributesW(file.path.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
                    SetFileAttributesW(file.path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
                if (DeleteFileW(file.path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                    ++result.filesEvicted; result.bytesEvicted += file.sizeBytes;
                    RemoveIndexFile(index, file.path);
                } else ++result.filesLocked;
            }
        }
        return result;
    }
}

inline void InvalidateCacheAccounting(const std::wstring& root) {
    std::lock_guard<std::recursive_mutex> lock(Detail::CacheIOMutex());
    Detail::CacheIndexes().erase(Detail::CacheRootKey(root));
}

inline EvictionResult EvictCacheToBudget(const CacheBudgetConfig& budgets, const std::wstring& root = GetExeDir(),
        std::optional<CacheClass> targetClass = std::nullopt, uint64_t incomingBytes = 0, size_t maxScanEntries = SIZE_MAX) {
    (void)maxScanEntries; // Budget enforcement must account for the entire cache.
    std::lock_guard<std::recursive_mutex> lock(Detail::CacheIOMutex());
    return Detail::EvictIndexed(Detail::IndexFor(root, true), budgets, root, targetClass, incomingBytes);
}

inline StorageStatus AtomicWriteCacheFile( CacheClass cacheClass, const std::wstring& relativePath, const std::string& data,
    const CacheBudgetConfig& budgets = GetGlobalCacheBudgets(), const std::wstring& root = GetExeDir(),
    const std::string* expectedContent = nullptr)
{
    std::lock_guard<std::recursive_mutex> ioLock(Detail::CacheIOMutex());
    if (relativePath.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Cache relative path is empty");
    if (root.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Root path is empty");
    // The atomic write below already creates a unique file in the destination
    // directory and reports open/write errors. Avoid an extra create/close/delete
    // writability probe for every cache entry (thumbnail imports can write many).
    const std::wstring classDir = GetCacheDir(cacheClass, root);
    const DWORD classDirAttributes = GetFileAttributesW(classDir.c_str());
    if (classDirAttributes != INVALID_FILE_ATTRIBUTES && (classDirAttributes & FILE_ATTRIBUTE_READONLY))
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, classDir, "Cache directory has read-only attribute");
    std::wstring targetPath = GetFullPathDynamic(classDir + L"\\" + NormalizePathSeparators(relativePath));
    if (!IsCachePathSafe(targetPath, root))
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, targetPath, "Target cache path is unsafe or outside cache root");
    if (ClassifyCachePath(targetPath, root) != cacheClass)
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, targetPath, "Target escapes the requested cache class");
    if (!budgets.IsEnabled(cacheClass) || budgets.totalCeilingBytes == 0 || budgets.GetBudget(cacheClass) == 0)
        return StorageStatus::Error(StorageErrorCode::CacheBudgetExceeded, ERROR_DISK_FULL, targetPath, "Cache class budget is zero or cache is disabled");
    uint64_t dataSize = static_cast<uint64_t>(data.size());
    if (dataSize > budgets.GetBudget(cacheClass) || dataSize > budgets.totalCeilingBytes)
        return StorageStatus::Error(StorageErrorCode::CacheBudgetExceeded, ERROR_DISK_FULL, targetPath, "File size exceeds total or class cache budget");

    WIN32_FILE_ATTRIBUTE_DATA previous{};
    const bool existed = GetFileAttributesExW(targetPath.c_str(), GetFileExInfoStandard, &previous) != FALSE;
    if (expectedContent) {
        // Migration is a compare-and-replace, never a new write: clearing or
        // regenerating a thumbnail while it is queued must not resurrect it.
        if (!existed || data.size() >= expectedContent->size())
            return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_FILE_NOT_FOUND, targetPath, "Migration source changed");
        HANDLE old = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (old == INVALID_HANDLE_VALUE) return StorageStatus::Error(StorageErrorCode::FileOpenFailed, GetLastError(), targetPath);
        LARGE_INTEGER length{}; DWORD read = 0;
        bool same = GetFileSizeEx(old, &length) && length.QuadPart == expectedContent->size();
        std::string actual;
        if (same) {
            actual.resize(expectedContent->size());
            same = ReadFile(old, actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr) && read == actual.size() && actual == *expectedContent;
        }
        CloseHandle(old);
        if (!same) return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_INVALID_DATA, targetPath, "Migration source changed");
    }
    auto& index = Detail::IndexFor(root);
    // Refresh the target independently: replacement must charge only the size
    // delta, including when an external process removed or replaced that file.
    Detail::RemoveIndexFile(index, targetPath);
    if (existed) {
        CacheFileInfo old; old.path = targetPath; old.cacheClass = cacheClass;
        old.sizeBytes = (static_cast<uint64_t>(previous.nFileSizeHigh) << 32) | previous.nFileSizeLow;
        old.lastAccessTime = previous.ftLastAccessTime; old.lastWriteTime = previous.ftLastWriteTime; old.creationTime = previous.ftCreationTime;
        Detail::AddIndexFile(index, std::move(old));
    }
    const bool tiered = cacheClass == CacheClass::Thumbnails;
    const bool seek = Detail::IsSeekThumbnail(targetPath);
    auto fits = [&]() {
        auto old = index.files.find(targetPath);
        const auto oldBytes = old == index.files.end() ? 0 : old->sizeBytes;
        return index.total - oldBytes + dataSize <= budgets.totalCeilingBytes &&
            index.classes[cacheClass] - oldBytes + dataSize <= budgets.GetBudget(cacheClass) &&
            (!tiered || (seek ? index.seek : index.hub) - oldBytes + dataSize <= Detail::ThumbnailTierBudget(budgets, seek));
    };
    // A shrinking migration may proceed in an over-budget legacy cache without
    // evicting its neighbours. Ordinary writes always enforce all ceilings.
    if (!expectedContent && !fits()) {
        Detail::IndexFor(root, true);
        if (tiered && dataSize > Detail::ThumbnailTierBudget(budgets, seek))
            return StorageStatus::Error(StorageErrorCode::CacheBudgetExceeded, ERROR_DISK_FULL, targetPath, "Thumbnail exceeds tier allowance");
        Detail::EvictIndexed(index, budgets, root, cacheClass, dataSize, targetPath, tiered);
        if (!fits()) return StorageStatus::Error(StorageErrorCode::CacheBudgetExceeded, ERROR_DISK_FULL, targetPath, "Insufficient cache budget after eviction");
    }
    size_t lastSlash = targetPath.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? targetPath.substr(0, lastSlash) : classDir;
    DWORD dirAttr = GetFileAttributesW(dir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, dir, "Failed to create destination cache directory");
        }
    }
    static std::atomic<uint64_t> s_cacheTempCounter{ 0 };
    std::wstring tmp = targetPath + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                       std::to_wstring(GetCurrentThreadId()) + L"_" + std::to_wstring(++s_cacheTempCounter);
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, err, dir, "Cache directory is read-only or not writable");
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, tmp, "Failed to create temporary cache file for atomic write");
    }
    DWORD written = 0;
    BOOL writeOk = TRUE;
    if (!data.empty()) { writeOk = WriteFile(hFile, data.data(), (DWORD)data.size(), &written, NULL) && (written == (DWORD)data.size()); }
    DWORD writeErr = writeOk ? 0 : GetLastError();
    if (writeOk) { FlushFileBuffers(hFile); }
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        static std::atomic<uint64_t> s_fileTimeCounter{ 0 };
        uint64_t ft = ((static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime) + (++s_fileTimeCounter);
        now.dwLowDateTime = static_cast<DWORD>(ft & 0xFFFFFFFF);
        now.dwHighDateTime = static_cast<DWORD>(ft >> 32);
        if (expectedContent) SetFileTime(hFile, &previous.ftCreationTime, &previous.ftLastAccessTime, &previous.ftLastWriteTime);
        else SetFileTime(hFile, &now, &now, &now);
    CloseHandle(hFile);
    if (!writeOk) {
        DeleteFileW(tmp.c_str());
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, writeErr, tmp, "Failed to write content to temporary cache file");
    }
    if (!MoveFileExW(tmp.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD moveErr = GetLastError();
        DeleteFileW(tmp.c_str());
        return StorageStatus::Error(StorageErrorCode::AtomicRenameFailed, moveErr, targetPath, "Failed to rename temporary cache file to destination");
    }
    Detail::RemoveIndexFile(index, targetPath);
    CacheFileInfo writtenFile; writtenFile.path = targetPath; writtenFile.sizeBytes = dataSize; writtenFile.cacheClass = cacheClass;
    writtenFile.lastAccessTime = expectedContent ? previous.ftLastAccessTime : now;
    writtenFile.lastWriteTime = expectedContent ? previous.ftLastWriteTime : now;
    writtenFile.creationTime = expectedContent ? previous.ftCreationTime : now;
    Detail::AddIndexFile(index, std::move(writtenFile));
    return StorageStatus::Ok(targetPath);
}

inline StorageStatus ReadCacheFile( CacheClass cacheClass, const std::wstring& relativePath, std::string& outData,
    const std::wstring &root = GetExeDir(), bool touchAccessTime = true) {
    outData.clear();
    if (relativePath.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Cache relative path is empty");
    std::wstring targetPath = GetCacheDir(cacheClass, root) + L"\\" + NormalizePathSeparators(relativePath);
    if (!IsCachePathSafe(targetPath, root))
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, targetPath, "Target cache path is unsafe or outside cache root");
    HANDLE hFile = CreateFileW( targetPath.c_str(), GENERIC_READ | (touchAccessTime ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE && touchAccessTime) {
        hFile = CreateFileW( targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, targetPath, "Failed to open cache file for reading");
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, err, targetPath, "Failed to get cache file size");
    }
    if (size.QuadPart > 256 * 1024 * 1024) {
        CloseHandle(hFile);
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_FILE_TOO_LARGE, targetPath, "Cache file exceeds single read limit");
    }
    if (size.QuadPart == 0) { CloseHandle(hFile); return StorageStatus::Ok(targetPath); }
    outData.resize((size_t)size.QuadPart);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &outData[0], (DWORD)size.QuadPart, &bytesRead, NULL) || bytesRead != (DWORD)size.QuadPart) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        outData.clear();
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, err, targetPath, "Failed to read cache file content");
    }
    if (touchAccessTime) { FILETIME now; GetSystemTimeAsFileTime(&now); SetFileTime(hFile, nullptr, &now, nullptr); }
    CloseHandle(hFile);
    return StorageStatus::Ok(targetPath);
}

inline StorageStatus ClearDisposableCache(const std::wstring& root = GetExeDir(), std::optional<CacheClass> targetClass = std::nullopt) {
    std::lock_guard<std::recursive_mutex> ioLock(Detail::CacheIOMutex());
    InvalidateCacheAccounting(root);
    if (root.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Invalid root path for cache cleanup");
    std::wstring cacheDir = root + L"\\cache";
    std::wstring dataDir = root + L"\\data";
    if (!IsPathUnderRoot(cacheDir, root) || _wcsicmp(cacheDir.c_str(), dataDir.c_str()) == 0 ||
        _wcsicmp(cacheDir.c_str(), root.c_str()) == 0) {
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, cacheDir, "Invalid cache directory for cleanup");
    }
    auto removeDirContents = [](const std::wstring& dirPath, const auto& selfRef) -> bool {
        DWORD attr = GetFileAttributesW(dirPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
        std::wstring searchPattern = dirPath + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return true;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring itemPath = dirPath + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) { SetFileAttributesW(itemPath.c_str(), FILE_ATTRIBUTE_NORMAL); }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                selfRef(itemPath, selfRef);
                RemoveDirectoryW(itemPath.c_str());
            } else { DeleteFileW(itemPath.c_str()); }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        return true;
    };
    if (targetClass.has_value()) {
        std::wstring subDir = cacheDir + L"\\" + GetCacheClassSubDirName(*targetClass);
        removeDirContents(subDir, removeDirContents);
    } else {
        removeDirContents(cacheDir, removeDirContents);
        std::wstring legacyShaderCache = dataDir + L"\\shader-cache";
        DWORD legacyAttr = GetFileAttributesW(legacyShaderCache.c_str());
        if (legacyAttr != INVALID_FILE_ATTRIBUTES && (legacyAttr & FILE_ATTRIBUTE_DIRECTORY)) {
            removeDirContents(legacyShaderCache, removeDirContents);
            RemoveDirectoryW(legacyShaderCache.c_str());
        }
    }
    return StorageStatus::Ok(cacheDir);
}

} // namespace Aperture
