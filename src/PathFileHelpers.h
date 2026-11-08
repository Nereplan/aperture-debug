#pragma once

// Executable-relative path resolution, path keys, and file query helpers.

#include "AppIncludes.h"
#include "StorageStatus.h"
#include <optional>
#include <string>
#include <vector>

namespace Aperture {

// --- Path & File Helpers ---
enum class PathAvailability { Present, Missing, Unavailable };
struct PathAvailabilityObservation {
    PathAvailability value = PathAvailability::Unavailable;
    ULONGLONG checkedAtTick = 0;
    bool known = false, isDirectory = false;
};
inline PathAvailability QueryPathAvailability(const std::wstring& path, DWORD* error = nullptr, DWORD* attributes = nullptr) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    const DWORD err = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (error) *error = err;
    if (attributes) *attributes = attr;
    if (attr != INVALID_FILE_ATTRIBUTES) return PathAvailability::Present;
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND
        ? PathAvailability::Missing : PathAvailability::Unavailable;
}

inline std::wstring GetFullPathDynamic(const std::wstring& path) {
    if (path.empty()) return {};
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring fullPath(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, fullPath.data(), nullptr);
    if (written == 0 || written >= required) return path;
    fullPath.resize(written);
    return fullPath;
}

inline std::wstring GetExePath() {
    std::wstring buffer(512, L'\0');
    while (buffer.size() <= 32768) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1 || (length < buffer.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
    }
    return {};
}

inline const std::wstring& GetExeDir() {
    // The executable location is fixed for the process lifetime. Resolve once;
    // this function is a default argument on ResolveAppPath and friends, so it
    // sits under every path-key normalization, lookup, and save.
    static const std::wstring cached = []() {
        std::wstring path = GetExePath();
        if (path.empty()) return std::wstring(L".");
        const size_t pos = path.find_last_of(L"\\/");
        std::wstring dir = pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos);
        while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) { dir.pop_back(); }
#if !defined(_WIN32)
        // Hybrid portable + XDG mode on Linux: use the directory beside the binary when portable
        // (e.g. ./data exists or not installed under /usr or /bin); fall back to XDG_DATA_HOME/aperture when installed system-wide.
        std::wstring localData = dir + L"/data";
        DWORD localAttr = GetFileAttributesW(localData.c_str());
        bool hasPortableData = (localAttr != INVALID_FILE_ATTRIBUTES && (localAttr & FILE_ATTRIBUTE_DIRECTORY));
        bool isSystemBin = (dir.rfind(L"/usr", 0) == 0 || dir.rfind(L"/bin", 0) == 0 || dir.rfind(L"/opt", 0) == 0);
        if (!hasPortableData && isSystemBin) {
            wchar_t xdgBuf[1024] = {};
            if (GetEnvironmentVariableW(L"XDG_DATA_HOME", xdgBuf, 1024) > 0 && xdgBuf[0] != L'\0') {
                dir = std::wstring(xdgBuf) + L"/aperture";
            } else if (GetEnvironmentVariableW(L"HOME", xdgBuf, 1024) > 0 && xdgBuf[0] != L'\0') {
                dir = std::wstring(xdgBuf) + L"/.local/share/aperture";
            }
            SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        }
#endif
        return dir;
    }();
    return cached;
}

inline std::wstring GetDataDir() {
    std::wstring dir = GetExeDir() + L"\\data";
    const DWORD rootAttributes = GetFileAttributesW(GetExeDir().c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)) return dir;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { SHCreateDirectoryExW(NULL, dir.c_str(), NULL); }
    return dir;
}

inline StorageStatus CheckPortableRootAvailability(const std::wstring& rootDir) {
    const DWORD attributes = GetFileAttributesW(rootDir.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return StorageStatus::Ok(rootDir);
    return StorageStatus::Error(StorageErrorCode::FileReadFailed,
        attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY,
        rootDir, "Portable data root is unavailable; existing stores are protected");
}
inline uint64_t Fnv1a64(const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) { hash ^= static_cast<uint64_t>(ptr[i]); hash *= 1099511628211ULL; }
    return hash;
}

inline std::wstring NormalizePathSeparators(const std::wstring& path) {
    std::wstring result = path;
    for (auto& c : result) { if (c == L'/') c = L'\\'; }
    return result;
}

inline std::wstring ExtractDirectory(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) return L"";
    return path.substr(0, lastSlash);
}

// Delay-loaded system DLL calls can raise MSVC's module/procedure-not-found structured exceptions if the DLL changes between the presence
// check and the call. Handle only those expected loader failures and let unrelated faults continue to the normal crash handler.
inline int DelayLoadExceptionFilter(unsigned int exceptionCode) noexcept {
    constexpr unsigned int kDelayLoadModuleNotFound = 0xC06D007Eu;
    constexpr unsigned int kDelayLoadProcedureNotFound = 0xC06D007Fu;
    return (exceptionCode == kDelayLoadModuleNotFound || exceptionCode == kDelayLoadProcedureNotFound) ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

inline std::wstring GetFullPathNormalized(const std::wstring& input) {
    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) return {};
    std::wstring full(static_cast<size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), required, full.data(), nullptr);
    if (written == 0 || written >= required) return {};
    full.resize(static_cast<size_t>(written));
    return NormalizePathSeparators(full);
}

inline std::wstring ResolveAppPath(const std::wstring& path, const std::wstring& baseDir = GetExeDir()) {
    if (path.empty()) return L"";
    std::wstring p = NormalizePathSeparators(path);
    bool isAbsolute = (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') ||
                      (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
#if !defined(_WIN32)
    if (!p.empty() && p[0] == L'\\') isAbsolute = true;
#endif
    // Fast path: an absolute path with no dot directory components and no
    // trailing separator is already fully qualified. GetFullPathNameW would
    // return it unchanged; skipping the call keeps per-item normalization free
    // of kernel calls on large libraries.
    if (isAbsolute && p.find(L"\\.", 1) == std::wstring::npos && p.find(L"/.", 1) == std::wstring::npos) {
        const wchar_t last = p.back();
        if (last != L'\\' && last != L'/' && last != L'.') return p;
    }
    if (p.size() >= 2 && p[0] == L'.' && p[1] == L'\\') {
        std::wstring combined = baseDir + L"\\" + p.substr(2);
        std::wstring full = GetFullPathNormalized(combined);
        return !full.empty() ? full : NormalizePathSeparators(combined);
    }
    if (!isAbsolute) {
        std::wstring combined = baseDir + L"\\" + p;
        std::wstring full = GetFullPathNormalized(combined);
        return !full.empty() ? full : NormalizePathSeparators(combined);
    }
    std::wstring full = GetFullPathNormalized(p);
    return !full.empty() ? full : p;
}

inline std::wstring ToRelocatablePath(const std::wstring& path, const std::wstring& baseDir = GetExeDir()) {
    if (path.empty()) return L"";
    std::wstring resolved = ResolveAppPath(path, baseDir);
    std::wstring resolvedBase = ResolveAppPath(baseDir, baseDir);
    while (resolvedBase.size() > 3 && resolvedBase.back() == L'\\') { resolvedBase.pop_back(); }
    std::wstring prefix = resolvedBase + L"\\";
    if (resolved.size() >= prefix.size() && _wcsnicmp(resolved.c_str(), prefix.c_str(), prefix.size()) == 0)
        return L".\\" + resolved.substr(prefix.size());
    return resolved;
}

inline std::wstring NormalizePathKey(const std::wstring& input) {
    if (input.empty()) return {};
    std::wstring p = ResolveAppPath(input);
    for (auto& c : p) { if (c == L'/') c = L'\\'; c = (wchar_t)towlower(c); }
    while (p.size() > 3 && !p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}
inline bool IsPathUnderRoot(const std::wstring& path, const std::wstring& root = GetExeDir()) {
    if (path.empty() || root.empty()) return false;
    std::wstring resolvedPath = ResolveAppPath(path, root);
    std::wstring resolvedRoot = ResolveAppPath(root, root);
    while (resolvedRoot.size() > 3 && (resolvedRoot.back() == L'\\' || resolvedRoot.back() == L'/')) { resolvedRoot.pop_back(); }
    std::wstring prefix = resolvedRoot + L"\\";
    if (resolvedPath.size() >= prefix.size() && _wcsnicmp(resolvedPath.c_str(), prefix.c_str(), prefix.size()) == 0) { return true; }
    return _wcsicmp(resolvedPath.c_str(), resolvedRoot.c_str()) == 0;
}

inline bool IsCachePathSafe(const std::wstring& path, const std::wstring& root = GetExeDir()) {
    if (path.empty() || root.empty()) return false;
    // Reject NT stream separators and null characters
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == L':') { if (i != 1) return false; }
    }
    std::wstring resolvedRoot = ResolveAppPath(root, root);
    std::wstring cacheDir = ResolveAppPath(root + L"\\cache", root);
    std::wstring dataDir = ResolveAppPath(root + L"\\data", root);
    std::wstring resolvedPath = ResolveAppPath(path, root);
    while (resolvedRoot.size() > 3 && (resolvedRoot.back() == L'\\' || resolvedRoot.back() == L'/')) { resolvedRoot.pop_back(); }
    while (cacheDir.size() > 3 && (cacheDir.back() == L'\\' || cacheDir.back() == L'/')) { cacheDir.pop_back(); }
    while (dataDir.size() > 3 && (dataDir.back() == L'\\' || dataDir.back() == L'/')) { dataDir.pop_back(); }
    // Must not equal root, cacheDir itself, or dataDir
    if (_wcsicmp(resolvedPath.c_str(), resolvedRoot.c_str()) == 0 || _wcsicmp(resolvedPath.c_str(), cacheDir.c_str()) == 0 ||
        _wcsicmp(resolvedPath.c_str(), dataDir.c_str()) == 0) {
        return false;
    }
    // Must be strictly inside cacheDir
    std::wstring cachePrefix = cacheDir + L"\\";
    if (resolvedPath.size() <= cachePrefix.size()) return false;
    if (_wcsnicmp(resolvedPath.c_str(), cachePrefix.c_str(), cachePrefix.size()) != 0) { return false; }
    // Must NOT be inside dataDir
    std::wstring dataPrefix = dataDir + L"\\";
    if (resolvedPath.size() >= dataPrefix.size() && _wcsnicmp(resolvedPath.c_str(), dataPrefix.c_str(), dataPrefix.size()) == 0)
        return false;
    return true;
}

inline bool FileExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

// Delay-load gate: system-only presence check for an optionally delay-loaded system DLL. Uses LOAD_LIBRARY_SEARCH_SYSTEM32 so
// resolution can never pick up an app-local or CWD impostor. Returns false (feature must take its graceful fallback) without touching the
// delay import, so no SEH can fire. Call at feature entries before any delay-loaded API.
inline bool SystemDllPresent(const wchar_t* systemDllName) {
    if (!systemDllName || !*systemDllName) return false;
    HMODULE h = LoadLibraryExW(systemDllName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) { FreeLibrary(h); return true; }
    return false;
}

inline bool DirectoryExists(const std::wstring& path) {
    DWORD dwAttrib = GetFileAttributesW(path.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

inline std::wstring GetFileTitleFromPath(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring filename = (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
    size_t lastDot = filename.find_last_of(L".");
    if (lastDot != std::wstring::npos && lastDot > 0) { return filename.substr(0, lastDot); }
    return filename;
}

inline std::wstring GetFileNameFromPath(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    return (lastSlash != std::wstring::npos) ? path.substr(lastSlash + 1) : path;
}

inline std::wstring GetFileExtension(const std::wstring& path) {
    size_t lastDot = path.find_last_of(L".");
    if (lastDot != std::wstring::npos) { std::wstring ext = path.substr(lastDot); for (auto& ch : ext) ch = towlower(ch); return ext; }
    return L"";
}

inline uint64_t GetFileSizeInBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        return (uint64_t)size.QuadPart;
    }
    return 0;
}
struct WindowsFileIdentity { uint64_t fileIndex = 0; uint32_t volumeSerialNumber = 0; bool valid = false; };

inline WindowsFileIdentity GetWindowsFileIdentity(const std::wstring& path) {
    WindowsFileIdentity id;
    if (path.empty()) return id;
    HANDLE hFile = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        if (GetFileInformationByHandle(hFile, &info)) {
            id.volumeSerialNumber = info.dwVolumeSerialNumber;
            id.fileIndex = ((uint64_t)info.nFileIndexHigh << 32) | (uint64_t)info.nFileIndexLow;
            id.valid = true;
        }
        CloseHandle(hFile);
    }
    return id;
}


// Generic durable text-file IO and the data-directory backup location (shared
// by settings, library, session, and backup helpers).

inline std::wstring GetBackupDir() {
    std::wstring dir = GetDataDir() + L"\\backups";
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { SHCreateDirectoryExW(NULL, dir.c_str(), NULL); }
    return dir;
}

inline StorageStatus CheckDataDirectoryWritability(const std::wstring& rootDir = GetExeDir()) {
    StorageStatus rootStatus = CheckPortableRootAvailability(rootDir);
    if (!rootStatus.is_ok()) return rootStatus;
    std::wstring dir = rootDir + L"\\data";
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, dir, "Failed to create data directory");
        }
    }
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, dir, "Data directory has read-only attribute");
    std::wstring probe = dir + L"\\.writetest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE hFile = CreateFileW(probe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, err, dir, "Data directory is read-only or not writable");
    }
    CloseHandle(hFile);
    return StorageStatus::Ok(dir);
}

inline StorageStatus AtomicWriteTextFile(const std::wstring& path, const std::string& text,
                                       BY_HANDLE_FILE_INFORMATION* committedInfo = nullptr) {
    if (committedInfo) *committedInfo = {};
    if (text.size() > kMaxPortableStoreBytes)
        return StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_FILE_TOO_LARGE, path, "Output exceeds the readable store size limit");
    if (path.empty()) { return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, path, "Path is empty"); }
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? path.substr(0, lastSlash) : L".";
    DWORD dirAttr = GetFileAttributesW(dir.c_str());
    if (dirAttr == INVALID_FILE_ATTRIBUTES || !(dirAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        int res = SHCreateDirectoryExW(NULL, dir.c_str(), NULL);
        if (res != ERROR_SUCCESS && res != ERROR_ALREADY_EXISTS && res != ERROR_FILE_EXISTS) {
            DWORD err = GetLastError();
            return StorageStatus::Error(StorageErrorCode::DirectoryCreationFailed, err ? err : (DWORD)res, dir, "Failed to create destination directory");
        }
    }
    std::wstring tmp = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, tmp, "Failed to create temporary file for atomic write");
    }
    DWORD written = 0;
    BOOL writeOk = TRUE;
    if (!text.empty()) { writeOk = WriteFile(hFile, text.data(), (DWORD)text.size(), &written, NULL) && (written == (DWORD)text.size()); }
    DWORD writeErr = writeOk ? 0 : GetLastError();
    if (writeOk && !FlushFileBuffers(hFile)) { writeOk = FALSE; writeErr = GetLastError(); }
    BY_HANDLE_FILE_INFORMATION info{};
    CloseHandle(hFile);
    if (!writeOk) {
        DeleteFileW(tmp.c_str());
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, writeErr ? writeErr : ERROR_WRITE_FAULT, tmp, "Failed to write or flush temporary file");
    }
    if (committedInfo) {
        // The last-write time may not settle until the writing handle closes.
        HANDLE metadata = CreateFileW(tmp.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (metadata != INVALID_HANDLE_VALUE) { GetFileInformationByHandle(metadata, &info); CloseHandle(metadata); }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD moveErr = GetLastError();
        DeleteFileW(tmp.c_str());
        return StorageStatus::Error(StorageErrorCode::AtomicRenameFailed, moveErr, path, "Failed to rename temporary file to destination");
    }
    if (committedInfo) *committedInfo = info;
    return StorageStatus::Ok(path);
}

inline StorageStatus ReadTextFile(const std::wstring& path, std::string& out, size_t byteLimit = kMaxPortableStoreBytes) {
    out.clear();
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, err, path, "Failed to open file for reading");
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size)) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, err, path, "Failed to get file size");
    }
    if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>((std::min)(byteLimit, kMaxPortableStoreBytes))) {
        CloseHandle(hFile);
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, ERROR_FILE_TOO_LARGE, path, "File exceeds size limit");
    }
    if (size.QuadPart == 0) { CloseHandle(hFile); return StorageStatus::Ok(path); }
    out.resize((size_t)size.QuadPart);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &out[0], (DWORD)size.QuadPart, &bytesRead, NULL) || bytesRead != (DWORD)size.QuadPart) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        out.clear();
        return StorageStatus::Error(StorageErrorCode::FileReadFailed, err, path, "Failed to read file content");
    }
    CloseHandle(hFile);
    return StorageStatus::Ok(path);
}

} // namespace Aperture
