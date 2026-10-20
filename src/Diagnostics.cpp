#include "Diagnostics.h"
#include "Common.h"

#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <thread>
#include <vector>

namespace Aperture::Diagnostics {
namespace {

constexpr uint64_t kMaxLogBytes = 1024 * 1024;
constexpr int kRetainedLogs = 3;
constexpr DWORD kHeartbeatPollMilliseconds = 5000;
constexpr ULONGLONG kHangThresholdMilliseconds = 30000;
constexpr size_t kMaxStartupMilestones = 32;
constexpr int kRetainedPerfReports = 20;
std::mutex g_mutex;
std::wstring g_directory;
bool g_loggingEnabled = false;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
std::atomic<ULONGLONG> g_lastHeartbeatTick{ 0 };
std::atomic<bool> g_hangCaptured{ false };
std::atomic<bool> g_shuttingDown{ false };
HANDLE g_watchdogStopEvent = nullptr;
HANDLE g_watchdogThread = nullptr;
struct StartupMilestone {
    std::wstring name;
    int64_t qpc = 0, qpcFreq = 0;
    ULONGLONG tickMs = 0;
    size_t workingSetBytes = 0, privateBytes = 0;
    DWORD threadCount = 0, moduleCount = 0;
    bool resourcesCaptured = false;
};
std::vector<StartupMilestone> g_startupMilestones;

std::wstring Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[32]{};
    swprintf_s(value, L"%04u%02u%02u-%02u%02u%02u-%03u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}

bool EnsureDirectory() {
    if (g_directory.empty()) g_directory = GetExeDir() + L"\\diagnostics";
    const int result = SHCreateDirectoryExW(nullptr, g_directory.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

void RotateLogs() {
    const std::wstring active = g_directory + L"\\aperture.log";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(active.c_str(), GetFileExInfoStandard, &data)) return;
    const uint64_t bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (bytes < kMaxLogBytes) return;
    DeleteFileW((active + L"." + std::to_wstring(kRetainedLogs)).c_str());
    for (int index = kRetainedLogs - 1; index >= 1; --index) {
        const std::wstring from = active + L"." + std::to_wstring(index);
        const std::wstring to = active + L"." + std::to_wstring(index + 1);
        MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    MoveFileExW(active.c_str(), (active + L".1").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// Space bound: keep only the newest kRetainedDumps minidumps and enforce a total byte ceiling so repeated hang/crash captures cannot grow
// diagnostics/ without limit. Dumps stay strictly under <root>\diagnostics and are safe to delete at any time.
void PruneOldDumps() {
    constexpr int kRetainedDumps = 3;
    constexpr uint64_t kMaxDumpBytes = 64ull * 1024ull * 1024ull;
    const std::wstring pattern = g_directory + L"\\*.dmp";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    struct Entry { std::wstring path; uint64_t ft; uint64_t bytes; };
    std::vector<Entry> dumps;
    uint64_t totalBytes = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint64_t ft = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (ft == 0) { ft = (static_cast<uint64_t>(fd.ftCreationTime.dwHighDateTime) << 32) | fd.ftCreationTime.dwLowDateTime; }
        uint64_t bytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        totalBytes += bytes;
        dumps.push_back({ g_directory + L"\\" + fd.cFileName, ft, bytes });
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    if (dumps.empty()) return;
    std::sort(dumps.begin(), dumps.end(), [](const Entry& a, const Entry& b) { return a.ft > b.ft; });
    // Newest-first: delete beyond count cap, then beyond byte cap (oldest first).
    for (size_t i = static_cast<size_t>(kRetainedDumps); i < dumps.size(); ++i) {
        if (DeleteFileW(dumps[i].path.c_str())) { totalBytes = (dumps[i].bytes > totalBytes) ? 0 : (totalBytes - dumps[i].bytes); }
    }
    if (dumps.size() > static_cast<size_t>(kRetainedDumps)) { dumps.resize(static_cast<size_t>(kRetainedDumps)); }
    // Oldest-first within the retained set for the byte ceiling.
    std::sort(dumps.begin(), dumps.end(), [](const Entry& a, const Entry& b) { return a.ft < b.ft; });
    for (const auto& e : dumps) {
        if (totalBytes <= kMaxDumpBytes) break;
        if (DeleteFileW(e.path.c_str())) { totalBytes = (e.bytes > totalBytes) ? 0 : (totalBytes - e.bytes); }
    }
}

// dbghelp is delay-loaded: POD-only gate (HANDLE + POD struct, no C++
// objects, so __try is legal under /EHsc). Presence is checked by the caller.
BOOL DelaySafeMiniDumpWriteDump(HANDLE file, MINIDUMP_EXCEPTION_INFORMATION* infoOrNull) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) return FALSE;
    BOOL written = FALSE;
    __try {
        written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, infoOrNull, nullptr, nullptr);
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { written = FALSE; }
    return written;
}

std::wstring WriteProcessDump(const wchar_t* prefix, EXCEPTION_POINTERS* exceptionPointers) {
    if (!EnsureDirectory()) return {};
    // dbghelp is delay-loaded: system-only presence first so a missing DLL skips dump writing gracefully instead of faulting the crash
    // path. The call itself runs through a POD-only gate (no C++ objects, so
    // __try is legal); the std::wstring path below never crosses SEH.
    if (!SystemDllPresent(L"dbghelp.dll")) return {};
    const std::wstring path = g_directory + L"\\" + prefix + L"-" + Timestamp() + L".dmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = exceptionPointers;
    information.ClientPointers = FALSE;
    BOOL written = DelaySafeMiniDumpWriteDump(file, exceptionPointers ? &information : nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(path.c_str());
        return {};
    }
    PruneOldDumps();
    return path;
}

DWORD WINAPI WatchdogMain(void*) {
    while (WaitForSingleObject(g_watchdogStopEvent, kHeartbeatPollMilliseconds) == WAIT_TIMEOUT) {
        const ULONGLONG last = g_lastHeartbeatTick.load(std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        if (last != 0 && now - last >= kHangThresholdMilliseconds && !g_hangCaptured.exchange(true, std::memory_order_relaxed))
            WriteProcessDump(L"hang", nullptr);
    }
    return 0;
}

LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
    WriteProcessDump(L"crash", exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

bool Initialize(bool loggingEnabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shuttingDown.store(false, std::memory_order_release);
    g_directory = GetExeDir() + L"\\diagnostics";
    g_loggingEnabled = loggingEnabled;
    if (!EnsureDirectory()) return false;
    g_lastHeartbeatTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_hangCaptured.store(false, std::memory_order_relaxed);
    // Crash protection is available from process start without paying for a watchdog thread on the critical startup path. Call
    // StartWatchdog() after the first present.
    g_previousFilter = SetUnhandledExceptionFilter(WriteCrashDump);
    return true;
}

void StartWatchdog() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shuttingDown.load(std::memory_order_acquire) || g_watchdogThread) return;
    g_lastHeartbeatTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_watchdogStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watchdogStopEvent) return;
    g_watchdogThread = CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr);
    if (!g_watchdogThread) { CloseHandle(g_watchdogStopEvent); g_watchdogStopEvent = nullptr; }
}

void Shutdown() {
    g_shuttingDown.store(true, std::memory_order_release);
    HANDLE thread = nullptr;
    HANDLE stopEvent = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        thread = g_watchdogThread;
        stopEvent = g_watchdogStopEvent;
        g_watchdogThread = nullptr;
        g_watchdogStopEvent = nullptr;
        previousFilter = g_previousFilter;
        g_previousFilter = nullptr;
    }
    if (stopEvent) SetEvent(stopEvent);
    if (thread) { WaitForSingleObject(thread, 10000); CloseHandle(thread); }
    if (stopEvent) CloseHandle(stopEvent);
    if (previousFilter) SetUnhandledExceptionFilter(previousFilter);
}

void SetLoggingEnabled(bool enabled) { std::lock_guard<std::mutex> lock(g_mutex); g_loggingEnabled = enabled; }

void Log(const AppError& error) {
    if (g_shuttingDown.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shuttingDown.load(std::memory_order_acquire) || !g_loggingEnabled || !EnsureDirectory() || !error.IsValid()) return;
    RotateLogs();
    const std::wstring path = g_directory + L"\\aperture.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::wostringstream line;
    line << L"[" << Timestamp() << L"] [" << ErrorDomainName(error.domain) << L"]" << L" code=" << error.code << L" system="
         << error.systemError << L" recoverable=" << (error.recoverable ? L"true" : L"false") << L" message=\"" << error.technicalMessage
         << L"\"";
    if (!error.relatedPath.empty()) line << L" path=\"" << error.relatedPath << L"\"";
    line << L"\r\n";
    const std::string utf8 = Utf16ToUtf8(line.str());
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void RecordHeartbeat() {
    if (g_shuttingDown.load(std::memory_order_acquire)) return;
    g_lastHeartbeatTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_hangCaptured.store(false, std::memory_order_relaxed);
}

void RecordStartupMilestone(const std::wstring& name, bool captureResources) {
    StartupMilestone m;
    m.name = name;
    LARGE_INTEGER freq{}, now{};
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&now)) { m.qpcFreq = freq.QuadPart; m.qpc = now.QuadPart; }
    m.tickMs = GetTickCount64();
    if (captureResources) {
        m.resourcesCaptured = true;
        PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(PROCESS_MEMORY_COUNTERS_EX) };
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            m.workingSetBytes = static_cast<size_t>(pmc.WorkingSetSize);
            m.privateBytes = static_cast<size_t>(pmc.PrivateUsage);
        }
        // Thread + module counts are best-effort; failures leave zeros.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            DWORD pid = GetCurrentProcessId();
            THREADENTRY32 te{ sizeof(te) };
            DWORD count = 0;
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid) ++count;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            m.threadCount = count;
        }
        HMODULE mods[512]{};
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) { m.moduleCount = needed / sizeof(HMODULE); }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_startupMilestones.size() >= kMaxStartupMilestones) { return; }
    g_startupMilestones.push_back(std::move(m));
}

void WriteStartupReport() {
    std::vector<StartupMilestone> copy;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_startupMilestones.empty()) return;
        copy = g_startupMilestones;
        if (g_directory.empty()) g_directory = GetExeDir() + L"\\diagnostics";
        dir = g_directory;
    }
    std::stable_sort(copy.begin(), copy.end(), [](const StartupMilestone& a, const StartupMilestone& b) { return a.qpc < b.qpc; });
    std::wstring perfDir = dir + L"\\perf";
    SHCreateDirectoryExW(nullptr, perfDir.c_str(), nullptr);
    std::ostringstream json;
    json << "{\n  \"milestones\": [\n";
    for (size_t i = 0; i < copy.size(); ++i) {
        const auto& m = copy[i];
        double msSinceFirst = 0.0;
        if (i > 0 && copy[0].qpcFreq > 0 && m.qpcFreq > 0) { msSinceFirst = (double)(m.qpc - copy[0].qpc) * 1000.0 / (double)m.qpcFreq; }
        std::string nameUtf8 = Utf16ToUtf8(m.name);
        // Minimal JSON escaping for milestone names (controlled constants).
        std::string esc;
        esc.reserve(nameUtf8.size());
        for (char c : nameUtf8) {
            if (c == '"' || c == '\\') { esc.push_back('\\'); esc.push_back(c); }
            else if (c == '\n') esc += "\\n";
            else esc.push_back(c);
        }
        json << "    {\"name\": \"" << esc << "\", \"msSinceStart\": " << msSinceFirst << ", \"tickMs\": " << m.tickMs
             << ", \"workingSetBytes\": " << m.workingSetBytes << ", \"privateBytes\": " << m.privateBytes
             << ", \"threadCount\": " << m.threadCount << ", \"moduleCount\": " << m.moduleCount
             << ", \"resourcesCaptured\": " << (m.resourcesCaptured ? "true" : "false") << "}";
        if (i + 1 < copy.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n}\n";
    std::string payload = json.str();
    if (payload.size() > 64 * 1024) payload.resize(64 * 1024);
    std::wstring path = perfDir + L"\\startup-" + Timestamp() + L".json";
    AtomicWriteTextFile(path, payload);
    // Retention: keep newest N reports, all strictly under <root>\diagnostics.
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((perfDir + L"\\startup-*.json").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::vector<std::pair<std::wstring, uint64_t>> files;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            uint64_t ft = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            files.emplace_back(perfDir + L"\\" + fd.cFileName, ft);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        if (files.size() > static_cast<size_t>(kRetainedPerfReports)) {
            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = static_cast<size_t>(kRetainedPerfReports); i < files.size(); ++i) { DeleteFileW(files[i].first.c_str()); }
        }
    }
}

} // namespace Aperture::Diagnostics
