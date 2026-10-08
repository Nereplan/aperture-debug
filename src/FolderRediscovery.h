#pragma once

#include "MediaLibrary.h"
#include "OperationCancellation.h"
#include <unordered_set>

namespace Aperture {

struct RediscoveryFile {
    std::string id;
    std::wstring path;
    uint64_t size = 0, fileIndex = 0;
    uint32_t volume = 0;
    bool Matches(const MediaItem& item) const {
        return item.id == id && NormalizePathKey(item.filePath) == NormalizePathKey(path) &&
            item.fileSizeBytes == size && item.fileIndex == fileIndex && item.volumeSerialNumber == volume;
    }
    static RediscoveryFile Capture(const MediaItem& item) {
        return {item.id, item.filePath, item.fileSizeBytes, item.fileIndex, item.volumeSerialNumber};
    }
};

struct FolderRediscoverySnapshot {
    std::vector<RediscoveryFile> files;
    std::vector<std::wstring> watchedFolders;
    static FolderRediscoverySnapshot Capture(const MediaLibrary& library) {
        FolderRediscoverySnapshot snapshot;
        snapshot.files.reserve(library.mediaItems.size());
        for (const auto& item : library.mediaItems) snapshot.files.push_back(RediscoveryFile::Capture(item));
        snapshot.watchedFolders = library.watchedFolders;
        return snapshot;
    }
};

struct PreparedFolderRediscovery {
    struct Move {
        RediscoveryFile source;
        std::optional<RediscoveryFile> destination;
        std::wstring path;
        uint64_t size = 0;
        WindowsFileIdentity identity;
        size_t folder = 0;
    };
    std::vector<std::pair<std::wstring, std::wstring>> folders;
    std::vector<Move> moves;
    StorageStatus status = StorageStatus::Ok();
    size_t directoryScans = 0;
    bool canceled = false;
};

// Discovery owns only file facts. All user-state reconciliation happens later
// against the current library. Candidate directories are enumerated once and
// indexed by filename instead of issuing a file-stat call for every pair.
inline PreparedFolderRediscovery PrepareFolderRediscovery(const FolderRediscoverySnapshot& snapshot,
    const std::shared_ptr<OperationCancellation>& cancellation = {}) {
    PreparedFolderRediscovery result;
    const auto canceled = [&] {
        if (cancellation && cancellation->IsCancelled()) result.canceled = true;
        return result.canceled;
    };
    const auto error = [&](DWORD code, const std::wstring& path) {
        if (result.status.is_ok()) result.status = StorageStatus::Error(StorageErrorCode::FileReadFailed, code, path,
            "Folder rediscovery was incomplete; unconfirmed records retained");
    };
    const auto nameKey = [](std::wstring name) {
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        return name;
    };
    struct MissingFolder { std::wstring path; std::vector<size_t> files; };
    std::unordered_map<std::wstring, MissingFolder> missing;
    std::unordered_map<std::wstring, size_t> libraryByPath;
    libraryByPath.reserve(snapshot.files.size());
    std::unordered_set<std::wstring> wantedNames;
    for (size_t i = 0; i < snapshot.files.size() && !canceled(); ++i) {
        const auto& item = snapshot.files[i];
        libraryByPath[NormalizePathKey(item.path)] = i;
        const auto availability = QueryPathAvailability(item.path);
        if (availability == PathAvailability::Unavailable) { error(ERROR_NOT_READY, item.path); continue; }
        if (availability != PathAvailability::Missing) continue;
        const auto directory = ExtractDirectory(item.path);
        if (directory.empty()) continue;
        auto& group = missing[NormalizePathKey(directory)];
        group.path = directory;
        group.files.push_back(i);
        wantedNames.insert(nameKey(GetFileNameFromPath(item.path)));
    }
    if (canceled() || missing.empty()) return result;
    struct DiskFile { std::wstring path; uint64_t size = 0; WindowsFileIdentity identity; bool identityKnown = false; };
    struct Listing { bool complete = true; std::unordered_map<std::wstring, DiskFile> files; };
    std::unordered_map<std::wstring, std::shared_ptr<Listing>> listings;
    size_t cachedFileCount = 0;
    const auto readDirectory = [&](const std::wstring& directory) {
        const auto key = NormalizePathKey(directory);
        const auto cached = listings.find(key);
        if (cached != listings.end()) return cached->second;
        auto listing = std::make_shared<Listing>();
        auto prefix = ResolveAppPath(directory);
        if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
        WIN32_FIND_DATAW data{};
        ++result.directoryScans;
        HANDLE find = FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            listing->complete = code == ERROR_FILE_NOT_FOUND;
            if (!listing->complete) error(code, directory);
        } else {
            struct Closer { HANDLE handle; ~Closer() { FindClose(handle); } } closer{find};
            do {
                if (canceled()) { listing->complete = false; break; }
                if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                auto name = nameKey(data.cFileName);
                if (!wantedNames.count(name)) continue;
                listing->files.emplace(std::move(name), DiskFile{prefix + data.cFileName,
                    (uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow});
            } while (FindNextFileW(find, &data));
            if (!result.canceled && GetLastError() != ERROR_NO_MORE_FILES) {
                listing->complete = false; error(GetLastError(), directory);
            }
        }
        // Bound retained cross-folder lookup data. A very large directory is
        // still processed fully, but does not displace a second copy in cache.
        constexpr size_t maxCachedFiles = 65536, maxCachedDirectories = 128;
        if (listing->files.size() <= maxCachedFiles) {
            if (cachedFileCount + listing->files.size() > maxCachedFiles || listings.size() >= maxCachedDirectories) {
                listings.clear(); cachedFileCount = 0;
            }
            cachedFileCount += listing->files.size();
            listings.emplace(key, listing);
        }
        return listing;
    };
    struct Siblings { bool complete = true; std::vector<std::wstring> paths; };
    std::unordered_map<std::wstring, Siblings> siblingCache;
    for (const auto& pair : missing) {
        if (canceled()) break;
        const auto& oldKey = pair.first;
        const auto& group = pair.second;
        const auto parent = ExtractDirectory(group.path);
        const auto parentKey = NormalizePathKey(parent);
        auto siblings = siblingCache.find(parentKey);
        if (siblings == siblingCache.end()) {
            Siblings found;
            auto prefix = parent;
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
            WIN32_FIND_DATAW data{};
            HANDLE find = parent.empty() ? INVALID_HANDLE_VALUE : FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic,
                &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find != INVALID_HANDLE_VALUE) {
                struct Closer { HANDLE handle; ~Closer() { FindClose(handle); } } closer{find};
                do {
                    if (canceled()) break;
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) found.paths.push_back(prefix + data.cFileName);
                } while (FindNextFileW(find, &data));
                if (!result.canceled && GetLastError() != ERROR_NO_MORE_FILES) { found.complete = false; error(GetLastError(), parent); }
            } else if (!parent.empty()) {
                const DWORD code = GetLastError();
                // A removed parent may still be relocated into a watched root.
                if (code != ERROR_PATH_NOT_FOUND && code != ERROR_FILE_NOT_FOUND) { found.complete = false; error(code, parent); }
            }
            if (siblingCache.size() >= 128) siblingCache.clear();
            siblings = siblingCache.emplace(parentKey, std::move(found)).first;
        }
        if (!siblings->second.complete || canceled()) continue;
        auto candidates = siblings->second.paths;
        candidates.insert(candidates.end(), snapshot.watchedFolders.begin(), snapshot.watchedFolders.end());
        std::unordered_set<std::wstring> seen;
        size_t bestCount = 0, bestIdentities = 0;
        bool ambiguous = false;
        std::wstring bestDirectory;
        std::vector<PreparedFolderRediscovery::Move> bestMoves;
        for (const auto& directory : candidates) {
            if (canceled()) break;
            const auto key = NormalizePathKey(directory);
            if (key == oldKey || !seen.insert(key).second) continue;
            auto listing = readDirectory(directory);
            if (!listing->complete) continue;
            size_t identities = 0;
            std::vector<PreparedFolderRediscovery::Move> moves;
            for (size_t index : group.files) {
                if (canceled()) break;
                const auto& source = snapshot.files[index];
                const auto entry = listing->files.find(nameKey(GetFileNameFromPath(source.path)));
                if (entry == listing->files.end()) continue;
                auto& target = entry->second;
                if (!target.identityKnown) { target.identity = GetWindowsFileIdentity(target.path); target.identityKnown = true; }
                const bool sameIdentity = source.fileIndex && target.identity.valid && source.fileIndex == target.identity.fileIndex &&
                    source.volume == target.identity.volumeSerialNumber;
                const bool conflictingIdentity = source.fileIndex && target.identity.valid &&
                    source.volume == target.identity.volumeSerialNumber && !sameIdentity;
                if (!sameIdentity && (conflictingIdentity || (source.size && source.size != target.size))) continue;
                identities += sameIdentity;
                PreparedFolderRediscovery::Move move;
                move.source = source; move.path = target.path; move.size = target.size; move.identity = target.identity;
                const auto existing = libraryByPath.find(NormalizePathKey(target.path));
                if (existing != libraryByPath.end()) move.destination = snapshot.files[existing->second];
                moves.push_back(std::move(move));
            }
            if (moves.empty() || static_cast<double>(moves.size()) / group.files.size() < 0.70) continue;
            if (moves.size() > bestCount || (moves.size() == bestCount && identities > bestIdentities)) {
                bestCount = moves.size(); bestIdentities = identities; bestDirectory = directory;
                bestMoves = std::move(moves); ambiguous = false;
            } else if (moves.size() == bestCount && identities == bestIdentities) ambiguous = true;
        }
        if (canceled() || ambiguous || bestMoves.empty()) continue;
        const size_t folder = result.folders.size();
        result.folders.emplace_back(group.path, bestDirectory);
        for (auto& move : bestMoves) {
            if (canceled()) break;
            // Recheck disappearance after candidate I/O, before proposing a move.
            if (QueryPathAvailability(move.source.path) != PathAvailability::Missing) continue;
            move.folder = folder;
            result.moves.push_back(std::move(move));
        }
    }
    // Two missing records claiming the same destination are ambiguous too.
    // Do not let processing order choose which user's record is discarded.
    std::unordered_map<std::wstring, size_t> claims;
    for (const auto& move : result.moves) ++claims[NormalizePathKey(move.path)];
    result.moves.erase(std::remove_if(result.moves.begin(), result.moves.end(), [&](const auto& move) {
        return claims[NormalizePathKey(move.path)] > 1;
    }), result.moves.end());
    return result;
}

} // namespace Aperture
