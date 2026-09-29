#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include "Common.h"

namespace Aperture {
namespace DllLoader {

// Harden process-wide DLL search paths to prevent loading arbitrary DLLs from CWD or untrusted search paths
inline bool HardenProcessDllSearch() {
    BOOL setDirOk = SetDllDirectoryW(L"");
    BOOL setDefaultOk =
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
    return (setDirOk != FALSE) || (setDefaultOk != FALSE);
}

// Check if a path is absolute
inline bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) { return true; }
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) {
        return true; // UNC path
    }
    return false;
}

// Resolve package-local DLL path strictly against GetExeDir()
inline std::wstring ResolvePackageDllPath(const std::wstring& dllName) {
    std::wstring exeDir = GetExeDir();
    if (exeDir.empty()) return L"";
    std::wstring cleanName = dllName;
    // Strip leading path separators
    while (!cleanName.empty() && (cleanName.front() == L'\\' || cleanName.front() == L'/')) { cleanName.erase(cleanName.begin()); }
    std::wstring fullPath = exeDir + L"\\" + cleanName;
    return fullPath;
}

// Load package-local DLL using absolute root-derived paths and restricted DLL search flags
inline HMODULE LoadPackageLocalDll(const std::wstring& dllName) {
    std::wstring absPath;
    if (IsAbsolutePath(dllName)) { absPath = dllName; } else { absPath = ResolvePackageDllPath(dllName); }
    // Verify the file exists before attempting load
    DWORD attr = GetFileAttributesW(absPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) { return NULL; }
    // Restrict dependency resolution to application directory and system32
    DWORD flags = LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
    HMODULE hMod = LoadLibraryExW(absPath.c_str(), NULL, flags);
    if (!hMod) {
        // Fallback with ALTERED_SEARCH_PATH if needed for dependencies located next to the DLL
        hMod = LoadLibraryExW(absPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    return hMod;
}

} // namespace DllLoader
} // namespace Aperture
