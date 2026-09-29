#pragma once

// Rotating backup creation and recovery for durable JSON stores.

#include "AppIncludes.h"
#include "PathFileHelpers.h"
#include "StorageStatus.h"
#include <string>
#include <vector>

namespace Aperture {

inline StorageStatus CreateRotatingBackup(const std::wstring& filePath, size_t maxBackups = 3, const std::wstring& customBackupDir = L"") {
    if (filePath.empty()) return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, filePath, "Empty file path");
    if (!FileExists(filePath)) return StorageStatus::Ok(filePath);
    std::wstring backupDir = customBackupDir;
    if (backupDir.empty()) {
        size_t lastSlash = filePath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring parentDir = filePath.substr(0, lastSlash);
            backupDir = parentDir + L"\\backups";
        } else { backupDir = GetBackupDir(); }
    }
    DWORD attr = GetFileAttributesW(backupDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, backupDir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, backupDir, "Failed to create backup directory");
        }
    }
    size_t lastSlash = filePath.find_last_of(L"\\/");
    std::wstring fileName = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
    if (fileName.empty()) return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, filePath, "Invalid file name");
    if (maxBackups < 1) maxBackups = 1;
    if (maxBackups > 20) maxBackups = 20;
    for (size_t i = maxBackups; i >= 2; --i) {
        std::wstring dst = backupDir + L"\\" + fileName + L".bak." + std::to_wstring(i);
        std::wstring src = backupDir + L"\\" + fileName + L".bak." + std::to_wstring(i - 1);
        DWORD srcAttr = GetFileAttributesW(src.c_str());
        if (srcAttr != INVALID_FILE_ATTRIBUTES && !(srcAttr & FILE_ATTRIBUTE_DIRECTORY))
            MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    std::wstring bak1 = backupDir + L"\\" + fileName + L".bak.1";
    if (!CopyFileW(filePath.c_str(), bak1.c_str(), FALSE)) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, err, bak1, "Failed to copy file to backup");
    }
    // Byte ceiling in addition to the count limit: a multi-megabyte library could otherwise make 3x backups dominate the portable folder.
    // Cap the entire backups directory (default 32 MiB), pruning oldest first. Live
    // files are untouched; only recovery copies are removed.
    {
        constexpr uint64_t kBackupDirByteCeiling = 32ull * 1024ull * 1024ull;
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW((backupDir + L"\\*.bak.*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            struct Entry { std::wstring path; uint64_t writeFt; uint64_t bytes; };
            std::vector<Entry> entries;
            uint64_t total = 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                uint64_t ft = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                total += bytes;
                entries.push_back({ backupDir + L"\\" + fd.cFileName, ft, bytes });
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
            if (total > kBackupDirByteCeiling && !entries.empty()) {
                std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.writeFt < b.writeFt; });
                for (const auto& e : entries) {
                    if (total <= kBackupDirByteCeiling) break;
                    if (DeleteFileW(e.path.c_str())) { total = (e.bytes > total) ? 0 : (total - e.bytes); }
                }
            }
        }
    }
    return StorageStatus::Ok(bak1);
}

inline StorageStatus RestoreFromNewestValidBackup( const std::wstring& filePath, std::string& outText,
    const std::function<bool(const std::string&)>& validator = nullptr, size_t maxBackups = 5, const std::wstring& customBackupDir = L"")
{
    outText.clear();
    std::wstring backupDir = customBackupDir;
    if (backupDir.empty()) {
        size_t lastSlash = filePath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            std::wstring parentDir = filePath.substr(0, lastSlash);
            backupDir = parentDir + L"\\backups";
        } else { backupDir = GetBackupDir(); }
    }
    size_t lastSlash = filePath.find_last_of(L"\\/");
    std::wstring fileName = (lastSlash != std::wstring::npos) ? filePath.substr(lastSlash + 1) : filePath;
    if (fileName.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, filePath, "Invalid file name");
    for (size_t i = 1; i <= maxBackups; ++i) {
        std::wstring bak = backupDir + L"\\" + fileName + L".bak." + std::to_wstring(i);
        DWORD attr = GetFileAttributesW(bak.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            std::string content;
            StorageStatus readStatus = ReadTextFile(bak, content);
            if (readStatus.is_ok() && !content.empty()) {
                if (!validator || validator(content)) { outText = std::move(content); return StorageStatus::Ok(bak); }
            }
        }
    }
    return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_FILE_NOT_FOUND, filePath, "No valid backup found");
}

} // namespace Aperture
