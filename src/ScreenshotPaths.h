#pragma once

// Screenshot directory resolution, filename generation, and path-containment checks.

#include "AppIncludes.h"
#include "PathFileHelpers.h"
#include "StorageStatus.h"
#include <string>

namespace Aperture {

// --- Screenshot Path & Safety Helpers ---
inline std::wstring GetScreenshotsDir(const std::wstring& root = GetExeDir()) {
    std::wstring r = root.empty() ? GetExeDir() : root;
    return ResolveAppPath(r + L"\\screenshots", r);
}

inline std::wstring SanitizeMediaStemForFilename(const std::wstring& rawStem) {
    if (rawStem.empty()) return L"screenshot";
    std::wstring title = GetFileTitleFromPath(rawStem);
    if (title.empty()) title = rawStem;
    std::wstring sanitized;
    sanitized.reserve(title.size());
    for (wchar_t ch : title) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            sanitized.push_back(L'_');
        } else { sanitized.push_back(ch); }
    }
    size_t start = 0;
    while (start < sanitized.size() && (sanitized[start] == L' ' || sanitized[start] == L'\t')) { start++; }
    size_t end = sanitized.size();
    while (end > start && (sanitized[end - 1] == L' ' || sanitized[end - 1] == L'\t' || sanitized[end - 1] == L'.')) { end--; }
    if (start >= end) { return L"screenshot"; }
    std::wstring result = sanitized.substr(start, end - start);
    std::wstring upper = result;
    for (auto& c : upper) c = (wchar_t)towupper(c);
    static const wchar_t *const kReservedNames[] = {L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4",
                                                    L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
                                                    L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* res : kReservedNames) {
        if (upper == res) { result = L"_" + result; break; }
    }
    if (result.size() > 120) {
        result.resize(120);
        while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) { result.pop_back(); }
        if (result.empty()) result = L"screenshot";
    }
    return result;
}

inline std::wstring FormatScreenshotTimestamp(const SYSTEMTIME* customTime = nullptr) {
    SYSTEMTIME st;
    if (customTime) { st = *customTime; } else { GetLocalTime(&st); }
    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

inline bool IsScreenshotPathSafe(const std::wstring& path, const std::wstring& root = GetExeDir()) {
    if (path.empty() || root.empty()) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == L':') { if (i != 1) return false; }
        if (path[i] == L'\0') return false;
    }
    std::wstring resolvedRoot = ResolveAppPath(root, root);
    std::wstring screenshotsDir = ResolveAppPath(root + L"\\screenshots", root);
    std::wstring dataDir = ResolveAppPath(root + L"\\data", root);
    std::wstring cacheDir = ResolveAppPath(root + L"\\cache", root);
    std::wstring resolvedPath = ResolveAppPath(path, root);
    while (resolvedRoot.size() > 3 && (resolvedRoot.back() == L'\\' || resolvedRoot.back() == L'/')) { resolvedRoot.pop_back(); }
    while (screenshotsDir.size() > 3 && (screenshotsDir.back() == L'\\' || screenshotsDir.back() == L'/')) { screenshotsDir.pop_back(); }
    while (dataDir.size() > 3 && (dataDir.back() == L'\\' || dataDir.back() == L'/')) { dataDir.pop_back(); }
    while (cacheDir.size() > 3 && (cacheDir.back() == L'\\' || cacheDir.back() == L'/')) { cacheDir.pop_back(); }
    if (_wcsicmp(resolvedPath.c_str(), resolvedRoot.c_str()) == 0 || _wcsicmp(resolvedPath.c_str(), screenshotsDir.c_str()) == 0 ||
        _wcsicmp(resolvedPath.c_str(), dataDir.c_str()) == 0 || _wcsicmp(resolvedPath.c_str(), cacheDir.c_str()) == 0) {
        return false;
    }
    std::wstring screenshotsPrefix = screenshotsDir + L"\\";
    if (resolvedPath.size() <= screenshotsPrefix.size()) return false;
    if (_wcsnicmp(resolvedPath.c_str(), screenshotsPrefix.c_str(), screenshotsPrefix.size()) != 0) { return false; }
    std::wstring dataPrefix = dataDir + L"\\";
    if (resolvedPath.size() >= dataPrefix.size() && _wcsnicmp(resolvedPath.c_str(), dataPrefix.c_str(), dataPrefix.size()) == 0)
        return false;
    std::wstring cachePrefix = cacheDir + L"\\";
    if (resolvedPath.size() >= cachePrefix.size() && _wcsnicmp(resolvedPath.c_str(), cachePrefix.c_str(), cachePrefix.size()) == 0)
        return false;
    return IsPathUnderRoot(resolvedPath, root);
}

inline StorageStatus CheckScreenshotsDirectoryWritability(const std::wstring& root = GetExeDir(), bool createIfMissing = false) {
    if (root.empty()) { return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, L"", "Root path is empty"); }
    std::wstring dir = ResolveAppPath(root + L"\\screenshots", root);
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!createIfMissing) {
            DWORD rootAttr = GetFileAttributesW(root.c_str());
            if (rootAttr == INVALID_FILE_ATTRIBUTES || (rootAttr & FILE_ATTRIBUTE_READONLY))
                return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, root, "Root directory is read-only or not found");
            std::wstring rootProbe = root + L"\\.writetest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
            HANDLE hFile = CreateFileW(rootProbe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, err, root, "Root directory is not writable");
            }
            CloseHandle(hFile);
            return StorageStatus::Ok(dir);
        }
        int res = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, dir, "Failed to create screenshots directory");
        }
        attr = GetFileAttributesW(dir.c_str());
    }
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, dir, "Screenshots directory has read-only attribute");
    std::wstring probe = dir + L"\\.writetest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE hFile = CreateFileW(probe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, err, dir, "Screenshots directory is read-only or not writable");
    }
    CloseHandle(hFile);
    return StorageStatus::Ok(dir);
}

inline StorageStatus EnsureScreenshotsDirectoryCreated(const std::wstring& root = GetExeDir()) {
    return CheckScreenshotsDirectoryWritability(root, true);
}

inline std::wstring GenerateScreenshotFilePath( const std::wstring& mediaPathOrTitle, const std::wstring& root = GetExeDir(),
    const SYSTEMTIME* customTime = nullptr, std::function<bool(const std::wstring&)> fileExistsFn = nullptr,
    const std::wstring& extension = L".png")
{
    std::wstring stem = SanitizeMediaStemForFilename(mediaPathOrTitle);
    std::wstring timestamp = FormatScreenshotTimestamp(customTime);
    std::wstring screenshotsDir = GetScreenshotsDir(root);
    std::wstring baseName = stem + L"_" + timestamp;
    const std::wstring ext = (!extension.empty() && extension.front() == L'.') ? extension : L"." + extension;
    std::wstring candidate = screenshotsDir + L"\\" + baseName + ext;
    auto checkExists = [&](const std::wstring& p) -> bool { if (fileExistsFn) return fileExistsFn(p); return FileExists(p); };
    if (!checkExists(candidate)) { return candidate; }
    for (int suffix = 1; suffix <= 9999; ++suffix) {
        wchar_t buf[16];
        swprintf_s(buf, L"_%02d%s", suffix, ext.c_str());
        std::wstring suffixed = screenshotsDir + L"\\" + baseName + buf;
        if (!checkExists(suffixed)) { return suffixed; }
    }
    return candidate;
}

} // namespace Aperture
