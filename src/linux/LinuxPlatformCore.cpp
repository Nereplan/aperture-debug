#if !defined(_WIN32)

#include "LinuxPlatform.h"
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
thread_local DWORD g_lastError = ERROR_SUCCESS;

DWORD ErrnoToWin32(int err) {
    switch (err) {
        case 0: return ERROR_SUCCESS;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case EACCES:
        case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
        case EINVAL: return ERROR_INVALID_PARAMETER;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case ENOSPC: return ERROR_DISK_FULL;
        case EBUSY: return ERROR_SHARING_VIOLATION;
        case ETIMEDOUT: return ERROR_WAIT_TIMEOUT;
        case ENOTDIR: return ERROR_DIRECTORY;
        default: return ERROR_INVALID_FUNCTION;
    }
}

constexpr uint64_t EPOCH_DIFFERENCE_100NS = 116444736000000000ULL;

FILETIME TimespecToFileTime(time_t sec, long nsec) {
    uint64_t total100ns = (static_cast<uint64_t>(sec) * 10000000ULL) + (static_cast<uint64_t>(nsec) / 100ULL) + EPOCH_DIFFERENCE_100NS;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(total100ns & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(total100ns >> 32);
    return ft;
}

} // namespace

DWORD GetLastError() { return g_lastError; }
void SetLastError(DWORD dwErrCode) { g_lastError = dwErrCode; }

// ============================================================================
// UTF-8 <-> UTF-16/32 (wchar_t on Linux is 32-bit UTF-32)
// ============================================================================

std::string LinuxWideToUtf8(const wchar_t* wstr, int len) {
    if (!wstr) return {};
    size_t count = (len < 0) ? std::wcslen(wstr) : static_cast<size_t>(len);
    std::string out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        uint32_t cp = static_cast<uint32_t>(wstr[i]);
        // Handle surrogate pairs if UTF-16 code units were placed into wchar_t
        if (cp >= 0xD800 && cp <= 0xDBFF && (i + 1) < count) {
            uint32_t low = static_cast<uint32_t>(wstr[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string LinuxWideToUtf8(const std::wstring& wstr) {
    return LinuxWideToUtf8(wstr.data(), static_cast<int>(wstr.size()));
}

std::wstring LinuxUtf8ToWide(const char* str, int len) {
    if (!str) return {};
    size_t count = (len < 0) ? std::strlen(str) : static_cast<size_t>(len);
    std::wstring out;
    out.reserve(count);
    size_t i = 0;
    while (i < count) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < count) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < count) {
            cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < count) {
            cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) |
                 ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = 0xFFFD;
            i += 1;
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::wstring LinuxUtf8ToWide(const std::string& str) {
    return LinuxUtf8ToWide(str.data(), static_cast<int>(str.size()));
}

std::string LinuxWinPathToPosix(const std::wstring& wpath) {
    if (wpath.empty()) return {};
    std::wstring p = wpath;
    // Strip \\?\ prefix if present
    if (p.rfind(L"\\\\?\\", 0) == 0) {
        p = p.substr(4);
    }
    // Convert C:\... synthetic test paths into /tmp/aperture-winroot/c/...
    if (p.size() >= 2 && std::iswalpha(p[0]) && p[1] == L':') {
        wchar_t drive = static_cast<wchar_t>(std::towlower(p[0]));
        std::wstring rest = p.substr(2);
        p = L"/tmp/aperture-drive-" + std::wstring(1, drive) + rest;
    }
    for (wchar_t& ch : p) {
        if (ch == L'\\') ch = L'/';
    }
    return LinuxWideToUtf8(p);
}

std::wstring LinuxPosixPathToWin(const std::string& path) {
    return LinuxUtf8ToWide(path);
}

int WideCharToMultiByte(UINT /*CodePage*/, DWORD /*dwFlags*/, LPCWSTR lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR /*lpDefaultChar*/, LPBOOL /*lpUsedDefaultChar*/) {
    if (!lpWideCharStr) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    bool nullTerm = (cchWideChar == -1);
    int inLen = nullTerm ? static_cast<int>(std::wcslen(lpWideCharStr)) : cchWideChar;
    std::string utf8 = LinuxWideToUtf8(lpWideCharStr, inLen);
    int needed = static_cast<int>(utf8.size()) + (nullTerm ? 1 : 0);
    if (cbMultiByte == 0 || !lpMultiByteStr) {
        return needed;
    }
    if (cbMultiByte < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::memcpy(lpMultiByteStr, utf8.data(), utf8.size());
    if (nullTerm) lpMultiByteStr[utf8.size()] = '\0';
    return needed;
}

int MultiByteToWideChar(UINT /*CodePage*/, DWORD /*dwFlags*/, LPCSTR lpMultiByteStr, int cbMultiByte,
                        LPWSTR lpWideCharStr, int cchWideChar) {
    if (!lpMultiByteStr) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    bool nullTerm = (cbMultiByte == -1);
    int inLen = nullTerm ? static_cast<int>(std::strlen(lpMultiByteStr)) : cbMultiByte;
    std::wstring wide = LinuxUtf8ToWide(lpMultiByteStr, inLen);
    int needed = static_cast<int>(wide.size()) + (nullTerm ? 1 : 0);
    if (cchWideChar == 0 || !lpWideCharStr) {
        return needed;
    }
    if (cchWideChar < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::wmemcpy(lpWideCharStr, wide.data(), wide.size());
    if (nullTerm) lpWideCharStr[wide.size()] = L'\0';
    return needed;
}

int CompareStringOrdinal(LPCWSTR lpString1, int cchCount1, LPCWSTR lpString2, int cchCount2, BOOL bIgnoreCase) {
    if (!lpString1 || !lpString2) return 0;
    size_t len1 = (cchCount1 < 0) ? std::wcslen(lpString1) : static_cast<size_t>(cchCount1);
    size_t len2 = (cchCount2 < 0) ? std::wcslen(lpString2) : static_cast<size_t>(cchCount2);
    size_t minLen = std::min(len1, len2);
    for (size_t i = 0; i < minLen; ++i) {
        wchar_t c1 = bIgnoreCase ? static_cast<wchar_t>(std::towlower(lpString1[i])) : lpString1[i];
        wchar_t c2 = bIgnoreCase ? static_cast<wchar_t>(std::towlower(lpString2[i])) : lpString2[i];
        if (c1 < c2) return CSTR_LESS_THAN;
        if (c1 > c2) return CSTR_GREATER_THAN;
    }
    if (len1 < len2) return CSTR_LESS_THAN;
    if (len1 > len2) return CSTR_GREATER_THAN;
    return CSTR_EQUAL;
}

int LCMapStringEx(LPCWSTR /*lpLocaleName*/, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc,
                  LPWSTR lpDestStr, int cchDest, void* /*lpVersionInformation*/, void* /*lpReserved*/, LPARAM /*sortHandle*/) {
    if (!lpSrcStr) return 0;
    bool nullTerm = (cchSrc < 0);
    int len = nullTerm ? static_cast<int>(std::wcslen(lpSrcStr)) : cchSrc;
    int needed = len + (nullTerm ? 1 : 0);
    if (cchDest == 0 || !lpDestStr) return needed;
    if (cchDest < needed) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
    for (int i = 0; i < len; ++i) {
        if (dwMapFlags & LCMAP_LOWERCASE) lpDestStr[i] = static_cast<wchar_t>(std::towlower(lpSrcStr[i]));
        else if (dwMapFlags & LCMAP_UPPERCASE) lpDestStr[i] = static_cast<wchar_t>(std::towupper(lpSrcStr[i]));
        else lpDestStr[i] = lpSrcStr[i];
    }
    if (nullTerm) lpDestStr[len] = L'\0';
    return needed;
}

int GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName) {
    const char* lang = std::getenv("LC_ALL");
    if (!lang || !*lang) lang = std::getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = std::getenv("LANG");
    std::string loc = (lang && *lang) ? lang : "en_US";
    size_t dot = loc.find('.');
    if (dot != std::string::npos) loc = loc.substr(0, dot);
    for (char& c : loc) if (c == '_') c = '-';
    std::wstring wloc = LinuxUtf8ToWide(loc);
    if (wloc.empty() || wloc == L"C" || wloc == L"POSIX") wloc = L"en-US";
    if (!lpLocaleName || cchLocaleName <= static_cast<int>(wloc.size())) return 0;
    wcscpy_s(lpLocaleName, cchLocaleName, wloc.c_str());
    return static_cast<int>(wloc.size()) + 1;
}

int GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE /*LCType*/, LPWSTR lpLCData, int cchData) {
    wchar_t buf[64] = {};
    if (!lpLocaleName || !*lpLocaleName) {
        GetUserDefaultLocaleName(buf, 64);
        lpLocaleName = buf;
    }
    std::wstring s(lpLocaleName);
    size_t dash = s.find(L'-');
    if (dash != std::wstring::npos) s = s.substr(0, dash);
    if (s.empty()) s = L"en";
    if (cchData == 0 || !lpLCData) return static_cast<int>(s.size()) + 1;
    wcscpy_s(lpLCData, cchData, s.c_str());
    return static_cast<int>(s.size()) + 1;
}

// ============================================================================
// Path, URL, CommandLine & CRT File Helpers
// ============================================================================

FILE* _wfopen(const wchar_t* filename, const wchar_t* mode) {
    if (!filename || !mode) return nullptr;
    std::string u8Path = LinuxWinPathToPosix(filename);
    std::string u8Mode = LinuxWideToUtf8(mode);
    // Strip 'c' or 'N' Win32-specific mode flags if present
    std::string cleanMode;
    for (char c : u8Mode) {
        if (c == 'r' || c == 'w' || c == 'a' || c == '+' || c == 'b') cleanMode.push_back(c);
    }
    if (cleanMode.empty()) cleanMode = "r";
    return std::fopen(u8Path.c_str(), cleanMode.c_str());
}

int _wfopen_s(FILE** pFile, const wchar_t* filename, const wchar_t* mode) {
    if (!pFile) return EINVAL;
    *pFile = _wfopen(filename, mode);
    return *pFile ? 0 : (errno ? errno : ENOENT);
}

wchar_t* _wfullpath(wchar_t* absPath, const wchar_t* relPath, size_t maxLength) {
    if (!relPath) return nullptr;
    std::string u8Rel = LinuxWinPathToPosix(relPath);
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::u8path(u8Rel), ec), ec);
    if (ec) {
        p = fs::absolute(fs::u8path(u8Rel), ec);
    }
    std::wstring wabs = LinuxPosixPathToWin(p.u8string());
    if (!absPath) {
        size_t len = wabs.size() + 1;
        wchar_t* res = static_cast<wchar_t*>(std::malloc(len * sizeof(wchar_t)));
        if (res) wcscpy_s(res, len, wabs.c_str());
        return res;
    }
    if (wabs.size() >= maxLength) return nullptr;
    wcscpy_s(absPath, maxLength, wabs.c_str());
    return absPath;
}

BOOL PathIsRelativeW(LPCWSTR lpszPath) {
    if (!lpszPath || !*lpszPath) return TRUE;
    if (lpszPath[0] == L'/' || lpszPath[0] == L'\\') return FALSE;
    if (std::iswalpha(lpszPath[0]) && lpszPath[1] == L':') return FALSE;
    return TRUE;
}

BOOL PathRelativePathToW(LPWSTR pszPath, LPCWSTR pszFrom, DWORD dwAttrFrom, LPCWSTR pszTo, DWORD /*dwAttrTo*/) {
    if (!pszPath || !pszFrom || !pszTo) return FALSE;
    std::error_code ec;
    fs::path fromPath = fs::u8path(LinuxWinPathToPosix(pszFrom));
    if (!(dwAttrFrom & FILE_ATTRIBUTE_DIRECTORY)) {
        fromPath = fromPath.parent_path();
    }
    fs::path toPath = fs::u8path(LinuxWinPathToPosix(pszTo));
    fs::path rel = fs::relative(toPath, fromPath, ec);
    if (ec || rel.empty()) return FALSE;
    std::wstring wrel = LinuxPosixPathToWin(rel.u8string());
    if (wrel.rfind(L".", 0) != 0 && wrel.rfind(L"/", 0) != 0) {
        wrel = L"./" + wrel;
    }
    wcscpy_s(pszPath, MAX_PATH, wrel.c_str());
    return TRUE;
}

HRESULT UrlCombineW(LPCWSTR pszBase, LPCWSTR pszRelative, LPWSTR pszCombined, LPDWORD pcchCombined, DWORD /*dwFlags*/) {
    if (!pszBase || !pszRelative || !pcchCombined) return E_INVALIDARG;
    std::wstring base(pszBase);
    std::wstring rel(pszRelative);
    std::wstring result;
    if (rel.find(L"://") != std::wstring::npos) {
        result = rel;
    } else if (!rel.empty() && rel[0] == L'/') {
        size_t schemeEnd = base.find(L"://");
        if (schemeEnd != std::wstring::npos) {
            size_t hostEnd = base.find(L'/', schemeEnd + 3);
            result = (hostEnd != std::wstring::npos ? base.substr(0, hostEnd) : base) + rel;
        } else {
            result = rel;
        }
    } else {
        size_t lastSlash = base.rfind(L'/');
        size_t schemeEnd = base.find(L"://");
        if (lastSlash != std::wstring::npos && (schemeEnd == std::wstring::npos || lastSlash > schemeEnd + 2)) {
            result = base.substr(0, lastSlash + 1) + rel;
        } else {
            result = base + L"/" + rel;
        }
    }
    DWORD needed = static_cast<DWORD>(result.size() + 1);
    if (!pszCombined || *pcchCombined < needed) {
        *pcchCombined = needed;
        return E_POINTER;
    }
    wcscpy_s(pszCombined, *pcchCombined, result.c_str());
    *pcchCombined = static_cast<DWORD>(result.size());
    return S_OK;
}

static std::wstring g_commandLineW = L"aperture";

void LinuxSetCommandLine(int argc, char** argv) {
    std::wstring cmd;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) cmd += L" ";
        std::wstring arg = LinuxUtf8ToWide(argv[i] ? argv[i] : "");
        if (arg.find(L' ') != std::wstring::npos) {
            cmd += L"\"" + arg + L"\"";
        } else {
            cmd += arg;
        }
    }
    g_commandLineW = cmd;
}

LPWSTR GetCommandLineW() {
    return const_cast<LPWSTR>(g_commandLineW.c_str());
}

LPWSTR* CommandLineToArgvW(LPCWSTR lpCmdLine, int* pNumArgs) {
    if (!pNumArgs) return nullptr;
    std::wstring cmd = (lpCmdLine && *lpCmdLine) ? lpCmdLine : g_commandLineW;
    std::vector<std::wstring> args;
    std::wstring current;
    bool inQuotes = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        wchar_t c = cmd[i];
        if (c == L'"') {
            inQuotes = !inQuotes;
        } else if (std::iswspace(c) && !inQuotes) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) args.push_back(current);
    if (args.empty()) args.push_back(L"aperture");

    *pNumArgs = static_cast<int>(args.size());
    size_t ptrsBytes = (args.size() + 1) * sizeof(LPWSTR);
    size_t charsCount = 0;
    for (const auto& a : args) charsCount += a.size() + 1;
    uint8_t* block = static_cast<uint8_t*>(std::malloc(ptrsBytes + charsCount * sizeof(wchar_t)));
    if (!block) return nullptr;
    LPWSTR* argv = reinterpret_cast<LPWSTR*>(block);
    wchar_t* strPtr = reinterpret_cast<wchar_t*>(block + ptrsBytes);
    for (size_t i = 0; i < args.size(); ++i) {
        argv[i] = strPtr;
        std::wmemcpy(strPtr, args[i].c_str(), args[i].size() + 1);
        strPtr += args[i].size() + 1;
    }
    argv[args.size()] = nullptr;
    return argv;
}

HLOCAL LocalFree(HLOCAL hMem) {
    if (hMem) std::free(hMem);
    return nullptr;
}
void CoTaskMemFree(LPVOID pv) {
    if (pv) std::free(pv);
}
LPVOID CoTaskMemAlloc(SIZE_T cb) {
    return std::malloc(cb);
}

HRESULT CoInitializeEx(LPVOID /*pvReserved*/, DWORD /*dwCoInit*/) { return S_OK; }
void CoUninitialize() {}

HRESULT CoCreateInstance(REFCLSID rclsid, IUnknown* /*pUnkOuter*/, DWORD /*dwClsContext*/, REFIID /*riid*/, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid == CLSID_WICImagingFactory) {
        *ppv = new IWICImagingFactory();
        return S_OK;
    }
    if (rclsid == CLSID_FileOpenDialog || rclsid == CLSID_FileSaveDialog) {
        *ppv = new IFileOpenDialog();
        return S_OK;
    }
    if (rclsid == CLSID_TaskbarList) {
        *ppv = new ITaskbarList3();
        return S_OK;
    }
    if (rclsid == CLSID_DestinationList) {
        *ppv = new ICustomDestinationList();
        return S_OK;
    }
    if (rclsid == CLSID_EnumerableObjectCollection) {
        *ppv = new IObjectCollection();
        return S_OK;
    }
    if (rclsid == CLSID_ShellLink) {
        *ppv = new IShellLinkW();
        return S_OK;
    }
    if (rclsid == CLSID_ApplicationAssociationRegistrationUI) {
        *ppv = new IApplicationAssociationRegistrationUI();
        return S_OK;
    }
    return E_NOINTERFACE;
}

// ============================================================================
// Filesystem & Handle APIs
// ============================================================================

struct LinuxHandleObject {
    enum Type { File, Event, Mutex, Timer, Process } type = File;
    int fd = -1;
    std::string path;
    bool deleteOnClose = false;
    // Event / Timer state
    std::mutex mtx;
    std::condition_variable cv;
    bool manualReset = false;
    bool signaled = false;
    // Timer due time
    std::chrono::steady_clock::time_point dueTime{};
    bool timerArmed = false;
    LONG timerPeriodMs = 0;
    // Process
    pid_t pid = -1;
    DWORD exitCode = 0;
};

DWORD GetModuleFileNameW(HMODULE /*hModule*/, LPWSTR lpFilename, DWORD nSize) {
    if (!lpFilename || nSize == 0) return 0;
    char buf[PATH_MAX] = {};
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exePath = (len > 0) ? std::string(buf, static_cast<size_t>(len)) : "/usr/bin/aperture";
    std::wstring wpath = LinuxPosixPathToWin(exePath);
    if (wpath.size() >= nSize) {
        std::wmemcpy(lpFilename, wpath.data(), nSize - 1);
        lpFilename[nSize - 1] = L'\0';
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nSize;
    }
    wcscpy_s(lpFilename, nSize, wpath.c_str());
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(wpath.size());
}

HMODULE GetModuleHandleW(LPCWSTR /*lpModuleName*/) {
    return reinterpret_cast<HMODULE>(0x1000);
}

BOOL SetDllDirectoryW(LPCWSTR /*lpPathName*/) { return TRUE; }
BOOL SetDefaultDllDirectories(DWORD /*DirectoryFlags*/) { return TRUE; }

BOOL GetModuleHandleExW(DWORD /*dwFlags*/, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!phModule) return FALSE;
    *phModule = reinterpret_cast<HMODULE>(const_cast< LPWSTR >(lpModuleName));
    return TRUE;
}

HMODULE LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE /*hFile*/, DWORD /*dwFlags*/) {
    if (!lpLibFileName) return nullptr;
    std::string u8 = LinuxWinPathToPosix(lpLibFileName);
    // Translate Windows DLL names to Linux shared libraries
    if (u8.find("libmpv") != std::string::npos) {
        // First try package-local libmpv.so.2 or libmpv.so next to binary
        fs::path p = fs::u8path(u8).parent_path();
        for (const char* name : {"libmpv.so.2", "libmpv.so"}) {
            fs::path candidate = p / name;
            if (fs::exists(candidate)) {
                void* h = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (h) return reinterpret_cast<HMODULE>(h);
            }
        }
        // Then system libmpv on CachyOS/Arch Linux
        for (const char* name : {"libmpv.so.2", "libmpv.so", "libmpv.so.1"}) {
            void* h = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (h) return reinterpret_cast<HMODULE>(h);
        }
    }
    void* h = ::dlopen(u8.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) SetLastError(ERROR_MOD_NOT_FOUND);
    return reinterpret_cast<HMODULE>(h);
}

HMODULE LoadLibraryW(LPCWSTR lpLibFileName) {
    return LoadLibraryExW(lpLibFileName, nullptr, 0);
}

FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    if (!hModule || !lpProcName) return nullptr;
    void* sym = ::dlsym(hModule, lpProcName);
    if (!sym) SetLastError(ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(sym);
}

BOOL FreeLibrary(HMODULE hLibModule) {
    if (!hLibModule || hLibModule == reinterpret_cast<HMODULE>(0x1000)) return TRUE;
    // Check if it looks like a real dlopen handle
    ::dlclose(hLibModule);
    return TRUE;
}

DWORD GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    if (!lpFileName) return 0;
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    std::error_code ec;
    fs::path absP = fs::weakly_canonical(fs::absolute(fs::u8path(u8), ec), ec);
    if (ec) absP = fs::absolute(fs::u8path(u8), ec);
    std::wstring wabs = LinuxPosixPathToWin(absP.u8string());
    if (nBufferLength <= wabs.size() || !lpBuffer) {
        return static_cast<DWORD>(wabs.size() + 1);
    }
    wcscpy_s(lpBuffer, nBufferLength, wabs.c_str());
    if (lpFilePart) {
        wchar_t* slash = std::wcsrchr(lpBuffer, L'/');
        if (!slash) slash = std::wcsrchr(lpBuffer, L'\\');
        *lpFilePart = slash ? (slash + 1) : lpBuffer;
    }
    return static_cast<DWORD>(wabs.size());
}

DWORD GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer) {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    std::string s(tmp);
    if (s.back() != '/') s.push_back('/');
    std::wstring ws = LinuxUtf8ToWide(s);
    if (lpBuffer && nBufferLength > ws.size()) {
        wcscpy_s(lpBuffer, nBufferLength, ws.c_str());
    }
    return static_cast<DWORD>(ws.size());
}

DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
    if (!lpName) return 0;
    std::string name = LinuxWideToUtf8(lpName);
    const char* val = std::getenv(name.c_str());
    if (!val) {
        SetLastError(ERROR_NOT_FOUND);
        return 0;
    }
    std::wstring wval = LinuxUtf8ToWide(val);
    if (!lpBuffer || nSize <= wval.size()) {
        return static_cast<DWORD>(wval.size() + 1);
    }
    wcscpy_s(lpBuffer, nSize, wval.c_str());
    return static_cast<DWORD>(wval.size());
}

BOOL SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue) {
    if (!lpName) return FALSE;
    std::string name = LinuxWideToUtf8(lpName);
    if (!lpValue) {
        ::unsetenv(name.c_str());
    } else {
        std::string val = LinuxWideToUtf8(lpValue);
        ::setenv(name.c_str(), val.c_str(), 1);
    }
    return TRUE;
}

DWORD ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize) {
    if (!lpSrc) return 0;
    std::wstring src(lpSrc);
    if (!lpDst || nSize <= src.size()) return static_cast<DWORD>(src.size() + 1);
    wcscpy_s(lpDst, nSize, src.c_str());
    return static_cast<DWORD>(src.size() + 1);
}

DWORD GetFileAttributesW(LPCWSTR lpFileName) {
    if (!lpFileName || !*lpFileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    // Special case: DllLoader checks for package-local libmpv-2.dll
    if (u8.find("libmpv-2.dll") != std::string::npos) {
        for (const char* name : {"libmpv.so.2", "libmpv.so", "libmpv.so.1"}) {
            void* h = ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
            if (!h) h = ::dlopen(name, RTLD_LAZY);
            if (h) {
                ::dlclose(h);
                return FILE_ATTRIBUTE_NORMAL;
            }
        }
    }
    struct stat lst {};
    if (::lstat(u8.c_str(), &lst) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attrs = 0;
    if (S_ISLNK(lst.st_mode)) {
        attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
        struct stat st {};
        if (::stat(u8.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            attrs |= FILE_ATTRIBUTE_DIRECTORY;
        }
    } else if (S_ISDIR(lst.st_mode)) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    }
    if ((lst.st_mode & S_IWUSR) == 0) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }
    return attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
}

BOOL GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS /*fInfoLevelId*/, LPVOID lpFileInformation) {
    if (!lpFileName || !lpFileInformation) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    struct stat lst {};
    if (::lstat(u8.c_str(), &lst) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    auto* info = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(lpFileInformation);
    std::memset(info, 0, sizeof(*info));
    DWORD attrs = 0;
    if (S_ISLNK(lst.st_mode)) {
        attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
        struct stat st {};
        if (::stat(u8.c_str(), &st) == 0) {
            lst = st;
            attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
        }
    }
    if (S_ISDIR(lst.st_mode)) attrs |= FILE_ATTRIBUTE_DIRECTORY;
    if ((lst.st_mode & S_IWUSR) == 0) attrs |= FILE_ATTRIBUTE_READONLY;
    info->dwFileAttributes = attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
    info->ftCreationTime = TimespecToFileTime(lst.st_ctim.tv_sec, lst.st_ctim.tv_nsec);
    info->ftLastAccessTime = TimespecToFileTime(lst.st_atim.tv_sec, lst.st_atim.tv_nsec);
    info->ftLastWriteTime = TimespecToFileTime(lst.st_mtim.tv_sec, lst.st_mtim.tv_nsec);
    uint64_t sz = S_ISDIR(lst.st_mode) ? 0ULL : static_cast<uint64_t>(lst.st_size);
    info->nFileSizeLow = static_cast<DWORD>(sz & 0xFFFFFFFFULL);
    info->nFileSizeHigh = static_cast<DWORD>(sz >> 32);
    return TRUE;
}

BOOL SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes) {
    if (!lpFileName) return FALSE;
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    struct stat st {};
    if (::stat(u8.c_str(), &st) != 0) return FALSE;
    mode_t mode = st.st_mode;
    if (dwFileAttributes & FILE_ATTRIBUTE_READONLY) mode &= ~S_IWUSR;
    else mode |= S_IWUSR;
    return ::chmod(u8.c_str(), mode) == 0 ? TRUE : FALSE;
}

HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD /*dwShareMode*/,
                   LPSECURITY_ATTRIBUTES /*lpSecurityAttributes*/, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE /*hTemplateFile*/) {
    if (!lpFileName || !*lpFileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    int flags = O_CLOEXEC;
    bool wantRead = (dwDesiredAccess & GENERIC_READ) != 0;
    bool wantWrite = (dwDesiredAccess & GENERIC_WRITE) != 0;
    if (wantRead && wantWrite) flags |= O_RDWR;
    else if (wantWrite) flags |= O_WRONLY;
    else flags |= O_RDONLY;

    if (dwFlagsAndAttributes & FILE_FLAG_OPEN_REPARSE_POINT) {
        flags |= O_NOFOLLOW;
    }

    switch (dwCreationDisposition) {
        case CREATE_NEW:
            flags |= O_CREAT | O_EXCL;
            break;
        case CREATE_ALWAYS:
            flags |= O_CREAT | O_TRUNC;
            break;
        case OPEN_EXISTING:
            break;
        case OPEN_ALWAYS:
            flags |= O_CREAT;
            break;
        case TRUNCATE_EXISTING:
            flags |= O_TRUNC;
            break;
        default:
            break;
    }

    int fd = ::open(u8.c_str(), flags, 0644);
    if (fd < 0 && (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS)) {
        fd = ::open(u8.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    if (fd < 0) {
        SetLastError(ErrnoToWin32(errno));
        return INVALID_HANDLE_VALUE;
    }
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::File;
    obj->fd = fd;
    obj->path = u8;
    obj->deleteOnClose = (dwFlagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    SetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HANDLE>(obj);
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED /*lpOverlapped*/) {
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
    if (!hFile || hFile == INVALID_HANDLE_VALUE || !lpBuffer) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    ssize_t n = ::read(obj->fd, lpBuffer, nNumberOfBytesToRead);
    if (n < 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<DWORD>(n);
    return TRUE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED /*lpOverlapped*/) {
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
    if (!hFile || hFile == INVALID_HANDLE_VALUE || (!lpBuffer && nNumberOfBytesToWrite > 0)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    size_t total = 0;
    const uint8_t* ptr = static_cast<const uint8_t*>(lpBuffer);
    while (total < nNumberOfBytesToWrite) {
        ssize_t n = ::write(obj->fd, ptr + total, nNumberOfBytesToWrite - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            SetLastError(ErrnoToWin32(errno));
            return FALSE;
        }
        total += static_cast<size_t>(n);
    }
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<DWORD>(total);
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE hFile) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    return ::fsync(obj->fd) == 0 ? TRUE : FALSE;
}

BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    int whence = SEEK_SET;
    if (dwMoveMethod == FILE_CURRENT) whence = SEEK_CUR;
    else if (dwMoveMethod == FILE_END) whence = SEEK_END;
    off_t pos = ::lseek(obj->fd, static_cast<off_t>(liDistanceToMove.QuadPart), whence);
    if (pos == (off_t)-1) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    if (lpNewFilePointer) lpNewFilePointer->QuadPart = static_cast<LONGLONG>(pos);
    return TRUE;
}

DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    LARGE_INTEGER dist{};
    if (lpDistanceToMoveHigh) {
        dist.LowPart = static_cast<DWORD>(lDistanceToMove);
        dist.HighPart = *lpDistanceToMoveHigh;
    } else {
        dist.QuadPart = lDistanceToMove;
    }
    LARGE_INTEGER newPos{};
    if (!SetFilePointerEx(hFile, dist, &newPos, dwMoveMethod)) {
        return INVALID_SET_FILE_POINTER;
    }
    if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = newPos.HighPart;
    return newPos.LowPart;
}

BOOL SetEndOfFile(HANDLE hFile) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    off_t cur = ::lseek(obj->fd, 0, SEEK_CUR);
    if (cur == (off_t)-1) return FALSE;
    return ::ftruncate(obj->fd, cur) == 0 ? TRUE : FALSE;
}

BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE || !lpFileSize) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    struct stat st {};
    if (::fstat(obj->fd, &st) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    lpFileSize->QuadPart = static_cast<LONGLONG>(st.st_size);
    return TRUE;
}

BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE || !lpFileInformation) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    struct stat st {};
    if (::fstat(obj->fd, &st) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    std::memset(lpFileInformation, 0, sizeof(*lpFileInformation));
    lpFileInformation->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    lpFileInformation->ftCreationTime = TimespecToFileTime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    lpFileInformation->ftLastAccessTime = TimespecToFileTime(st.st_atim.tv_sec, st.st_atim.tv_nsec);
    lpFileInformation->ftLastWriteTime = TimespecToFileTime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
    lpFileInformation->dwVolumeSerialNumber = static_cast<DWORD>(st.st_dev);
    uint64_t sz = static_cast<uint64_t>(st.st_size);
    lpFileInformation->nFileSizeLow = static_cast<DWORD>(sz & 0xFFFFFFFFULL);
    lpFileInformation->nFileSizeHigh = static_cast<DWORD>(sz >> 32);
    lpFileInformation->nNumberOfLinks = static_cast<DWORD>(st.st_nlink);
    uint64_t ino = static_cast<uint64_t>(st.st_ino);
    lpFileInformation->nFileIndexLow = static_cast<DWORD>(ino & 0xFFFFFFFFULL);
    lpFileInformation->nFileIndexHigh = static_cast<DWORD>(ino >> 32);
    return TRUE;
}

BOOL SetFileTime(HANDLE hFile, const FILETIME* /*lpCreationTime*/, const FILETIME* lpLastAccessTime, const FILETIME* lpLastWriteTime) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hFile);
    if (obj->type != LinuxHandleObject::File || obj->fd < 0) return FALSE;
    struct timespec ts[2] = {};
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_nsec = UTIME_OMIT;
    auto toTimespec = [](const FILETIME* ft, struct timespec& out) {
        uint64_t total100ns = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
        if (total100ns >= EPOCH_DIFFERENCE_100NS) {
            uint64_t unix100ns = total100ns - EPOCH_DIFFERENCE_100NS;
            out.tv_sec = static_cast<time_t>(unix100ns / 10000000ULL);
            out.tv_nsec = static_cast<long>((unix100ns % 10000000ULL) * 100ULL);
        }
    };
    if (lpLastAccessTime) toTimespec(lpLastAccessTime, ts[0]);
    if (lpLastWriteTime) toTimespec(lpLastWriteTime, ts[1]);
    return ::futimens(obj->fd, ts) == 0 ? TRUE : FALSE;
}

BOOL CloseHandle(HANDLE hObject) {
    if (!hObject || hObject == INVALID_HANDLE_VALUE) return FALSE;
    if (hObject == reinterpret_cast<HANDLE>(0x2000) || hObject == reinterpret_cast<HANDLE>(0x2001)) return TRUE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hObject);
    if (obj->type == LinuxHandleObject::File && obj->fd >= 0) {
        ::close(obj->fd);
        if (obj->deleteOnClose && !obj->path.empty()) {
            ::unlink(obj->path.c_str());
        }
    }
    delete obj;
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR lpFileName) {
    if (!lpFileName) return FALSE;
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    if (::unlink(u8.c_str()) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE;
    std::string src = LinuxWinPathToPosix(lpExistingFileName);
    std::string dst = LinuxWinPathToPosix(lpNewFileName);
    if (!(dwFlags & MOVEFILE_REPLACE_EXISTING)) {
        struct stat st {};
        if (::lstat(dst.c_str(), &st) == 0) {
            SetLastError(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
    }
    if (::rename(src.c_str(), dst.c_str()) == 0) {
        if (dwFlags & MOVEFILE_WRITE_THROUGH) {
            fs::path parent = fs::u8path(dst).parent_path();
            int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dirFd >= 0) {
                ::fsync(dirFd);
                ::close(dirFd);
            }
        }
        return TRUE;
    }
    if (errno == EXDEV && (dwFlags & MOVEFILE_COPY_ALLOWED)) {
        std::error_code ec;
        auto opts = (dwFlags & MOVEFILE_REPLACE_EXISTING) ? fs::copy_options::overwrite_existing : fs::copy_options::none;
        if (fs::copy_file(fs::u8path(src), fs::u8path(dst), opts, ec)) {
            ::unlink(src.c_str());
            return TRUE;
        }
    }
    SetLastError(ErrnoToWin32(errno));
    return FALSE;
}

BOOL CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, BOOL bFailIfExists) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE;
    std::string src = LinuxWinPathToPosix(lpExistingFileName);
    std::string dst = LinuxWinPathToPosix(lpNewFileName);
    std::error_code ec;
    auto opts = bFailIfExists ? fs::copy_options::none : fs::copy_options::overwrite_existing;
    if (!fs::copy_file(fs::u8path(src), fs::u8path(dst), opts, ec)) {
        SetLastError(ErrnoToWin32(ec.value()));
        return FALSE;
    }
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES /*lpSecurityAttributes*/) {
    if (!lpPathName) return FALSE;
    std::string u8 = LinuxWinPathToPosix(lpPathName);
    if (::mkdir(u8.c_str(), 0755) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL RemoveDirectoryW(LPCWSTR lpPathName) {
    if (!lpPathName) return FALSE;
    std::string u8 = LinuxWinPathToPosix(lpPathName);
    if (::rmdir(u8.c_str()) != 0) {
        SetLastError(ErrnoToWin32(errno));
        return FALSE;
    }
    return TRUE;
}

int SHCreateDirectoryExW(HWND /*hwnd*/, LPCWSTR pszPath, const void* /*psa*/) {
    if (!pszPath) return ERROR_INVALID_PARAMETER;
    std::string u8 = LinuxWinPathToPosix(pszPath);
    std::error_code ec;
    fs::create_directories(fs::u8path(u8), ec);
    return ec ? ErrnoToWin32(ec.value()) : ERROR_SUCCESS;
}

struct LinuxFindHandle {
    DIR* dir = nullptr;
    std::string dirPath;
    std::string pattern;
};

static bool PopulateFindData(const std::string& dirPath, const char* name, LPWIN32_FIND_DATAW lpFindFileData) {
    std::memset(lpFindFileData, 0, sizeof(*lpFindFileData));
    std::wstring wname = LinuxUtf8ToWide(name);
    wcscpy_s(lpFindFileData->cFileName, MAX_PATH, wname.c_str());
    std::string full = dirPath + "/" + name;
    struct stat lst {};
    if (::lstat(full.c_str(), &lst) == 0) {
        DWORD attrs = 0;
        if (S_ISLNK(lst.st_mode)) {
            attrs |= FILE_ATTRIBUTE_REPARSE_POINT;
            lpFindFileData->dwReserved0 = IO_REPARSE_TAG_SYMLINK;
            struct stat st {};
            if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                attrs |= FILE_ATTRIBUTE_DIRECTORY;
            }
        } else if (S_ISDIR(lst.st_mode)) {
            attrs |= FILE_ATTRIBUTE_DIRECTORY;
        }
        if (name[0] == '.' && std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0) {
            attrs |= FILE_ATTRIBUTE_HIDDEN;
        }
        lpFindFileData->dwFileAttributes = attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
        lpFindFileData->ftCreationTime = TimespecToFileTime(lst.st_ctim.tv_sec, lst.st_ctim.tv_nsec);
        lpFindFileData->ftLastAccessTime = TimespecToFileTime(lst.st_atim.tv_sec, lst.st_atim.tv_nsec);
        lpFindFileData->ftLastWriteTime = TimespecToFileTime(lst.st_mtim.tv_sec, lst.st_mtim.tv_nsec);
        uint64_t sz = S_ISDIR(lst.st_mode) ? 0ULL : static_cast<uint64_t>(lst.st_size);
        lpFindFileData->nFileSizeLow = static_cast<DWORD>(sz & 0xFFFFFFFFULL);
        lpFindFileData->nFileSizeHigh = static_cast<DWORD>(sz >> 32);
    } else {
        lpFindFileData->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    return true;
}

HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!lpFileName || !lpFindFileData) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string u8 = LinuxWinPathToPosix(lpFileName);
    fs::path p = fs::u8path(u8);
    std::string dirPath = p.parent_path().u8string();
    if (dirPath.empty()) dirPath = ".";
    std::string pat = p.filename().u8string();
    if (pat == "*.*") pat = "*";

    // If exact path without wildcards, check directly first
    if (pat.find('*') == std::string::npos && pat.find('?') == std::string::npos) {
        struct stat lst {};
        if (::lstat(u8.c_str(), &lst) != 0) {
            SetLastError(ErrnoToWin32(errno));
            return INVALID_HANDLE_VALUE;
        }
        PopulateFindData(dirPath, pat.c_str(), lpFindFileData);
        auto* fh = new LinuxFindHandle();
        fh->dir = nullptr;
        fh->dirPath = dirPath;
        fh->pattern = pat;
        return reinterpret_cast<HANDLE>(fh);
    }

    DIR* d = ::opendir(dirPath.c_str());
    if (!d) {
        SetLastError(ErrnoToWin32(errno));
        return INVALID_HANDLE_VALUE;
    }
    auto* fh = new LinuxFindHandle();
    fh->dir = d;
    fh->dirPath = dirPath;
    fh->pattern = pat;
    if (!FindNextFileW(reinterpret_cast<HANDLE>(fh), lpFindFileData)) {
        ::closedir(d);
        delete fh;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(fh);
}

HANDLE FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS /*fInfoLevelId*/, LPVOID lpFindFileData,
                        FINDEX_SEARCH_OPS /*fSearchOp*/, LPVOID /*lpSearchFilter*/, DWORD /*dwAdditionalFlags*/) {
    return FindFirstFileW(lpFileName, static_cast<LPWIN32_FIND_DATAW>(lpFindFileData));
}

BOOL FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!hFindFile || hFindFile == INVALID_HANDLE_VALUE || !lpFindFileData) return FALSE;
    auto* fh = reinterpret_cast<LinuxFindHandle*>(hFindFile);
    if (!fh->dir) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    while (struct dirent* ent = ::readdir(fh->dir)) {
        if (fh->pattern == "*" || ::fnmatch(fh->pattern.c_str(), ent->d_name, FNM_CASEFOLD) == 0) {
            PopulateFindData(fh->dirPath, ent->d_name, lpFindFileData);
            return TRUE;
        }
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

BOOL FindClose(HANDLE hFindFile) {
    if (!hFindFile || hFindFile == INVALID_HANDLE_VALUE) return FALSE;
    auto* fh = reinterpret_cast<LinuxFindHandle*>(hFindFile);
    if (fh->dir) ::closedir(fh->dir);
    delete fh;
    return TRUE;
}

BOOL GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
                         PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    std::string u8 = lpDirectoryName ? LinuxWinPathToPosix(lpDirectoryName) : ".";
    struct statvfs vfs {};
    if (::statvfs(u8.c_str(), &vfs) != 0) {
        if (::statvfs("/", &vfs) != 0) return FALSE;
    }
    uint64_t avail = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    uint64_t freeB = static_cast<uint64_t>(vfs.f_bfree) * vfs.f_frsize;
    if (lpFreeBytesAvailableToCaller) lpFreeBytesAvailableToCaller->QuadPart = avail;
    if (lpTotalNumberOfBytes) lpTotalNumberOfBytes->QuadPart = total;
    if (lpTotalNumberOfFreeBytes) lpTotalNumberOfFreeBytes->QuadPart = freeB;
    return TRUE;
}

BOOL CancelSynchronousIo(HANDLE /*hThread*/) { return TRUE; }
BOOL CancelIoEx(HANDLE /*hFile*/, LPOVERLAPPED /*lpOverlapped*/) { return TRUE; }

// ============================================================================
// Timing, Threading, Synchronization
// ============================================================================

ULONGLONG GetTickCount64() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

DWORD GetTickCount() {
    return static_cast<DWORD>(GetTickCount64());
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    if (!lpPerformanceCount) return FALSE;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    lpPerformanceCount->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency) {
    if (!lpFrequency) return FALSE;
    lpFrequency->QuadPart = 1000000000LL;
    return TRUE;
}

void GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
    if (!lpSystemTimeAsFileTime) return;
    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    *lpSystemTimeAsFileTime = TimespecToFileTime(ts.tv_sec, ts.tv_nsec);
}

static void TmToSystemTime(const struct tm& t, WORD ms, LPSYSTEMTIME st) {
    st->wYear = static_cast<WORD>(t.tm_year + 1900);
    st->wMonth = static_cast<WORD>(t.tm_mon + 1);
    st->wDayOfWeek = static_cast<WORD>(t.tm_wday);
    st->wDay = static_cast<WORD>(t.tm_mday);
    st->wHour = static_cast<WORD>(t.tm_hour);
    st->wMinute = static_cast<WORD>(t.tm_min);
    st->wSecond = static_cast<WORD>(t.tm_sec);
    st->wMilliseconds = ms;
}

void GetSystemTime(LPSYSTEMTIME lpSystemTime) {
    if (!lpSystemTime) return;
    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t {};
    ::gmtime_r(&ts.tv_sec, &t);
    TmToSystemTime(t, static_cast<WORD>(ts.tv_nsec / 1000000), lpSystemTime);
}

void GetLocalTime(LPSYSTEMTIME lpSystemTime) {
    if (!lpSystemTime) return;
    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t {};
    ::localtime_r(&ts.tv_sec, &t);
    TmToSystemTime(t, static_cast<WORD>(ts.tv_nsec / 1000000), lpSystemTime);
}

BOOL FileTimeToSystemTime(const FILETIME* lpFileTime, LPSYSTEMTIME lpSystemTime) {
    if (!lpFileTime || !lpSystemTime) return FALSE;
    uint64_t total100ns = (static_cast<uint64_t>(lpFileTime->dwHighDateTime) << 32) | lpFileTime->dwLowDateTime;
    if (total100ns < EPOCH_DIFFERENCE_100NS) {
        std::memset(lpSystemTime, 0, sizeof(*lpSystemTime));
        lpSystemTime->wYear = 1970;
        lpSystemTime->wMonth = 1;
        lpSystemTime->wDay = 1;
        return TRUE;
    }
    uint64_t unix100ns = total100ns - EPOCH_DIFFERENCE_100NS;
    time_t sec = static_cast<time_t>(unix100ns / 10000000ULL);
    WORD ms = static_cast<WORD>((unix100ns % 10000000ULL) / 10000ULL);
    struct tm t {};
    ::gmtime_r(&sec, &t);
    TmToSystemTime(t, ms, lpSystemTime);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* lpSystemTime, LPFILETIME lpFileTime) {
    if (!lpSystemTime || !lpFileTime) return FALSE;
    struct tm t {};
    t.tm_year = lpSystemTime->wYear - 1900;
    t.tm_mon = lpSystemTime->wMonth - 1;
    t.tm_mday = lpSystemTime->wDay;
    t.tm_hour = lpSystemTime->wHour;
    t.tm_min = lpSystemTime->wMinute;
    t.tm_sec = lpSystemTime->wSecond;
    time_t sec = ::timegm(&t);
    *lpFileTime = TimespecToFileTime(sec, static_cast<long>(lpSystemTime->wMilliseconds) * 1000000L);
    return TRUE;
}

BOOL SystemTimeToTzSpecificLocalTime(const void* /*lpTimeZoneInformation*/, const SYSTEMTIME* lpUniversalTime, LPSYSTEMTIME lpLocalTime) {
    if (!lpUniversalTime || !lpLocalTime) return FALSE;
    FILETIME ft{};
    if (!SystemTimeToFileTime(lpUniversalTime, &ft)) return FALSE;
    uint64_t total100ns = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    time_t sec = static_cast<time_t>((total100ns - EPOCH_DIFFERENCE_100NS) / 10000000ULL);
    struct tm t {};
    ::localtime_r(&sec, &t);
    TmToSystemTime(t, lpUniversalTime->wMilliseconds, lpLocalTime);
    return TRUE;
}

void Sleep(DWORD dwMilliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
}

DWORD GetCurrentProcessId() { return static_cast<DWORD>(::getpid()); }
DWORD GetCurrentThreadId() { return static_cast<DWORD>(::syscall(SYS_gettid)); }
HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(0x2000); }
HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(0x2001); }
BOOL SetThreadPriority(HANDLE /*hThread*/, int /*nPriority*/) { return TRUE; }
EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE esFlags) { return esFlags; }
BOOL ProcessIdToSessionId(DWORD /*dwProcessId*/, DWORD* pSessionId) {
    if (pSessionId) *pSessionId = 1;
    return TRUE;
}

HANDLE OpenProcess(DWORD /*dwDesiredAccess*/, BOOL /*bInheritHandle*/, DWORD dwProcessId) {
    if (::kill(static_cast<pid_t>(dwProcessId), 0) != 0 && errno == ESRCH) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Process;
    obj->pid = static_cast<pid_t>(dwProcessId);
    return reinterpret_cast<HANDLE>(obj);
}

BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode) {
    if (!hProcess) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hProcess);
    if (obj->type == LinuxHandleObject::Process && obj->pid > 0) {
        obj->exitCode = uExitCode;
        ::kill(obj->pid, SIGKILL);
        int status = 0;
        ::waitpid(obj->pid, &status, 0);
        return TRUE;
    }
    return FALSE;
}

BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) {
    if (!hProcess || !lpExitCode) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hProcess);
    if (obj->type == LinuxHandleObject::Process) {
        int status = 0;
        pid_t r = ::waitpid(obj->pid, &status, WNOHANG);
        if (r == 0) {
            *lpExitCode = 259; // STILL_ACTIVE
        } else if (r > 0 && WIFEXITED(status)) {
            obj->exitCode = static_cast<DWORD>(WEXITSTATUS(status));
            *lpExitCode = obj->exitCode;
        } else {
            *lpExitCode = obj->exitCode;
        }
        return TRUE;
    }
    *lpExitCode = 0;
    return TRUE;
}

HANDLE CreateToolhelp32Snapshot(DWORD /*dwFlags*/, DWORD /*th32ProcessID*/) {
    return INVALID_HANDLE_VALUE;
}
BOOL Process32FirstW(HANDLE /*hSnapshot*/, PROCESSENTRY32W* /*lppe*/) { return FALSE; }
BOOL Process32NextW(HANDLE /*hSnapshot*/, PROCESSENTRY32W* /*lppe*/) { return FALSE; }

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES /*lpEventAttributes*/, BOOL bManualReset, BOOL bInitialState, LPCWSTR /*lpName*/) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Event;
    obj->manualReset = (bManualReset != FALSE);
    obj->signaled = (bInitialState != FALSE);
    return reinterpret_cast<HANDLE>(obj);
}

BOOL SetEvent(HANDLE hEvent) {
    if (!hEvent || hEvent == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hEvent);
    std::lock_guard<std::mutex> lock(obj->mtx);
    obj->signaled = true;
    if (obj->manualReset) obj->cv.notify_all();
    else obj->cv.notify_one();
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent) {
    if (!hEvent || hEvent == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hEvent);
    std::lock_guard<std::mutex> lock(obj->mtx);
    obj->signaled = false;
    return TRUE;
}

HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES /*lpMutexAttributes*/, BOOL bInitialOwner, LPCWSTR /*lpName*/) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Mutex;
    obj->signaled = !bInitialOwner;
    SetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HANDLE>(obj);
}

BOOL ReleaseMutex(HANDLE hMutex) {
    return SetEvent(hMutex);
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES /*lpTimerAttributes*/, LPCWSTR /*lpTimerName*/, DWORD dwFlags, DWORD /*dwDesiredAccess*/) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Timer;
    obj->manualReset = (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    obj->signaled = false;
    return reinterpret_cast<HANDLE>(obj);
}

BOOL SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                      void* /*pfnCompletionRoutine*/, LPVOID /*lpArgToCompletionRoutine*/, BOOL /*fResume*/) {
    if (!hTimer || hTimer == INVALID_HANDLE_VALUE || !lpDueTime) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hTimer);
    std::lock_guard<std::mutex> lock(obj->mtx);
    obj->signaled = false;
    obj->timerArmed = true;
    obj->timerPeriodMs = lPeriod;
    int64_t val = lpDueTime->QuadPart;
    if (val <= 0) {
        // Relative in 100ns units
        auto ns = std::chrono::nanoseconds((-val) * 100LL);
        obj->dueTime = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(ns);
    } else {
        obj->dueTime = std::chrono::steady_clock::now();
    }
    obj->cv.notify_all();
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE hTimer) {
    if (!hTimer || hTimer == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hTimer);
    std::lock_guard<std::mutex> lock(obj->mtx);
    obj->timerArmed = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    if (!hHandle || hHandle == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hHandle);
    if (obj->type == LinuxHandleObject::Process) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(dwMilliseconds);
        while (true) {
            int status = 0;
            pid_t r = ::waitpid(obj->pid, &status, WNOHANG);
            if (r != 0) {
                if (r > 0 && WIFEXITED(status)) obj->exitCode = static_cast<DWORD>(WEXITSTATUS(status));
                return WAIT_OBJECT_0;
            }
            if (dwMilliseconds == 0) return WAIT_TIMEOUT;
            if (dwMilliseconds != INFINITE && std::chrono::steady_clock::now() >= deadline) return WAIT_TIMEOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::unique_lock<std::mutex> lock(obj->mtx);
    if (obj->type == LinuxHandleObject::Timer && obj->timerArmed) {
        auto now = std::chrono::steady_clock::now();
        if (now >= obj->dueTime) {
            if (!obj->manualReset) obj->timerArmed = false;
            return WAIT_OBJECT_0;
        }
        auto maxWait = (dwMilliseconds == INFINITE)
                           ? obj->dueTime
                           : std::min(obj->dueTime, now + std::chrono::milliseconds(dwMilliseconds));
        obj->cv.wait_until(lock, maxWait);
        if (std::chrono::steady_clock::now() >= obj->dueTime && obj->timerArmed) {
            if (!obj->manualReset) obj->timerArmed = false;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }

    if (obj->signaled) {
        if (!obj->manualReset) obj->signaled = false;
        return WAIT_OBJECT_0;
    }
    if (dwMilliseconds == 0) return WAIT_TIMEOUT;
    if (dwMilliseconds == INFINITE) {
        obj->cv.wait(lock, [&] { return obj->signaled; });
    } else {
        if (!obj->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds), [&] { return obj->signaled; })) {
            return WAIT_TIMEOUT;
        }
    }
    if (!obj->manualReset) obj->signaled = false;
    return WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    if (nCount == 0 || !lpHandles) return WAIT_FAILED;
    if (nCount == 1) return WaitForSingleObject(lpHandles[0], dwMilliseconds);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        bool allSignaled = true;
        for (DWORD i = 0; i < nCount; ++i) {
            DWORD r = WaitForSingleObject(lpHandles[i], 0);
            if (r == WAIT_OBJECT_0) {
                if (!bWaitAll) return WAIT_OBJECT_0 + i;
            } else {
                allSignaled = false;
            }
        }
        if (bWaitAll && allSignaled) return WAIT_OBJECT_0;
        if (dwMilliseconds == 0) return WAIT_TIMEOUT;
        if (dwMilliseconds != INFINITE) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= dwMilliseconds) return WAIT_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD /*dwWakeMask*/) {
    if (nCount == 0 || !pHandles) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<DWORD>(dwMilliseconds, 10)));
        return WAIT_OBJECT_0;
    }
    return WaitForMultipleObjects(nCount, pHandles, fWaitAll, std::min<DWORD>(dwMilliseconds, 15));
}

void OutputDebugStringW(LPCWSTR /*lpOutputString*/) {}
void OutputDebugStringA(LPCSTR /*lpOutputString*/) {}

// ============================================================================
// WinHttpCrackUrl & BCrypt & CryptBinary
// ============================================================================

BOOL WinHttpCrackUrl(LPCWSTR pwszUrl, DWORD dwUrlLength, DWORD /*dwFlags*/, LPURL_COMPONENTS lpUrlComponents) {
    if (!pwszUrl || !lpUrlComponents) return FALSE;
    size_t len = (dwUrlLength == 0) ? std::wcslen(pwszUrl) : static_cast<size_t>(dwUrlLength);
    std::wstring url(pwszUrl, len);
    size_t schemePos = url.find(L"://");
    if (schemePos == std::wstring::npos || schemePos == 0) return FALSE;
    std::wstring scheme = url.substr(0, schemePos);
    if (_wcsicmp(scheme.c_str(), L"https") == 0) {
        lpUrlComponents->nScheme = INTERNET_SCHEME_HTTPS;
        lpUrlComponents->nPort = 443;
    } else if (_wcsicmp(scheme.c_str(), L"http") == 0) {
        lpUrlComponents->nScheme = INTERNET_SCHEME_HTTP;
        lpUrlComponents->nPort = 80;
    } else {
        lpUrlComponents->nScheme = 0;
        lpUrlComponents->nPort = 0;
    }

    size_t hostStart = schemePos + 3;
    size_t pathStart = url.find_first_of(L"/?#", hostStart);
    size_t hostEnd = (pathStart == std::wstring::npos) ? url.size() : pathStart;
    std::wstring authority = url.substr(hostStart, hostEnd - hostStart);
    if (authority.empty()) return FALSE;

    // Strip userinfo if any
    size_t atPos = authority.rfind(L'@');
    size_t hostPartOffset = (atPos == std::wstring::npos) ? 0 : (atPos + 1);
    std::wstring hostPort = authority.substr(hostPartOffset);

    size_t colonPos = hostPort.rfind(L':');
    size_t bracketEnd = hostPort.rfind(L']');
    size_t hostLen = hostPort.size();
    if (colonPos != std::wstring::npos && (bracketEnd == std::wstring::npos || colonPos > bracketEnd)) {
        std::wstring portStr = hostPort.substr(colonPos + 1);
        hostLen = colonPos;
        if (!portStr.empty()) {
            lpUrlComponents->nPort = static_cast<WORD>(std::wcstoul(portStr.c_str(), nullptr, 10));
        }
    }
    if (hostLen == 0) return FALSE;

    lpUrlComponents->lpszScheme = const_cast<LPWSTR>(pwszUrl);
    lpUrlComponents->dwSchemeLength = static_cast<DWORD>(schemePos);
    lpUrlComponents->lpszHostName = const_cast<LPWSTR>(pwszUrl + hostStart + hostPartOffset);
    lpUrlComponents->dwHostNameLength = static_cast<DWORD>(hostLen);
    if (pathStart != std::wstring::npos) {
        lpUrlComponents->lpszUrlPath = const_cast<LPWSTR>(pwszUrl + pathStart);
        lpUrlComponents->dwUrlPathLength = static_cast<DWORD>(url.size() - pathStart);
    } else {
        lpUrlComponents->lpszUrlPath = const_cast<LPWSTR>(pwszUrl + url.size());
        lpUrlComponents->dwUrlPathLength = 0;
    }
    return TRUE;
}

// Built-in pure C++ SHA-256 implementation + OpenSSL dynamic ECDSA P-256 verification
namespace {

struct Sha256State {
    uint32_t state[8];
    uint64_t bitcount = 0;
    uint8_t buffer[64];
    size_t bufferLen = 0;

    Sha256State() { Reset(); }
    void Reset() {
        state[0] = 0x6a09e667; state[1] = 0xbb67ae85; state[2] = 0x3c6ef372; state[3] = 0xa54ff53a;
        state[4] = 0x510e527f; state[5] = 0x9b05688c; state[6] = 0x1f83d9ab; state[7] = 0x5be0cd19;
        bitcount = 0;
        bufferLen = 0;
    }

    static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void Transform(const uint8_t block[64]) {
        static constexpr uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t W[64];
        for (int i = 0; i < 16; ++i) {
            W[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
            uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
            W[i] = W[i - 16] + s0 + W[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[i] + W[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void Update(const uint8_t* data, size_t len) {
        bitcount += static_cast<uint64_t>(len) * 8ULL;
        while (len > 0) {
            size_t take = std::min(len, 64 - bufferLen);
            std::memcpy(buffer + bufferLen, data, take);
            bufferLen += take;
            data += take;
            len -= take;
            if (bufferLen == 64) {
                Transform(buffer);
                bufferLen = 0;
            }
        }
    }

    void Finalize(uint8_t digest[32]) {
        buffer[bufferLen++] = 0x80;
        if (bufferLen > 56) {
            std::memset(buffer + bufferLen, 0, 64 - bufferLen);
            Transform(buffer);
            bufferLen = 0;
        }
        std::memset(buffer + bufferLen, 0, 56 - bufferLen);
        for (int i = 0; i < 8; ++i) {
            buffer[56 + i] = static_cast<uint8_t>((bitcount >> ((7 - i) * 8)) & 0xFF);
        }
        Transform(buffer);
        for (int i = 0; i < 8; ++i) {
            digest[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
        }
    }
};

struct LinuxBCryptKey {
    std::vector<uint8_t> x;
    std::vector<uint8_t> y;
};

} // namespace

NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId, LPCWSTR /*pszImplementation*/, ULONG /*dwFlags*/) {
    if (!phAlgorithm || !pszAlgId) return -1;
    *phAlgorithm = reinterpret_cast<BCRYPT_ALG_HANDLE>(const_cast<LPWSTR>(pszAlgId));
    return 0;
}

NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE /*hAlgorithm*/, ULONG /*dwFlags*/) { return 0; }

NTSTATUS BCryptGetProperty(BCRYPT_HANDLE /*hObject*/, LPCWSTR pszProperty, PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG /*dwFlags*/) {
    if (!pszProperty || !pbOutput || cbOutput < sizeof(DWORD)) return -1;
    if (pcbResult) *pcbResult = sizeof(DWORD);
    DWORD val = 32;
    if (std::wcscmp(pszProperty, BCRYPT_OBJECT_LENGTH) == 0) val = sizeof(Sha256State);
    std::memcpy(pbOutput, &val, sizeof(DWORD));
    return 0;
}

NTSTATUS BCryptCreateHash(BCRYPT_ALG_HANDLE /*hAlgorithm*/, BCRYPT_HASH_HANDLE* phHash, PUCHAR /*pbHashObject*/, ULONG /*cbHashObject*/, PUCHAR /*pbSecret*/, ULONG /*cbSecret*/, ULONG /*dwFlags*/) {
    if (!phHash) return -1;
    *phHash = reinterpret_cast<BCRYPT_HASH_HANDLE>(new Sha256State());
    return 0;
}

NTSTATUS BCryptHashData(BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG /*dwFlags*/) {
    if (!hHash || (!pbInput && cbInput > 0)) return -1;
    auto* st = reinterpret_cast<Sha256State*>(hHash);
    if (cbInput > 0) st->Update(pbInput, cbInput);
    return 0;
}

NTSTATUS BCryptFinishHash(BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG /*dwFlags*/) {
    if (!hHash || !pbOutput || cbOutput < 32) return -1;
    auto* st = reinterpret_cast<Sha256State*>(hHash);
    st->Finalize(pbOutput);
    return 0;
}

NTSTATUS BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash) {
    if (hHash) delete reinterpret_cast<Sha256State*>(hHash);
    return 0;
}

NTSTATUS BCryptImportKeyPair(BCRYPT_ALG_HANDLE /*hAlgorithm*/, BCRYPT_KEY_HANDLE /*hImportKey*/, LPCWSTR /*pszBlobType*/,
                             BCRYPT_KEY_HANDLE* phKey, PUCHAR pbInput, ULONG cbInput, ULONG /*dwFlags*/) {
    if (!phKey || !pbInput || cbInput < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return -1;
    auto* hdr = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(pbInput);
    if (hdr->dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || hdr->cbKey != 32) return -1;
    auto* key = new LinuxBCryptKey();
    const uint8_t* xy = pbInput + sizeof(BCRYPT_ECCKEY_BLOB);
    key->x.assign(xy, xy + 32);
    key->y.assign(xy + 32, xy + 64);
    *phKey = reinterpret_cast<BCRYPT_KEY_HANDLE>(key);
    return 0;
}

NTSTATUS BCryptVerifySignature(BCRYPT_KEY_HANDLE hKey, void* /*pPaddingInfo*/, PUCHAR pbHash, ULONG cbHash,
                               PUCHAR pbSignature, ULONG cbSignature, ULONG /*dwFlags*/) {
    if (!hKey || !pbHash || cbHash != 32 || !pbSignature || cbSignature != 64) return -1;
    auto* key = reinterpret_cast<LinuxBCryptKey*>(hKey);
    // Try OpenSSL libcrypto for P-256 verification if available on system
    void* lib = ::dlopen("libcrypto.so.3", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = ::dlopen("libcrypto.so.1.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) lib = ::dlopen("libcrypto.so", RTLD_LAZY | RTLD_LOCAL);
    if (lib) {
        using fn_EC_KEY_new_by_curve_name = void* (*)(int);
        using fn_EC_KEY_free = void (*)(void*);
        using fn_BN_bin2bn = void* (*)(const uint8_t*, int, void*);
        using fn_BN_free = void (*)(void*);
        using fn_EC_KEY_set_public_key_affine_coordinates = int (*)(void*, void*, void*);
        using fn_ECDSA_SIG_new = void* (*)();
        using fn_ECDSA_SIG_free = void (*)(void*);
        using fn_ECDSA_SIG_set0 = int (*)(void*, void*, void*);
        using fn_ECDSA_do_verify = int (*)(const uint8_t*, int, const void*, void*);

        auto pEC_KEY_new = reinterpret_cast<fn_EC_KEY_new_by_curve_name>(::dlsym(lib, "EC_KEY_new_by_curve_name"));
        auto pEC_KEY_free = reinterpret_cast<fn_EC_KEY_free>(::dlsym(lib, "EC_KEY_free"));
        auto pBN_bin2bn = reinterpret_cast<fn_BN_bin2bn>(::dlsym(lib, "BN_bin2bn"));
        auto pBN_free = reinterpret_cast<fn_BN_free>(::dlsym(lib, "BN_free"));
        auto pSetCoords = reinterpret_cast<fn_EC_KEY_set_public_key_affine_coordinates>(::dlsym(lib, "EC_KEY_set_public_key_affine_coordinates"));
        auto pSIG_new = reinterpret_cast<fn_ECDSA_SIG_new>(::dlsym(lib, "ECDSA_SIG_new"));
        auto pSIG_free = reinterpret_cast<fn_ECDSA_SIG_free>(::dlsym(lib, "ECDSA_SIG_free"));
        auto pSIG_set0 = reinterpret_cast<fn_ECDSA_SIG_set0>(::dlsym(lib, "ECDSA_SIG_set0"));
        auto pDoVerify = reinterpret_cast<fn_ECDSA_do_verify>(::dlsym(lib, "ECDSA_do_verify"));

        if (pEC_KEY_new && pEC_KEY_free && pBN_bin2bn && pBN_free && pSetCoords && pSIG_new && pSIG_free && pSIG_set0 && pDoVerify) {
            constexpr int NID_X9_62_prime256v1 = 415;
            void* eckey = pEC_KEY_new(NID_X9_62_prime256v1);
            void* bx = pBN_bin2bn(key->x.data(), 32, nullptr);
            void* by = pBN_bin2bn(key->y.data(), 32, nullptr);
            int ok = 0;
            if (eckey && bx && by && pSetCoords(eckey, bx, by) == 1) {
                void* sig = pSIG_new();
                void* br = pBN_bin2bn(pbSignature, 32, nullptr);
                void* bs = pBN_bin2bn(pbSignature + 32, 32, nullptr);
                if (sig && br && bs && pSIG_set0(sig, br, bs) == 1) {
                    ok = pDoVerify(pbHash, 32, sig, eckey);
                } else {
                    if (br) pBN_free(br);
                    if (bs) pBN_free(bs);
                }
                if (sig) pSIG_free(sig);
            }
            if (bx) pBN_free(bx);
            if (by) pBN_free(by);
            if (eckey) pEC_KEY_free(eckey);
            ::dlclose(lib);
            return (ok == 1) ? 0 : -1;
        }
        ::dlclose(lib);
    }
    return -1;
}

NTSTATUS BCryptDestroyKey(BCRYPT_KEY_HANDLE hKey) {
    if (hKey) delete reinterpret_cast<LinuxBCryptKey*>(hKey);
    return 0;
}

BOOL CryptStringToBinaryA(LPCSTR pszString, DWORD cchString, DWORD /*dwFlags*/, BYTE* pbBinary, DWORD* pcbBinary, DWORD* /*pdwSkip*/, DWORD* /*pdwFlags*/) {
    if (!pszString || !pcbBinary) return FALSE;
    size_t len = (cchString == 0) ? std::strlen(pszString) : static_cast<size_t>(cchString);
    static const int8_t b64[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((len * 3) / 4);
    uint32_t val = 0;
    int bits = -8;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = static_cast<uint8_t>(pszString[i]);
        if (std::isspace(c)) continue;
        int8_t d = b64[c];
        if (d == -2) break;
        if (d < 0) return FALSE;
        val = (val << 6) + static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    if (!pbBinary) {
        *pcbBinary = static_cast<DWORD>(out.size());
        return TRUE;
    }
    if (*pcbBinary < out.size()) return FALSE;
    std::memcpy(pbBinary, out.data(), out.size());
    *pcbBinary = static_cast<DWORD>(out.size());
    return TRUE;
}

BOOL CryptStringToBinaryW(LPCWSTR pszString, DWORD cchString, DWORD dwFlags, BYTE* pbBinary, DWORD* pcbBinary, DWORD* pdwSkip, DWORD* pdwFlags) {
    if (!pszString) return FALSE;
    std::string u8 = LinuxWideToUtf8(pszString, cchString == 0 ? -1 : static_cast<int>(cchString));
    return CryptStringToBinaryA(u8.c_str(), static_cast<DWORD>(u8.size()), dwFlags, pbBinary, pcbBinary, pdwSkip, pdwFlags);
}

BOOL CryptBinaryToStringW(const BYTE* pbBinary, DWORD cbBinary, DWORD /*dwFlags*/, LPWSTR pszString, DWORD* pcchString) {
    if (!pbBinary || !pcchString) return FALSE;
    static const wchar_t* kB64 = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve(((cbBinary + 2) / 3) * 4 + 1);
    for (DWORD i = 0; i < cbBinary; i += 3) {
        uint32_t n = static_cast<uint32_t>(pbBinary[i]) << 16;
        if (i + 1 < cbBinary) n |= static_cast<uint32_t>(pbBinary[i + 1]) << 8;
        if (i + 2 < cbBinary) n |= static_cast<uint32_t>(pbBinary[i + 2]);
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back((i + 1 < cbBinary) ? kB64[(n >> 6) & 63] : L'=');
        out.push_back((i + 2 < cbBinary) ? kB64[n & 63] : L'=');
    }
    DWORD needed = static_cast<DWORD>(out.size() + 1);
    if (!pszString) {
        *pcchString = needed;
        return TRUE;
    }
    if (*pcchString < needed) return FALSE;
    wcscpy_s(pszString, *pcchString, out.c_str());
    *pcchString = static_cast<DWORD>(out.size());
    return TRUE;
}

#endif // !defined(_WIN32)
