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
#if __has_include(<sys/file.h>)
#include <sys/file.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {
constexpr int kFlockEx = 2;
constexpr int kFlockNb = 4;
constexpr int kFlockUn = 8;

int LinuxFlock(int fd, int op) {
#if __has_include(<sys/file.h>)
    return ::flock(fd, op);
#elif defined(SYS_flock)
    return static_cast<int>(::syscall(SYS_flock, fd, op));
#else
    struct flock fl{};
    fl.l_type = (op & kFlockUn) ? F_UNLCK : F_WRLCK;
    fl.l_whence = SEEK_SET;
    return ::fcntl(fd, (op & kFlockNb) ? F_SETLK : F_SETLKW, &fl);
#endif
}

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

std::string GetRealExeDirUtf8() {
    static const std::string cached = []() -> std::string {
        char buf[PATH_MAX] = {};
        ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        std::string exePath = (len > 0) ? std::string(buf, static_cast<size_t>(len)) : "/usr/bin/aperture";
        size_t slash = exePath.find_last_of('/');
        return (slash != std::string::npos && slash > 0) ? exePath.substr(0, slash) : std::string(".");
    }();
    return cached;
}

std::string GetHomeDirUtf8() {
    const char* home = std::getenv("HOME");
    return (home && *home) ? std::string(home) : std::string("/tmp");
}

std::string ResolveXdgEnvDir(const char* envName, const std::string& fallbackSuffix) {
    const char* val = std::getenv(envName);
    if (val && *val == '/') {
        std::string s(val);
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    }
    return GetHomeDirUtf8() + "/" + fallbackSuffix;
}

std::string GetXdgPicturesBaseUtf8() {
    const char* envPic = std::getenv("XDG_PICTURES_DIR");
    if (envPic && *envPic == '/') {
        std::string s(envPic);
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    }
    std::string cfgHome = ResolveXdgEnvDir("XDG_CONFIG_HOME", ".config");
    std::ifstream in(cfgHome + "/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XDG_PICTURES_DIR=", 0) == 0) {
            std::string v = line.substr(17);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                v = v.substr(1, v.size() - 2);
            }
            if (v.rfind("$HOME/", 0) == 0) {
                v = GetHomeDirUtf8() + v.substr(5);
            }
            if (!v.empty() && v.front() == '/') {
                while (v.size() > 1 && v.back() == '/') v.pop_back();
                return v;
            }
        }
    }
    return GetHomeDirUtf8() + "/Pictures";
}

bool IsSystemBinaryDir(const std::string& dir) {
    return dir.rfind("/usr", 0) == 0 || dir.rfind("/bin", 0) == 0 || dir.rfind("/sbin", 0) == 0 ||
           dir.rfind("/opt", 0) == 0 || dir.rfind("/lib", 0) == 0;
}

std::string LinuxWinPathToPosixRaw(const std::wstring& wpath) {
    if (wpath.empty()) return {};
    std::wstring p = wpath;
    if (p.rfind(L"\\\\?\\", 0) == 0) {
        p = p.substr(4);
    }
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

bool StartsWithPathSegment(const std::string& path, const std::string& prefix, std::string& outRest) {
    if (path.size() < prefix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    if (path.size() == prefix.size()) {
        outRest.clear();
        return true;
    }
    if (path[prefix.size()] == '/') {
        outRest = path.substr(prefix.size());
        return true;
    }
    return false;
}

} // namespace

bool LinuxIsPortableInstall() {
    static const bool cached = []() {
        const std::string exeDir = GetRealExeDirUtf8();
        const std::string localData = exeDir + "/data";
        struct stat st {};
        if (::stat(localData.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            if (::access(localData.c_str(), W_OK) == 0) return true;
        }
        return !IsSystemBinaryDir(exeDir) && (::access(exeDir.c_str(), W_OK) == 0);
    }();
    return cached;
}

std::wstring LinuxGetXdgConfigDir() {
    return LinuxUtf8ToWide(ResolveXdgEnvDir("XDG_CONFIG_HOME", ".config") + "/aperture");
}

std::wstring LinuxGetXdgDataDir() {
    return LinuxUtf8ToWide(ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share") + "/aperture");
}

std::wstring LinuxGetXdgCacheDir() {
    return LinuxUtf8ToWide(ResolveXdgEnvDir("XDG_CACHE_HOME", ".cache") + "/aperture");
}

std::wstring LinuxGetXdgRuntimeDir() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt == '/') {
        struct stat st {};
        if (::stat(rt, &st) == 0 && S_ISDIR(st.st_mode) && ::access(rt, W_OK) == 0) {
            std::string s(rt);
            while (s.size() > 1 && s.back() == '/') s.pop_back();
            return LinuxUtf8ToWide(s);
        }
    }
    return L"/tmp";
}

std::wstring LinuxGetXdgPicturesDir() {
    return LinuxUtf8ToWide(GetXdgPicturesBaseUtf8());
}

HRESULT SHGetFolderPathW(HWND /*hwnd*/, int csidl, HANDLE /*hToken*/, DWORD /*dwFlags*/, LPWSTR pszPath) {
    if (!pszPath) return E_INVALIDARG;
    std::string dir;
    switch (csidl & 0xFF) {
        case CSIDL_PROFILE: dir = GetHomeDirUtf8(); break;
        case CSIDL_APPDATA: dir = ResolveXdgEnvDir("XDG_CONFIG_HOME", ".config"); break;
        case CSIDL_LOCAL_APPDATA: dir = ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share"); break;
        case CSIDL_INTERNET_CACHE: dir = ResolveXdgEnvDir("XDG_CACHE_HOME", ".cache"); break;
        case CSIDL_MYPICTURES: dir = GetXdgPicturesBaseUtf8(); break;
        case CSIDL_MYVIDEO: dir = GetHomeDirUtf8() + "/Videos"; break;
        case CSIDL_MYMUSIC: dir = GetHomeDirUtf8() + "/Music"; break;
        default: dir = ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share"); break;
    }
    std::wstring w = LinuxUtf8ToWide(dir);
    wcscpy_s(pszPath, MAX_PATH, w.c_str());
    return S_OK;
}

HRESULT SHGetKnownFolderPath(const GUID& rfid, DWORD /*dwFlags*/, HANDLE /*hToken*/, PWSTR* ppszPath) {
    if (!ppszPath) return E_POINTER;
    *ppszPath = nullptr;
    std::string dir;
    if (rfid == FOLDERID_RoamingAppData) dir = ResolveXdgEnvDir("XDG_CONFIG_HOME", ".config");
    else if (rfid == FOLDERID_LocalAppData) dir = ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share");
    else if (rfid == FOLDERID_Pictures) dir = GetXdgPicturesBaseUtf8();
    else if (rfid == FOLDERID_Videos) dir = GetHomeDirUtf8() + "/Videos";
    else if (rfid == FOLDERID_Music) dir = GetHomeDirUtf8() + "/Music";
    else dir = ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share");
    std::wstring w = LinuxUtf8ToWide(dir);
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    PWSTR out = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!out) return E_OUTOFMEMORY;
    std::wmemcpy(out, w.c_str(), w.size() + 1);
    *ppszPath = out;
    return S_OK;
}

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
    std::string raw = LinuxWinPathToPosixRaw(wpath);
    if (raw.empty() || LinuxIsPortableInstall()) return raw;

    const std::string dataRoot = ResolveXdgEnvDir("XDG_DATA_HOME", ".local/share") + "/aperture";
    std::string rest;
    if (!StartsWithPathSegment(raw, dataRoot, rest)) return raw;

    std::string sub;
    if (StartsWithPathSegment(rest, "/cache", sub)) {
        return ResolveXdgEnvDir("XDG_CACHE_HOME", ".cache") + "/aperture" + sub;
    }
    if (StartsWithPathSegment(rest, "/screenshots", sub)) {
        return GetXdgPicturesBaseUtf8() + "/Aperture" + sub;
    }
    if (rest == "/data/settings.json" || rest.rfind("/data/settings.json.", 0) == 0) {
        return ResolveXdgEnvDir("XDG_CONFIG_HOME", ".config") + "/aperture" + rest.substr(5);
    }
    if (StartsWithPathSegment(rest, "/fonts", sub) || StartsWithPathSegment(rest, "/locales", sub)) {
        struct stat st {};
        if (::lstat(raw.c_str(), &st) == 0) return raw;
        const std::string candidates[] = {
            GetRealExeDirUtf8() + rest,
            std::string("/usr/share/aperture") + rest,
            std::string("/usr/local/share/aperture") + rest
        };
        for (const auto& c : candidates) {
            if (::lstat(c.c_str(), &st) == 0) return c;
        }
    }
    return raw;
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
    if (!LinuxIsPortableInstall() && (cleanMode.find('w') != std::string::npos || cleanMode.find('a') != std::string::npos)) {
        std::error_code ec;
        fs::path parent = fs::u8path(u8Path).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
    }
    return std::fopen(u8Path.c_str(), cleanMode.c_str());
}

int _wfopen_s(FILE** pFile, const wchar_t* filename, const wchar_t* mode) {
    if (!pFile) return EINVAL;
    *pFile = _wfopen(filename, mode);
    return *pFile ? 0 : (errno ? errno : ENOENT);
}

wchar_t* _wfullpath(wchar_t* absPath, const wchar_t* relPath, size_t maxLength) {
    if (!relPath) return nullptr;
    std::string u8Rel = LinuxWinPathToPosixRaw(relPath);
    std::error_code ec;
    fs::path p = fs::absolute(fs::u8path(u8Rel), ec).lexically_normal();
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

struct LinuxWaitChannel {
    std::mutex m;
    std::condition_variable cv;
    std::atomic<uint64_t> seq{0};
    int wakeFd = -1;
};

struct LinuxSyncState {
    std::mutex mtx;
    std::condition_variable cv;
    bool manualReset = false;
    bool signaled = false;
    bool timerArmed = false;
    std::chrono::steady_clock::time_point dueTime{};
    LONG timerPeriodMs = 0;
    std::vector<std::shared_ptr<LinuxWaitChannel>> waiters;

    std::vector<std::shared_ptr<LinuxWaitChannel>> SnapshotWaitersLocked() const {
        return waiters;
    }
};

struct LinuxHandleObject {
    enum Type { File, Event, Mutex, Timer, Process, Token } type = File;
    int fd = -1;
    std::string path;
    bool deleteOnClose = false;
    bool ownsLock = false;
    std::shared_ptr<LinuxSyncState> sync;
    // Process / Token
    pid_t pid = -1;
    uid_t uid = 0;
    DWORD exitCode = 0;
};

static void NotifyWaitChannelList(const std::vector<std::shared_ptr<LinuxWaitChannel>>& list) {
    for (const auto& w : list) {
        if (!w) continue;
        {
            std::lock_guard<std::mutex> lk(w->m);
            w->seq.fetch_add(1, std::memory_order_acq_rel);
        }
        w->cv.notify_all();
        if (w->wakeFd >= 0) {
            uint8_t one = 1;
            ssize_t unused = ::write(w->wakeFd, &one, sizeof(one));
            (void)unused;
        }
    }
}

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

static int g_fakeExeModuleMarker = 0;
static int g_fakeUser32ModuleMarker = 0;
static int g_fakeWin32ModuleMarker = 0;

static bool IsSyntheticModuleHandle(HMODULE hModule) {
    const void* p = static_cast<const void*>(hModule);
    return p == &g_fakeExeModuleMarker || p == &g_fakeUser32ModuleMarker || p == &g_fakeWin32ModuleMarker;
}

HMODULE GetModuleHandleW(LPCWSTR lpModuleName) {
    if (!lpModuleName || !*lpModuleName) {
        return reinterpret_cast<HMODULE>(&g_fakeExeModuleMarker);
    }
    std::string lower = LinuxWideToUtf8(lpModuleName);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lower.find("user32") != std::string::npos) {
        return reinterpret_cast<HMODULE>(&g_fakeUser32ModuleMarker);
    }
    return reinterpret_cast<HMODULE>(&g_fakeWin32ModuleMarker);
}

BOOL SetDllDirectoryW(LPCWSTR /*lpPathName*/) { return TRUE; }
BOOL SetDefaultDllDirectories(DWORD /*DirectoryFlags*/) { return TRUE; }

BOOL GetModuleHandleExW(DWORD /*dwFlags*/, LPCWSTR /*lpModuleName*/, HMODULE* phModule) {
    if (!phModule) return FALSE;
    *phModule = reinterpret_cast<HMODULE>(&g_fakeExeModuleMarker);
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
    if (u8.size() >= 4) {
        std::string tail = u8.substr(u8.size() - 4);
        for (char& c : tail) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (tail == ".dll" && !fs::exists(u8)) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            return nullptr;
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
    if (hModule == reinterpret_cast<HMODULE>(&g_fakeUser32ModuleMarker)) {
        if (std::strcmp(lpProcName, "GetDpiForWindow") == 0) {
            return reinterpret_cast<FARPROC>(&GetDpiForWindow);
        }
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    if (IsSyntheticModuleHandle(hModule)) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    void* sym = ::dlsym(hModule, lpProcName);
    if (!sym) SetLastError(ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(sym);
}

BOOL FreeLibrary(HMODULE hLibModule) {
    if (!hLibModule || IsSyntheticModuleHandle(hLibModule)) return TRUE;
    ::dlclose(hLibModule);
    return TRUE;
}

DWORD GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR* lpFilePart) {
    if (!lpFileName) return 0;
    std::string u8 = LinuxWinPathToPosixRaw(lpFileName);
    std::error_code ec;
    fs::path absP = fs::absolute(fs::u8path(u8), ec).lexically_normal();
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

    if (wantWrite && !LinuxIsPortableInstall()) {
        std::error_code ec;
        fs::path parent = fs::u8path(u8).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
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
    if (hObject == reinterpret_cast<HANDLE>(0x2000) ||
        hObject == reinterpret_cast<HANDLE>(0x2001)) {
        return TRUE;
    }
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hObject);
    if (obj->type == LinuxHandleObject::File && obj->fd >= 0) {
        ::close(obj->fd);
        if (obj->deleteOnClose && !obj->path.empty()) {
            ::unlink(obj->path.c_str());
        }
    } else if (obj->type == LinuxHandleObject::Mutex && obj->fd >= 0) {
        if (obj->ownsLock) {
            LinuxFlock(obj->fd, kFlockUn);
        }
        ::close(obj->fd);
    }
    obj->sync.reset();
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
namespace {

struct DbusScreenSaverClient {
    std::mutex mtx;
    void* lib = nullptr;
    void* conn = nullptr;
    bool triedInit = false;
    bool inhibited = false;
    uint32_t cookie = 0;
    bool usingGnome = false;

    struct DBusError { const char* name; const char* message; unsigned int dummy[5]; void* pad; };
    int (*dbus_threads_init_default)() = nullptr;
    void (*dbus_error_init)(DBusError*) = nullptr;
    void (*dbus_error_free)(DBusError*) = nullptr;
    int (*dbus_error_is_set)(const DBusError*) = nullptr;
    void* (*dbus_bus_get)(int, DBusError*) = nullptr;
    void* (*dbus_message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    int (*dbus_message_append_args)(void*, int, ...) = nullptr;
    int (*dbus_message_get_args)(void*, DBusError*, int, ...) = nullptr;
    void (*dbus_message_unref)(void*) = nullptr;
    void* (*dbus_connection_send_with_reply_and_block)(void*, void*, int, DBusError*) = nullptr;
    void (*dbus_connection_flush)(void*) = nullptr;

    bool EnsureConnectedLocked() {
        if (conn) return true;
        if (triedInit) return false;
        triedInit = true;
        for (const char* name : {"libdbus-1.so.3", "libdbus-1.so"}) {
            lib = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return false;
        dbus_threads_init_default = reinterpret_cast<decltype(dbus_threads_init_default)>(::dlsym(lib, "dbus_threads_init_default"));
        dbus_error_init = reinterpret_cast<decltype(dbus_error_init)>(::dlsym(lib, "dbus_error_init"));
        dbus_error_free = reinterpret_cast<decltype(dbus_error_free)>(::dlsym(lib, "dbus_error_free"));
        dbus_error_is_set = reinterpret_cast<decltype(dbus_error_is_set)>(::dlsym(lib, "dbus_error_is_set"));
        dbus_bus_get = reinterpret_cast<decltype(dbus_bus_get)>(::dlsym(lib, "dbus_bus_get"));
        dbus_message_new_method_call = reinterpret_cast<decltype(dbus_message_new_method_call)>(::dlsym(lib, "dbus_message_new_method_call"));
        dbus_message_append_args = reinterpret_cast<decltype(dbus_message_append_args)>(::dlsym(lib, "dbus_message_append_args"));
        dbus_message_get_args = reinterpret_cast<decltype(dbus_message_get_args)>(::dlsym(lib, "dbus_message_get_args"));
        dbus_message_unref = reinterpret_cast<decltype(dbus_message_unref)>(::dlsym(lib, "dbus_message_unref"));
        dbus_connection_send_with_reply_and_block = reinterpret_cast<decltype(dbus_connection_send_with_reply_and_block)>(::dlsym(lib, "dbus_connection_send_with_reply_and_block"));
        dbus_connection_flush = reinterpret_cast<decltype(dbus_connection_flush)>(::dlsym(lib, "dbus_connection_flush"));
        if (!dbus_error_init || !dbus_bus_get || !dbus_message_new_method_call || !dbus_message_append_args ||
            !dbus_message_get_args || !dbus_message_unref || !dbus_connection_send_with_reply_and_block) {
            return false;
        }
        if (dbus_threads_init_default) {
            static std::once_flag dbusThreadInitOnce;
            std::call_once(dbusThreadInitOnce, [this]() { dbus_threads_init_default(); });
        }
        DBusError err{};
        dbus_error_init(&err);
        conn = dbus_bus_get(0 /*DBUS_BUS_SESSION*/, &err);
        if (dbus_error_is_set && dbus_error_is_set(&err)) {
            conn = nullptr;
        }
        if (dbus_error_free) dbus_error_free(&err);
        return conn != nullptr;
    }

    void SetInhibited(bool wantInhibit) {
        std::lock_guard<std::mutex> lock(mtx);
        if (wantInhibit == inhibited) return;
        if (!EnsureConnectedLocked()) return;

        constexpr int DBUS_TYPE_UINT32_VAL = 'u';
        constexpr int DBUS_TYPE_STRING_VAL = 's';
        constexpr int DBUS_TYPE_INVALID_VAL = 0;

        if (wantInhibit) {
            const char* app = "Aperture";
            const char* reason = "Media playback";
            DBusError err{};
            dbus_error_init(&err);
            void* msg = dbus_message_new_method_call(
                "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                "org.freedesktop.ScreenSaver", "Inhibit");
            void* reply = nullptr;
            if (msg) {
                if (dbus_message_append_args(msg, DBUS_TYPE_STRING_VAL, &app, DBUS_TYPE_STRING_VAL, &reason, DBUS_TYPE_INVALID_VAL)) {
                    reply = dbus_connection_send_with_reply_and_block(conn, msg, 200, &err);
                }
                dbus_message_unref(msg);
            }
            uint32_t c = 0;
            bool ok = false;
            if (reply) {
                ok = (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32_VAL, &c, DBUS_TYPE_INVALID_VAL) != 0);
                dbus_message_unref(reply);
            }
            if (dbus_error_free) dbus_error_free(&err);
            if (ok) {
                cookie = c;
                usingGnome = false;
                inhibited = true;
                return;
            }
            // Fallback: org.gnome.SessionManager
            dbus_error_init(&err);
            msg = dbus_message_new_method_call(
                "org.gnome.SessionManager", "/org/gnome/SessionManager",
                "org.gnome.SessionManager", "Inhibit");
            reply = nullptr;
            if (msg) {
                uint32_t toplevelXid = 0;
                uint32_t flags = 8; // Inhibit idle
                if (dbus_message_append_args(msg,
                        DBUS_TYPE_STRING_VAL, &app,
                        DBUS_TYPE_UINT32_VAL, &toplevelXid,
                        DBUS_TYPE_STRING_VAL, &reason,
                        DBUS_TYPE_UINT32_VAL, &flags,
                        DBUS_TYPE_INVALID_VAL)) {
                    reply = dbus_connection_send_with_reply_and_block(conn, msg, 200, &err);
                }
                dbus_message_unref(msg);
            }
            if (reply) {
                ok = (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32_VAL, &c, DBUS_TYPE_INVALID_VAL) != 0);
                dbus_message_unref(reply);
            }
            if (dbus_error_free) dbus_error_free(&err);
            if (ok) {
                cookie = c;
                usingGnome = true;
                inhibited = true;
            }
        } else {
            const char* dest = usingGnome ? "org.gnome.SessionManager" : "org.freedesktop.ScreenSaver";
            const char* path = usingGnome ? "/org/gnome/SessionManager" : "/org/freedesktop/ScreenSaver";
            const char* method = usingGnome ? "Uninhibit" : "UnInhibit";
            void* msg = dbus_message_new_method_call(dest, path, dest, method);
            if (msg) {
                uint32_t c = cookie;
                dbus_message_append_args(msg, DBUS_TYPE_UINT32_VAL, &c, DBUS_TYPE_INVALID_VAL);
                DBusError err{};
                dbus_error_init(&err);
                void* reply = dbus_connection_send_with_reply_and_block(conn, msg, 200, &err);
                if (reply) dbus_message_unref(reply);
                if (dbus_error_free) dbus_error_free(&err);
                dbus_message_unref(msg);
            }
            inhibited = false;
            cookie = 0;
        }
    }
};

DbusScreenSaverClient& GetScreenSaverClient() {
    static DbusScreenSaverClient instance;
    return instance;
}

std::atomic<EXECUTION_STATE> g_execState{ES_CONTINUOUS};

} // namespace

EXECUTION_STATE SetThreadExecutionState(EXECUTION_STATE esFlags) {
    EXECUTION_STATE prev = g_execState.exchange(esFlags, std::memory_order_acq_rel);
    bool wantInhibit = ((esFlags & ES_CONTINUOUS) != 0) &&
                       ((esFlags & (ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)) != 0);
    GetScreenSaverClient().SetInhibited(wantInhibit);
    return prev;
}
BOOL ProcessIdToSessionId(DWORD dwProcessId, DWORD* pSessionId) {
    pid_t sid = ::getsid(static_cast<pid_t>(dwProcessId));
    if (sid < 0) return FALSE;
    if (pSessionId) *pSessionId = static_cast<DWORD>(sid);
    return TRUE;
}

BOOL OpenProcessToken(HANDLE ProcessHandle, DWORD /*DesiredAccess*/, PHANDLE TokenHandle) {
    if (!TokenHandle || !ProcessHandle || ProcessHandle == INVALID_HANDLE_VALUE) return FALSE;
    pid_t pid = ::getpid();
    uid_t uid = ::getuid();
    if (ProcessHandle != reinterpret_cast<HANDLE>(0x2000)) {
        auto* procObj = reinterpret_cast<LinuxHandleObject*>(ProcessHandle);
        if (procObj->type != LinuxHandleObject::Process || procObj->pid <= 0) return FALSE;
        pid = procObj->pid;
        std::string statusPath = "/proc/" + std::to_string(pid) + "/status";
        std::ifstream f(statusPath);
        std::string line;
        bool foundUid = false;
        while (std::getline(f, line)) {
            if (line.rfind("Uid:", 0) == 0) {
                unsigned long realUid = 0;
                if (std::sscanf(line.c_str() + 4, "%lu", &realUid) == 1) {
                    uid = static_cast<uid_t>(realUid);
                    foundUid = true;
                }
                break;
            }
        }
        if (!foundUid) return FALSE;
    }
    auto* tok = new LinuxHandleObject();
    tok->type = LinuxHandleObject::Token;
    tok->pid = pid;
    tok->uid = uid;
    *TokenHandle = reinterpret_cast<HANDLE>(tok);
    return TRUE;
}

BOOL GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, PDWORD ReturnLength) {
    if (!TokenHandle || TokenHandle == INVALID_HANDLE_VALUE) return FALSE;
    auto* tok = reinterpret_cast<LinuxHandleObject*>(TokenHandle);
    if (tok->type != LinuxHandleObject::Token) return FALSE;
    if (TokenInformationClass == TokenSessionId) {
        if (ReturnLength) *ReturnLength = sizeof(DWORD);
        if (!TokenInformation || TokenInformationLength < sizeof(DWORD)) return FALSE;
        pid_t sid = ::getsid(tok->pid);
        *static_cast<DWORD*>(TokenInformation) = (sid >= 0) ? static_cast<DWORD>(sid) : 0;
        return sid >= 0 ? TRUE : FALSE;
    }
    if (TokenInformationClass == TokenUser) {
        if (ReturnLength) *ReturnLength = sizeof(TOKEN_USER) + sizeof(uint32_t);
        if (!TokenInformation || TokenInformationLength < sizeof(TOKEN_USER) + sizeof(uint32_t)) return FALSE;
        auto* tu = static_cast<PTOKEN_USER>(TokenInformation);
        auto* sid = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(TokenInformation) + sizeof(TOKEN_USER));
        *sid = static_cast<uint32_t>(tok->uid);
        tu->User.Sid = sid;
        tu->User.Attributes = 0;
        return TRUE;
    }
    return FALSE;
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

namespace {

std::mutex g_namedEventsMtx;
std::unordered_map<std::string, std::weak_ptr<LinuxSyncState>> g_namedEvents;

std::string SanitizeIpcToken(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "default";
    return out;
}

std::string GetRuntimeDirUtf8() {
    return LinuxWideToUtf8(LinuxGetXdgRuntimeDir());
}

struct PipeSignal {
    int readFd = -1;
    int writeFd = -1;
};

bool CreateNonBlockingPipe(int fds[2]) {
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

const PipeSignal& GetWakePipe() {
    static const PipeSignal pipeSig = []() {
        PipeSignal p;
        int fds[2] = {-1, -1};
        if (CreateNonBlockingPipe(fds)) {
            p.readFd = fds[0];
            p.writeFd = fds[1];
        }
        return p;
    }();
    return pipeSig;
}

std::atomic<int> g_displayWakeFd{-1};

void DrainPipeFd(int fd) {
    if (fd < 0) return;
    uint8_t buf[64];
    while (::read(fd, buf, sizeof(buf)) > 0) {}
}

bool TryPollProcessExit(pid_t pid, DWORD& outExitCode, DWORD dwMilliseconds) {
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r != 0) {
        if (r > 0 && WIFEXITED(status)) outExitCode = static_cast<DWORD>(WEXITSTATUS(status));
        return true;
    }
    if (dwMilliseconds == 0) return false;

    int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
    if (pidfd >= 0) {
        struct pollfd pfd { pidfd, POLLIN, 0 };
        int timeout = (dwMilliseconds == INFINITE) ? -1 : static_cast<int>(std::min<DWORD>(dwMilliseconds, INT_MAX));
        int pr = 0;
        do {
            pr = ::poll(&pfd, 1, timeout);
        } while (pr < 0 && errno == EINTR && dwMilliseconds == INFINITE);
        ::close(pidfd);
        if (pr > 0) {
            r = ::waitpid(pid, &status, WNOHANG);
            if (r != 0) {
                if (r > 0 && WIFEXITED(status)) outExitCode = static_cast<DWORD>(WEXITSTATUS(status));
                return true;
            }
        }
        return false;
    }

    // Fallback if pidfd_open is unavailable (e.g. older kernel or seccomp)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(dwMilliseconds);
    int backoffMs = 1;
    while (true) {
        r = ::waitpid(pid, &status, WNOHANG);
        if (r != 0) {
            if (r > 0 && WIFEXITED(status)) outExitCode = static_cast<DWORD>(WEXITSTATUS(status));
            return true;
        }
        if (dwMilliseconds != INFINITE) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            auto remMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            int sleepMs = static_cast<int>(std::min<int64_t>(backoffMs, std::max<int64_t>(1, remMs)));
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
        }
        backoffMs = std::min(backoffMs * 2, 25);
    }
}

bool CheckHandleSignaledNonBlocking(LinuxHandleObject* obj, bool consume, bool* outHasTimerDue = nullptr,
                                    std::chrono::steady_clock::time_point* outTimerDue = nullptr) {
    if (!obj) return false;
    if (obj->type == LinuxHandleObject::Process) {
        int status = 0;
        pid_t r = ::waitpid(obj->pid, &status, WNOHANG);
        if (r != 0) {
            if (r > 0 && WIFEXITED(status)) obj->exitCode = static_cast<DWORD>(WEXITSTATUS(status));
            return true;
        }
        return false;
    }
    if (!obj->sync) return false;
    std::lock_guard<std::mutex> lock(obj->sync->mtx);
    if (obj->type == LinuxHandleObject::Timer && obj->sync->timerArmed) {
        auto now = std::chrono::steady_clock::now();
        if (now >= obj->sync->dueTime) {
            if (consume && !obj->sync->manualReset) obj->sync->timerArmed = false;
            return true;
        }
        if (outHasTimerDue && (!*outHasTimerDue || obj->sync->dueTime < *outTimerDue)) {
            *outHasTimerDue = true;
            *outTimerDue = obj->sync->dueTime;
        }
        return false;
    }
    if (obj->sync->signaled) {
        if (consume && !obj->sync->manualReset) obj->sync->signaled = false;
        return true;
    }
    return false;
}

void RegisterWaitChannel(LinuxHandleObject* obj, const std::shared_ptr<LinuxWaitChannel>& wc) {
    if (!obj || !obj->sync || !wc) return;
    std::lock_guard<std::mutex> lock(obj->sync->mtx);
    obj->sync->waiters.push_back(wc);
}

void UnregisterWaitChannel(LinuxHandleObject* obj, const std::shared_ptr<LinuxWaitChannel>& wc) {
    if (!obj || !obj->sync || !wc) return;
    std::lock_guard<std::mutex> lock(obj->sync->mtx);
    auto& v = obj->sync->waiters;
    v.erase(std::remove(v.begin(), v.end(), wc), v.end());
}

} // namespace

void LinuxWakeMessageLoop() {
    int fd = GetWakePipe().writeFd;
    if (fd >= 0) {
        uint8_t one = 1;
        ssize_t unused = ::write(fd, &one, sizeof(one));
        (void)unused;
    }
}

int LinuxGetMessageWakeFd() {
    return GetWakePipe().readFd;
}

void LinuxSetDisplayWakeFd(int fd) {
    g_displayWakeFd.store(fd, std::memory_order_release);
}

HANDLE CreateEventW(LPSECURITY_ATTRIBUTES /*lpEventAttributes*/, BOOL bManualReset, BOOL bInitialState, LPCWSTR lpName) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Event;
    if (lpName && *lpName) {
        std::string key = LinuxWideToUtf8(lpName);
        obj->path = key;
        std::lock_guard<std::mutex> lk(g_namedEventsMtx);
        auto it = g_namedEvents.find(key);
        std::shared_ptr<LinuxSyncState> existing = (it != g_namedEvents.end()) ? it->second.lock() : nullptr;
        if (existing) {
            obj->sync = std::move(existing);
            SetLastError(ERROR_ALREADY_EXISTS);
        } else {
            obj->sync = std::make_shared<LinuxSyncState>();
            obj->sync->manualReset = (bManualReset != FALSE);
            obj->sync->signaled = (bInitialState != FALSE);
            g_namedEvents[key] = obj->sync;
            SetLastError(ERROR_SUCCESS);
        }
    } else {
        obj->sync = std::make_shared<LinuxSyncState>();
        obj->sync->manualReset = (bManualReset != FALSE);
        obj->sync->signaled = (bInitialState != FALSE);
        SetLastError(ERROR_SUCCESS);
    }
    return reinterpret_cast<HANDLE>(obj);
}

HANDLE OpenEventW(DWORD /*dwDesiredAccess*/, BOOL /*bInheritHandle*/, LPCWSTR lpName) {
    if (!lpName || !*lpName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::string key = LinuxWideToUtf8(lpName);
    std::shared_ptr<LinuxSyncState> entry;
    {
        std::lock_guard<std::mutex> lk(g_namedEventsMtx);
        auto it = g_namedEvents.find(key);
        if (it != g_namedEvents.end()) {
            entry = it->second.lock();
            if (!entry) g_namedEvents.erase(it);
        }
    }
    if (!entry) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return nullptr;
    }
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Event;
    obj->path = key;
    obj->sync = std::move(entry);
    SetLastError(ERROR_SUCCESS);
    return reinterpret_cast<HANDLE>(obj);
}

BOOL SetEvent(HANDLE hEvent) {
    if (!hEvent || hEvent == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hEvent);
    if (!obj->sync) return FALSE;
    std::vector<std::shared_ptr<LinuxWaitChannel>> toNotify;
    {
        std::lock_guard<std::mutex> lock(obj->sync->mtx);
        obj->sync->signaled = true;
        toNotify = obj->sync->SnapshotWaitersLocked();
        if (obj->sync->manualReset) obj->sync->cv.notify_all();
        else obj->sync->cv.notify_one();
    }
    NotifyWaitChannelList(toNotify);
    return TRUE;
}

BOOL ResetEvent(HANDLE hEvent) {
    if (!hEvent || hEvent == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hEvent);
    if (!obj->sync) return FALSE;
    std::lock_guard<std::mutex> lock(obj->sync->mtx);
    obj->sync->signaled = false;
    return TRUE;
}

HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES /*lpMutexAttributes*/, BOOL bInitialOwner, LPCWSTR lpName) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Mutex;
    obj->sync = std::make_shared<LinuxSyncState>();
    obj->sync->signaled = !bInitialOwner;
    if (lpName && *lpName) {
        std::string sanitized = SanitizeIpcToken(LinuxWideToUtf8(lpName));
        std::string lockPath = GetRuntimeDirUtf8() + "/aperture-mutex-" + std::to_string(::getuid()) + "-" + sanitized + ".lock";
        int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (LinuxFlock(fd, kFlockEx | kFlockNb) == 0) {
                obj->fd = fd;
                obj->path = lockPath;
                obj->ownsLock = true;
                SetLastError(ERROR_SUCCESS);
            } else {
                ::close(fd);
                obj->fd = -1;
                obj->ownsLock = false;
                SetLastError(ERROR_ALREADY_EXISTS);
            }
        } else {
            SetLastError(ErrnoToWin32(errno));
        }
    } else {
        SetLastError(ERROR_SUCCESS);
    }
    return reinterpret_cast<HANDLE>(obj);
}

BOOL ReleaseMutex(HANDLE hMutex) {
    if (!hMutex || hMutex == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hMutex);
    if (obj->type == LinuxHandleObject::Mutex && obj->fd >= 0 && obj->ownsLock) {
        LinuxFlock(obj->fd, kFlockUn);
        obj->ownsLock = false;
    }
    return SetEvent(hMutex);
}

HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES /*lpTimerAttributes*/, LPCWSTR /*lpTimerName*/, DWORD dwFlags, DWORD /*dwDesiredAccess*/) {
    auto* obj = new LinuxHandleObject();
    obj->type = LinuxHandleObject::Timer;
    obj->sync = std::make_shared<LinuxSyncState>();
    obj->sync->manualReset = (dwFlags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0;
    obj->sync->signaled = false;
    return reinterpret_cast<HANDLE>(obj);
}

BOOL SetWaitableTimer(HANDLE hTimer, const LARGE_INTEGER* lpDueTime, LONG lPeriod,
                      void* /*pfnCompletionRoutine*/, LPVOID /*lpArgToCompletionRoutine*/, BOOL /*fResume*/) {
    if (!hTimer || hTimer == INVALID_HANDLE_VALUE || !lpDueTime) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hTimer);
    if (!obj->sync) return FALSE;
    std::vector<std::shared_ptr<LinuxWaitChannel>> toNotify;
    {
        std::lock_guard<std::mutex> lock(obj->sync->mtx);
        obj->sync->signaled = false;
        obj->sync->timerArmed = true;
        obj->sync->timerPeriodMs = lPeriod;
        int64_t val = lpDueTime->QuadPart;
        if (val <= 0) {
            auto ns = std::chrono::nanoseconds((-val) * 100LL);
            obj->sync->dueTime = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(ns);
        } else {
            obj->sync->dueTime = std::chrono::steady_clock::now();
        }
        toNotify = obj->sync->SnapshotWaitersLocked();
        obj->sync->cv.notify_all();
    }
    NotifyWaitChannelList(toNotify);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE hTimer) {
    if (!hTimer || hTimer == INVALID_HANDLE_VALUE) return FALSE;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hTimer);
    if (!obj->sync) return FALSE;
    std::lock_guard<std::mutex> lock(obj->sync->mtx);
    obj->sync->timerArmed = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
    if (!hHandle || hHandle == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    auto* obj = reinterpret_cast<LinuxHandleObject*>(hHandle);
    if (obj->type == LinuxHandleObject::Process) {
        return TryPollProcessExit(obj->pid, obj->exitCode, dwMilliseconds) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    if (!obj->sync) return WAIT_FAILED;
    std::unique_lock<std::mutex> lock(obj->sync->mtx);
    if (obj->type == LinuxHandleObject::Timer && obj->sync->timerArmed) {
        auto now = std::chrono::steady_clock::now();
        if (now >= obj->sync->dueTime) {
            if (!obj->sync->manualReset) obj->sync->timerArmed = false;
            return WAIT_OBJECT_0;
        }
        if (dwMilliseconds == 0) return WAIT_TIMEOUT;
        auto maxWait = (dwMilliseconds == INFINITE)
                           ? obj->sync->dueTime
                           : std::min(obj->sync->dueTime, now + std::chrono::milliseconds(dwMilliseconds));
        obj->sync->cv.wait_until(lock, maxWait);
        if (std::chrono::steady_clock::now() >= obj->sync->dueTime && obj->sync->timerArmed) {
            if (!obj->sync->manualReset) obj->sync->timerArmed = false;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }

    if (obj->sync->signaled) {
        if (!obj->sync->manualReset) obj->sync->signaled = false;
        return WAIT_OBJECT_0;
    }
    if (dwMilliseconds == 0) return WAIT_TIMEOUT;
    if (dwMilliseconds == INFINITE) {
        obj->sync->cv.wait(lock, [&] { return obj->sync->signaled; });
    } else {
        if (!obj->sync->cv.wait_for(lock, std::chrono::milliseconds(dwMilliseconds), [&] { return obj->sync->signaled; })) {
            return WAIT_TIMEOUT;
        }
    }
    if (!obj->sync->manualReset) obj->sync->signaled = false;
    return WAIT_OBJECT_0;
}

DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE* lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
    if (nCount == 0 || !lpHandles) return WAIT_FAILED;
    if (nCount == 1) return WaitForSingleObject(lpHandles[0], dwMilliseconds);

    auto wc = std::make_shared<LinuxWaitChannel>();
    for (DWORD i = 0; i < nCount; ++i) {
        if (!lpHandles[i] || lpHandles[i] == INVALID_HANDLE_VALUE) {
            for (DWORD j = 0; j < i; ++j) UnregisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(lpHandles[j]), wc);
            return WAIT_FAILED;
        }
        RegisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(lpHandles[i]), wc);
    }
    struct Guard {
        DWORD count;
        const HANDLE* handles;
        std::shared_ptr<LinuxWaitChannel> ch;
        ~Guard() {
            for (DWORD i = 0; i < count; ++i) {
                UnregisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(handles[i]), ch);
            }
        }
    } guard{nCount, lpHandles, wc};

    const bool infinite = (dwMilliseconds == INFINITE);
    const auto overallDeadline = infinite
                                     ? std::chrono::steady_clock::time_point::max()
                                     : (std::chrono::steady_clock::now() + std::chrono::milliseconds(dwMilliseconds));

    while (true) {
        uint64_t seenSeq = wc->seq.load(std::memory_order_acquire);
        bool hasTimerDue = false;
        std::chrono::steady_clock::time_point earliestTimerDue{};

        if (!bWaitAll) {
            for (DWORD i = 0; i < nCount; ++i) {
                auto* obj = reinterpret_cast<LinuxHandleObject*>(lpHandles[i]);
                if (CheckHandleSignaledNonBlocking(obj, true, &hasTimerDue, &earliestTimerDue)) {
                    return WAIT_OBJECT_0 + i;
                }
            }
        } else {
            bool allReady = true;
            for (DWORD i = 0; i < nCount; ++i) {
                auto* obj = reinterpret_cast<LinuxHandleObject*>(lpHandles[i]);
                if (!CheckHandleSignaledNonBlocking(obj, false, &hasTimerDue, &earliestTimerDue)) {
                    allReady = false;
                }
            }
            if (allReady) {
                for (DWORD i = 0; i < nCount; ++i) {
                    CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(lpHandles[i]), true);
                }
                return WAIT_OBJECT_0;
            }
        }

        if (dwMilliseconds == 0) return WAIT_TIMEOUT;
        auto now = std::chrono::steady_clock::now();
        if (!infinite && now >= overallDeadline) return WAIT_TIMEOUT;

        auto waitUntil = overallDeadline;
        if (hasTimerDue && earliestTimerDue < waitUntil) waitUntil = earliestTimerDue;
        // If any handle is a Process, bound poll interval cleanly via pid check
        for (DWORD i = 0; i < nCount; ++i) {
            auto* obj = reinterpret_cast<LinuxHandleObject*>(lpHandles[i]);
            if (obj->type == LinuxHandleObject::Process) {
                auto procCheck = now + std::chrono::milliseconds(25);
                if (procCheck < waitUntil) waitUntil = procCheck;
                break;
            }
        }

        std::unique_lock<std::mutex> lk(wc->m);
        if (waitUntil == std::chrono::steady_clock::time_point::max()) {
            wc->cv.wait(lk, [&] { return wc->seq.load(std::memory_order_acquire) != seenSeq; });
        } else {
            wc->cv.wait_until(lk, waitUntil, [&] { return wc->seq.load(std::memory_order_acquire) != seenSeq; });
        }
    }
}

DWORD MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles, BOOL fWaitAll, DWORD dwMilliseconds, DWORD /*dwWakeMask*/) {
    const PipeSignal& wakePipe = GetWakePipe();
    const int wakeFd = wakePipe.readFd;
    if (nCount == 0 || !pHandles) {
        if (wakeFd < 0) {
            if (dwMilliseconds > 0 && dwMilliseconds != INFINITE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(dwMilliseconds));
            }
            return WAIT_TIMEOUT;
        }
        struct pollfd pfds[2] = {};
        nfds_t nfds = 1;
        pfds[0].fd = wakeFd;
        pfds[0].events = POLLIN;
        int dispFd = g_displayWakeFd.load(std::memory_order_acquire);
        if (dispFd >= 0) {
            pfds[1].fd = dispFd;
            pfds[1].events = POLLIN;
            nfds = 2;
        }
        int timeout = (dwMilliseconds == INFINITE) ? -1 : static_cast<int>(std::min<DWORD>(dwMilliseconds, static_cast<DWORD>(INT_MAX)));
        int pr = ::poll(pfds, nfds, timeout);
        if (pr > 0) {
            if (pfds[0].revents & POLLIN) DrainPipeFd(wakeFd);
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }

    // With kernel/user handles: check non-blocking first, then poll wakeFd alongside handle notifications
    auto wc = std::make_shared<LinuxWaitChannel>();
    wc->wakeFd = wakePipe.writeFd;
    for (DWORD i = 0; i < nCount; ++i) {
        if (!pHandles[i] || pHandles[i] == INVALID_HANDLE_VALUE) {
            for (DWORD j = 0; j < i; ++j) UnregisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(pHandles[j]), wc);
            return WAIT_FAILED;
        }
        RegisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), wc);
    }
    struct Guard {
        DWORD count;
        const HANDLE* handles;
        std::shared_ptr<LinuxWaitChannel> ch;
        ~Guard() {
            for (DWORD i = 0; i < count; ++i) {
                UnregisterWaitChannel(reinterpret_cast<LinuxHandleObject*>(handles[i]), ch);
            }
        }
    } guard{nCount, pHandles, wc};

    const bool infinite = (dwMilliseconds == INFINITE);
    const auto overallDeadline = infinite
                                     ? std::chrono::steady_clock::time_point::max()
                                     : (std::chrono::steady_clock::now() + std::chrono::milliseconds(dwMilliseconds));

    while (true) {
        bool hasTimerDue = false;
        std::chrono::steady_clock::time_point earliestTimerDue{};
        if (!fWaitAll) {
            for (DWORD i = 0; i < nCount; ++i) {
                if (CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), true, &hasTimerDue, &earliestTimerDue)) {
                    return WAIT_OBJECT_0 + i;
                }
            }
        } else {
            bool allReady = true;
            for (DWORD i = 0; i < nCount; ++i) {
                if (!CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), false, &hasTimerDue, &earliestTimerDue)) {
                    allReady = false;
                }
            }
            if (allReady) {
                for (DWORD i = 0; i < nCount; ++i) {
                    CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), true);
                }
                return WAIT_OBJECT_0;
            }
        }
        if (dwMilliseconds == 0) return WAIT_TIMEOUT;
        auto now = std::chrono::steady_clock::now();
        if (!infinite && now >= overallDeadline) return WAIT_TIMEOUT;

        auto waitUntil = overallDeadline;
        if (hasTimerDue && earliestTimerDue < waitUntil) waitUntil = earliestTimerDue;
        int timeoutMs = -1;
        if (waitUntil != std::chrono::steady_clock::time_point::max()) {
            auto remMs = std::chrono::duration_cast<std::chrono::milliseconds>(waitUntil - now).count();
            timeoutMs = static_cast<int>(std::clamp<int64_t>(remMs, 0, INT_MAX));
        }

        struct pollfd pfds[2] = {};
        nfds_t nfds = 0;
        if (wakeFd >= 0) {
            pfds[nfds].fd = wakeFd;
            pfds[nfds].events = POLLIN;
            ++nfds;
        }
        int dispFd = g_displayWakeFd.load(std::memory_order_acquire);
        if (dispFd >= 0) {
            pfds[nfds].fd = dispFd;
            pfds[nfds].events = POLLIN;
            ++nfds;
        }
        if (nfds == 0) return WAIT_TIMEOUT;
        int pr = ::poll(pfds, nfds, timeoutMs);
        if (pr == 0) return WAIT_TIMEOUT;
        if (pr > 0) {
            bool wokeMsg = false;
            if (wakeFd >= 0 && (pfds[0].revents & POLLIN)) {
                DrainPipeFd(wakeFd);
                wokeMsg = true;
            }
            if (dispFd >= 0 && (pfds[nfds - 1].revents & POLLIN)) {
                wokeMsg = true;
            }
            // Re-check handles first; if none signaled, report input message available
            if (!fWaitAll) {
                for (DWORD i = 0; i < nCount; ++i) {
                    if (CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), true)) {
                        return WAIT_OBJECT_0 + i;
                    }
                }
            } else {
                bool allReady = true;
                for (DWORD i = 0; i < nCount; ++i) {
                    if (!CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), false)) {
                        allReady = false;
                    }
                }
                if (allReady) {
                    for (DWORD i = 0; i < nCount; ++i) {
                        CheckHandleSignaledNonBlocking(reinterpret_cast<LinuxHandleObject*>(pHandles[i]), true);
                    }
                    return WAIT_OBJECT_0;
                }
            }
            if (wokeMsg) return WAIT_OBJECT_0 + nCount;
        }
    }
}

// ============================================================================
// Cross-Process Single-Instance IPC (FindWindowW + WM_COPYDATA via Unix Socket)
// ============================================================================

namespace {

constexpr uint32_t kIpcWireMagic = 0x49545041u; // 'APTI'
constexpr uint32_t kMaxIpcWireBytes = 2u * 1024u * 1024u;

struct IpcWireHeader {
    uint32_t magic;
    uint64_t dwData;
    uint32_t cbData;
};

struct LocalWindowEntry {
    HWND hwnd = nullptr;
    std::string className;
};

struct PendingCopyDataItem {
    HWND targetHwnd = nullptr;
    ULONG_PTR dwData = 0;
    std::vector<uint8_t> payload;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    LRESULT result = 0;
};

std::mutex g_ipcMutex;
std::vector<LocalWindowEntry> g_localWindows;
std::unordered_map<uintptr_t, std::string> g_remoteWindows;
uintptr_t g_nextRemoteHwndId = 0xFFFF0001ULL;

int g_ipcListenFd = -1;
int g_ipcStopRdFd = -1;
int g_ipcStopWrFd = -1;
HWND g_ipcServerHwnd = nullptr;
std::string g_ipcSocketPath;
std::thread g_ipcThread;
std::atomic<bool> g_ipcStopping{false};

std::mutex g_pendingIpcMtx;
std::vector<std::shared_ptr<PendingCopyDataItem>> g_pendingIpcQueue;
std::vector<std::weak_ptr<PendingCopyDataItem>> g_ipcInFlightItems;

std::string MakeIpcSocketPath(const std::string& className) {
    std::string cls = className.empty() ? "Aperture_Native_Direct2D_Class" : className;
    return GetRuntimeDirUtf8() + "/aperture-ipc-" + std::to_string(::getuid()) + "-" + SanitizeIpcToken(cls) + ".sock";
}

bool ReadExactWithTimeout(int fd, void* dst, size_t len, int timeoutMs) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < len) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd { fd, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, std::max(1, rem));
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            return false;
        }
        ssize_t n = ::read(fd, p + got, len - got);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool WriteExactWithTimeout(int fd, const void* src, size_t len, int timeoutMs) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (sent < len) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd pfd { fd, POLLOUT, 0 };
        int pr = ::poll(&pfd, 1, std::max(1, rem));
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue;
            return false;
        }
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool IsSocketAccepting(const std::string& sockPath) {
    if (sockPath.empty() || sockPath.size() >= sizeof(sockaddr_un::sun_path)) return false;
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    return ok;
}

void IpcServerThreadMain(int listenFd, int stopFd, HWND targetHwnd) {
    while (!g_ipcStopping.load(std::memory_order_acquire)) {
        struct pollfd pfds[2] = {
            { listenFd, POLLIN, 0 },
            { stopFd, POLLIN, 0 }
        };
        int pr = ::poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        int cfd = ::accept(listenFd, nullptr, nullptr);
        if (cfd < 0) continue;
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);

        struct ucred cred{};
        socklen_t credLen = sizeof(cred);
        if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0 || cred.uid != ::getuid()) {
            ::close(cfd);
            continue;
        }

        IpcWireHeader hdr{};
        if (!ReadExactWithTimeout(cfd, &hdr, sizeof(hdr), 2000) ||
            hdr.magic != kIpcWireMagic || hdr.cbData == 0 || hdr.cbData > kMaxIpcWireBytes) {
            ::close(cfd);
            continue;
        }

        auto item = std::make_shared<PendingCopyDataItem>();
        item->targetHwnd = targetHwnd;
        item->dwData = static_cast<ULONG_PTR>(hdr.dwData);
        item->payload.resize(hdr.cbData);
        if (!ReadExactWithTimeout(cfd, item->payload.data(), hdr.cbData, 2000)) {
            ::close(cfd);
            continue;
        }

        {
            std::lock_guard<std::mutex> qlk(g_pendingIpcMtx);
            g_pendingIpcQueue.push_back(item);
            g_ipcInFlightItems.erase(
                std::remove_if(g_ipcInFlightItems.begin(), g_ipcInFlightItems.end(),
                               [](const std::weak_ptr<PendingCopyDataItem>& w) { return w.expired(); }),
                g_ipcInFlightItems.end());
            g_ipcInFlightItems.push_back(item);
        }
        LinuxWakeMessageLoop();

        int32_t reply = 0;
        {
            std::unique_lock<std::mutex> lk(item->mtx);
            if (item->cv.wait_for(lk, std::chrono::milliseconds(4500), [&] {
                    return item->done || g_ipcStopping.load(std::memory_order_acquire);
                }) && item->done) {
                reply = (item->result != 0) ? 1 : 0;
            }
        }
        (void)WriteExactWithTimeout(cfd, &reply, sizeof(reply), 500);
        ::close(cfd);
    }
}

void StopIpcServerLocked(std::unique_lock<std::mutex>& lk) {
    g_ipcStopping.store(true, std::memory_order_release);
    if (g_ipcStopWrFd >= 0) {
        uint8_t one = 1;
        ssize_t unused = ::write(g_ipcStopWrFd, &one, sizeof(one));
        (void)unused;
    }
    if (g_ipcListenFd >= 0) {
        ::shutdown(g_ipcListenFd, SHUT_RDWR);
    }
    // Fail and wake any queued or in-flight callers BEFORE joining the IPC server thread.
    {
        std::lock_guard<std::mutex> qlk(g_pendingIpcMtx);
        for (auto& item : g_pendingIpcQueue) {
            std::lock_guard<std::mutex> ilk(item->mtx);
            item->done = true;
            item->result = 0;
            item->cv.notify_all();
        }
        g_pendingIpcQueue.clear();
        for (auto& w : g_ipcInFlightItems) {
            if (auto item = w.lock()) {
                std::lock_guard<std::mutex> ilk(item->mtx);
                item->done = true;
                item->result = 0;
                item->cv.notify_all();
            }
        }
        g_ipcInFlightItems.clear();
    }
    std::thread t = std::move(g_ipcThread);
    int lfd = g_ipcListenFd;
    int sRdFd = g_ipcStopRdFd;
    int sWrFd = g_ipcStopWrFd;
    std::string spath = std::move(g_ipcSocketPath);
    g_ipcListenFd = -1;
    g_ipcStopRdFd = -1;
    g_ipcStopWrFd = -1;
    g_ipcServerHwnd = nullptr;
    lk.unlock();

    if (t.joinable()) t.join();
    if (lfd >= 0) ::close(lfd);
    if (sRdFd >= 0) ::close(sRdFd);
    if (sWrFd >= 0) ::close(sWrFd);
    if (!spath.empty()) ::unlink(spath.c_str());

    lk.lock();
}

} // namespace

void LinuxIpcRegisterWindow(HWND hWnd, LPCWSTR lpClassName) {
    if (!hWnd || !lpClassName || !*lpClassName) return;
    std::string cls = LinuxWideToUtf8(lpClassName);
    std::lock_guard<std::mutex> lk(g_ipcMutex);
    for (auto& e : g_localWindows) {
        if (e.hwnd == hWnd) {
            e.className = cls;
            return;
        }
    }
    g_localWindows.push_back({hWnd, cls});

    if (g_ipcListenFd < 0 && cls == "Aperture_Native_Direct2D_Class") {
        std::string sockPath = MakeIpcSocketPath(cls);
        if (sockPath.size() < sizeof(sockaddr_un::sun_path) && !IsSocketAccepting(sockPath)) {
            ::unlink(sockPath.c_str());
            int lfd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int sfds[2] = {-1, -1};
            bool pipeOk = CreateNonBlockingPipe(sfds);
            if (lfd >= 0 && pipeOk) {
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
                if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                    ::listen(lfd, 8) == 0) {
                    ::chmod(sockPath.c_str(), 0600);
                    g_ipcListenFd = lfd;
                    g_ipcStopRdFd = sfds[0];
                    g_ipcStopWrFd = sfds[1];
                    g_ipcServerHwnd = hWnd;
                    g_ipcSocketPath = sockPath;
                    g_ipcStopping.store(false, std::memory_order_release);
                    g_ipcThread = std::thread(IpcServerThreadMain, lfd, sfds[0], hWnd);
                } else {
                    ::close(lfd);
                    ::close(sfds[0]);
                    ::close(sfds[1]);
                }
            } else {
                if (lfd >= 0) ::close(lfd);
                if (pipeOk) {
                    ::close(sfds[0]);
                    ::close(sfds[1]);
                }
            }
        }
    }
}

void LinuxIpcUnregisterWindow(HWND hWnd) {
    if (!hWnd) return;
    std::unique_lock<std::mutex> lk(g_ipcMutex);
    g_localWindows.erase(
        std::remove_if(g_localWindows.begin(), g_localWindows.end(), [hWnd](const LocalWindowEntry& e) { return e.hwnd == hWnd; }),
        g_localWindows.end());
    if (g_ipcServerHwnd == hWnd) {
        StopIpcServerLocked(lk);
    }
}

bool LinuxIpcIsRemoteWindow(HWND hWnd) {
    if (!hWnd) return false;
    std::lock_guard<std::mutex> lk(g_ipcMutex);
    return g_remoteWindows.find(reinterpret_cast<uintptr_t>(hWnd)) != g_remoteWindows.end();
}

HWND FindWindowW(LPCWSTR lpClassName, LPCWSTR /*lpWindowName*/) {
    if (!lpClassName || !*lpClassName) return nullptr;
    std::string cls = LinuxWideToUtf8(lpClassName);
    std::lock_guard<std::mutex> lk(g_ipcMutex);
    for (const auto& e : g_localWindows) {
        if (e.className == cls) return e.hwnd;
    }
    std::string sockPath = MakeIpcSocketPath(cls);
    if (!IsSocketAccepting(sockPath)) return nullptr;
    for (const auto& [id, p] : g_remoteWindows) {
        if (p == sockPath) return reinterpret_cast<HWND>(id);
    }
    uintptr_t id = g_nextRemoteHwndId++;
    g_remoteWindows[id] = sockPath;
    return reinterpret_cast<HWND>(id);
}

LRESULT LinuxIpcSendCopyData(HWND hWnd, const COPYDATASTRUCT* pCDS, UINT uTimeoutMs, DWORD_PTR* lpdwResult) {
    if (lpdwResult) *lpdwResult = 0;
    if (!hWnd || !pCDS || !pCDS->lpData || pCDS->cbData == 0 || pCDS->cbData > kMaxIpcWireBytes) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::string sockPath;
    {
        std::lock_guard<std::mutex> lk(g_ipcMutex);
        auto it = g_remoteWindows.find(reinterpret_cast<uintptr_t>(hWnd));
        if (it == g_remoteWindows.end()) {
            SetLastError(ERROR_INVALID_WINDOW_HANDLE);
            return 0;
        }
        sockPath = it->second;
    }
    if (sockPath.size() >= sizeof(sockaddr_un::sun_path)) return 0;

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        SetLastError(ERROR_TIMEOUT);
        return 0;
    }

    int timeout = static_cast<int>(std::max<UINT>(100, uTimeoutMs ? uTimeoutMs : 5000));
    IpcWireHeader hdr{};
    hdr.magic = kIpcWireMagic;
    hdr.dwData = static_cast<uint64_t>(pCDS->dwData);
    hdr.cbData = static_cast<uint32_t>(pCDS->cbData);

    bool ok = WriteExactWithTimeout(fd, &hdr, sizeof(hdr), timeout) &&
              WriteExactWithTimeout(fd, pCDS->lpData, pCDS->cbData, timeout);
    int32_t reply = 0;
    if (ok) {
        ok = ReadExactWithTimeout(fd, &reply, sizeof(reply), timeout);
    }
    ::close(fd);
    if (!ok) {
        SetLastError(ERROR_TIMEOUT);
        return 0;
    }
    if (lpdwResult) *lpdwResult = static_cast<DWORD_PTR>(reply);
    return reply != 0 ? 1 : 0;
}

void LinuxIpcPumpPending(LinuxDispatchMsgFn fn) {
    if (!fn) return;
    std::vector<std::shared_ptr<PendingCopyDataItem>> batch;
    {
        std::lock_guard<std::mutex> qlk(g_pendingIpcMtx);
        if (g_pendingIpcQueue.empty()) return;
        batch.swap(g_pendingIpcQueue);
    }
    for (auto& item : batch) {
        COPYDATASTRUCT cds{};
        cds.dwData = item->dwData;
        cds.cbData = static_cast<DWORD>(item->payload.size());
        cds.lpData = item->payload.data();
        LRESULT res = fn(item->targetHwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
        {
            std::lock_guard<std::mutex> ilk(item->mtx);
            item->result = res;
            item->done = true;
        }
        item->cv.notify_one();
    }
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

namespace {

struct CachedLibCrypto {
    void* lib = nullptr;
    bool loaded = false;

    void* (*OSSL_PARAM_BLD_new)() = nullptr;
    void (*OSSL_PARAM_BLD_free)(void*) = nullptr;
    int (*OSSL_PARAM_BLD_push_utf8_string)(void*, const char*, const char*, size_t) = nullptr;
    int (*OSSL_PARAM_BLD_push_octet_string)(void*, const char*, const void*, size_t) = nullptr;
    void* (*OSSL_PARAM_BLD_to_param)(void*) = nullptr;
    void (*OSSL_PARAM_free)(void*) = nullptr;

    void* (*EVP_PKEY_CTX_new_from_name)(void*, const char*, const char*) = nullptr;
    void* (*EVP_PKEY_CTX_new_from_pkey)(void*, void*, const char*) = nullptr;
    void (*EVP_PKEY_CTX_free)(void*) = nullptr;
    int (*EVP_PKEY_fromdata_init)(void*) = nullptr;
    int (*EVP_PKEY_fromdata)(void*, void**, int, void*) = nullptr;
    void (*EVP_PKEY_free)(void*) = nullptr;
    int (*EVP_PKEY_verify_init)(void*) = nullptr;
    int (*EVP_PKEY_verify_init_ex)(void*, const void*) = nullptr;
    int (*EVP_PKEY_verify)(void*, const unsigned char*, size_t, const unsigned char*, size_t) = nullptr;

    CachedLibCrypto() {
        for (const char* name : {"libcrypto.so.3", "libcrypto.so"}) {
            lib = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return;
        OSSL_PARAM_BLD_new = reinterpret_cast<decltype(OSSL_PARAM_BLD_new)>(::dlsym(lib, "OSSL_PARAM_BLD_new"));
        OSSL_PARAM_BLD_free = reinterpret_cast<decltype(OSSL_PARAM_BLD_free)>(::dlsym(lib, "OSSL_PARAM_BLD_free"));
        OSSL_PARAM_BLD_push_utf8_string = reinterpret_cast<decltype(OSSL_PARAM_BLD_push_utf8_string)>(::dlsym(lib, "OSSL_PARAM_BLD_push_utf8_string"));
        OSSL_PARAM_BLD_push_octet_string = reinterpret_cast<decltype(OSSL_PARAM_BLD_push_octet_string)>(::dlsym(lib, "OSSL_PARAM_BLD_push_octet_string"));
        OSSL_PARAM_BLD_to_param = reinterpret_cast<decltype(OSSL_PARAM_BLD_to_param)>(::dlsym(lib, "OSSL_PARAM_BLD_to_param"));
        OSSL_PARAM_free = reinterpret_cast<decltype(OSSL_PARAM_free)>(::dlsym(lib, "OSSL_PARAM_free"));
        EVP_PKEY_CTX_new_from_name = reinterpret_cast<decltype(EVP_PKEY_CTX_new_from_name)>(::dlsym(lib, "EVP_PKEY_CTX_new_from_name"));
        EVP_PKEY_CTX_new_from_pkey = reinterpret_cast<decltype(EVP_PKEY_CTX_new_from_pkey)>(::dlsym(lib, "EVP_PKEY_CTX_new_from_pkey"));
        EVP_PKEY_CTX_free = reinterpret_cast<decltype(EVP_PKEY_CTX_free)>(::dlsym(lib, "EVP_PKEY_CTX_free"));
        EVP_PKEY_fromdata_init = reinterpret_cast<decltype(EVP_PKEY_fromdata_init)>(::dlsym(lib, "EVP_PKEY_fromdata_init"));
        EVP_PKEY_fromdata = reinterpret_cast<decltype(EVP_PKEY_fromdata)>(::dlsym(lib, "EVP_PKEY_fromdata"));
        EVP_PKEY_free = reinterpret_cast<decltype(EVP_PKEY_free)>(::dlsym(lib, "EVP_PKEY_free"));
        EVP_PKEY_verify_init = reinterpret_cast<decltype(EVP_PKEY_verify_init)>(::dlsym(lib, "EVP_PKEY_verify_init"));
        EVP_PKEY_verify_init_ex = reinterpret_cast<decltype(EVP_PKEY_verify_init_ex)>(::dlsym(lib, "EVP_PKEY_verify_init_ex"));
        EVP_PKEY_verify = reinterpret_cast<decltype(EVP_PKEY_verify)>(::dlsym(lib, "EVP_PKEY_verify"));
        loaded = OSSL_PARAM_BLD_new && OSSL_PARAM_BLD_free && OSSL_PARAM_BLD_push_utf8_string &&
                 OSSL_PARAM_BLD_push_octet_string && OSSL_PARAM_BLD_to_param && OSSL_PARAM_free &&
                 EVP_PKEY_CTX_new_from_name && EVP_PKEY_CTX_new_from_pkey && EVP_PKEY_CTX_free &&
                 EVP_PKEY_fromdata_init && EVP_PKEY_fromdata && EVP_PKEY_free &&
                 (EVP_PKEY_verify_init_ex || EVP_PKEY_verify_init) && EVP_PKEY_verify;
    }
};

const CachedLibCrypto& GetLibCrypto() {
    static const CachedLibCrypto instance;
    return instance;
}

void AppendDerInt(std::vector<uint8_t>& out, const uint8_t* bytes32) {
    size_t start = 0;
    while (start < 31 && bytes32[start] == 0) ++start;
    bool padZero = (bytes32[start] & 0x80) != 0;
    size_t len = (32 - start) + (padZero ? 1 : 0);
    out.push_back(0x02); // ASN.1 INTEGER
    out.push_back(static_cast<uint8_t>(len));
    if (padZero) out.push_back(0x00);
    out.insert(out.end(), bytes32 + start, bytes32 + 32);
}

std::vector<uint8_t> EncodeEcdsaRawToDer(const uint8_t* sig64) {
    std::vector<uint8_t> body;
    body.reserve(70);
    AppendDerInt(body, sig64);
    AppendDerInt(body, sig64 + 32);
    std::vector<uint8_t> der;
    der.reserve(2 + body.size());
    der.push_back(0x30); // ASN.1 SEQUENCE
    der.push_back(static_cast<uint8_t>(body.size()));
    der.insert(der.end(), body.begin(), body.end());
    return der;
}

} // namespace

NTSTATUS BCryptVerifySignature(BCRYPT_KEY_HANDLE hKey, void* /*pPaddingInfo*/, PUCHAR pbHash, ULONG cbHash,
                               PUCHAR pbSignature, ULONG cbSignature, ULONG /*dwFlags*/) {
    if (!hKey || !pbHash || cbHash != 32 || !pbSignature || cbSignature != 64) return -1;
    const auto& crypto = GetLibCrypto();
    if (!crypto.loaded) return -1;

    auto* key = reinterpret_cast<LinuxBCryptKey*>(hKey);
    if (key->x.size() != 32 || key->y.size() != 32) return -1;

    uint8_t pub[65];
    pub[0] = 0x04; // Uncompressed EC point
    std::memcpy(pub + 1, key->x.data(), 32);
    std::memcpy(pub + 33, key->y.data(), 32);

    void* bld = crypto.OSSL_PARAM_BLD_new();
    if (!bld) return -1;
    int pushed = crypto.OSSL_PARAM_BLD_push_utf8_string(bld, "group", "prime256v1", 0) &&
                 crypto.OSSL_PARAM_BLD_push_octet_string(bld, "pub", pub, sizeof(pub));
    void* params = pushed ? crypto.OSSL_PARAM_BLD_to_param(bld) : nullptr;
    crypto.OSSL_PARAM_BLD_free(bld);
    if (!params) return -1;

    void* kctx = crypto.EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    void* pkey = nullptr;
    constexpr int kEvpPkeyPublicKey = 0x86;
    if (kctx && crypto.EVP_PKEY_fromdata_init(kctx) == 1) {
        (void)crypto.EVP_PKEY_fromdata(kctx, &pkey, kEvpPkeyPublicKey, params);
    }
    if (kctx) crypto.EVP_PKEY_CTX_free(kctx);
    crypto.OSSL_PARAM_free(params);
    if (!pkey) return -1;

    int ok = 0;
    void* vctx = crypto.EVP_PKEY_CTX_new_from_pkey(nullptr, pkey, nullptr);
    if (vctx) {
        int initOk = 0;
        if (crypto.EVP_PKEY_verify_init_ex) {
            void* vbld = crypto.OSSL_PARAM_BLD_new();
            void* vparams = nullptr;
            if (vbld) {
                if (crypto.OSSL_PARAM_BLD_push_utf8_string(vbld, "digest", "SHA256", 0)) {
                    vparams = crypto.OSSL_PARAM_BLD_to_param(vbld);
                }
                crypto.OSSL_PARAM_BLD_free(vbld);
            }
            initOk = crypto.EVP_PKEY_verify_init_ex(vctx, vparams);
            if (vparams) crypto.OSSL_PARAM_free(vparams);
        } else {
            initOk = crypto.EVP_PKEY_verify_init(vctx);
        }
        if (initOk == 1) {
            std::vector<uint8_t> derSig = EncodeEcdsaRawToDer(pbSignature);
            ok = crypto.EVP_PKEY_verify(vctx, derSig.data(), derSig.size(), pbHash, cbHash);
        }
        crypto.EVP_PKEY_CTX_free(vctx);
    }
    crypto.EVP_PKEY_free(pkey);
    return (ok == 1) ? 0 : -1;
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
