#if !defined(_WIN32)

#include "../Common.h"
#include "../FileAssociations.h"
#include "../JumpList.h"
#include "../Diagnostics.h"
#include "../MediaFoundation.h"
#include "../MediaProbeIsolation.h"
#include "../MediaLibrary.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Aperture {

namespace {

void RunDetachedNoShell(const std::vector<std::string>& args) {
    if (args.empty()) return;
    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        // Double-fork so init/systemd reaps the grandchild and parent waits only briefly.
        pid_t gpid = ::fork();
        if (gpid == 0) {
            int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (devNull >= 0) {
                ::dup2(devNull, STDIN_FILENO);
                ::dup2(devNull, STDOUT_FILENO);
                ::dup2(devNull, STDERR_FILENO);
                if (devNull > STDERR_FILENO) ::close(devNull);
            }
            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            ::_exit(127);
        }
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

bool CreateNonBlockingReadPipe(int fds[2]) {
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) == 0) {
        int fl = ::fcntl(fds[0], F_GETFL, 0);
        if (fl >= 0) ::fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
        return true;
    }
#endif
    if (::pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        int fdFl = ::fcntl(fds[i], F_GETFD, 0);
        if (fdFl >= 0) ::fcntl(fds[i], F_SETFD, fdFl | FD_CLOEXEC);
    }
    int fl = ::fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) ::fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    return true;
}

} // namespace

// ============================================================================
// FileAssociations (XDG .desktop + xdg-mime on CachyOS / Arch Linux)
// ============================================================================

std::wstring FileAssociationExtensionKey(const std::wstring& dotExtension) {
    std::wstring out;
    size_t start = 0;
    while (start < dotExtension.size() && dotExtension[start] == L'.') ++start;
    for (size_t i = start; i < dotExtension.size(); ++i) {
        out.push_back(static_cast<wchar_t>(std::towlower(dotExtension[i])));
    }
    return out;
}

static fs::path GetDesktopEntryPath() {
    std::string dataDir = LinuxWideToUtf8(LinuxGetXdgDataDir());
    if (dataDir.empty()) dataDir = "/tmp";
    return fs::u8path(dataDir) / "applications" / "aperture.desktop";
}

FileAssociationResult RegisterFileAssociations() {
    FileAssociationResult res{};
    std::error_code ec;
    fs::path desktopPath = GetDesktopEntryPath();
    fs::create_directories(desktopPath.parent_path(), ec);

    std::string exePath = LinuxWideToUtf8(GetExePath());
    if (exePath.empty()) exePath = "/usr/bin/aperture";

    std::ofstream out(desktopPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        res.failed = 1;
        return res;
    }
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Aperture\n"
        << "GenericName=Media Player\n"
        << "Comment=Fast, modern media player for Linux\n"
        << "Exec=\"" << exePath << "\" %U\n"
        << "Icon=multimedia-video-player\n"
        << "Terminal=false\n"
        << "Categories=AudioVideo;Video;Audio;Player;\n"
        << "MimeType=video/mp4;video/x-matroska;video/webm;video/x-msvideo;video/quicktime;video/mp2t;audio/mpeg;audio/flac;audio/ogg;audio/opus;audio/mp4;audio/x-wav;\n"
        << "StartupWMClass=aperture\n";
    out.close();

    const auto& exts = MediaLibrary::GetSupportedExtensions();
    res.registered = static_cast<int>(exts.size());
    RunDetachedNoShell({"update-desktop-database", desktopPath.parent_path().u8string()});
    return res;
}

FileAssociationResult UnregisterFileAssociations() {
    FileAssociationResult res{};
    std::error_code ec;
    fs::path desktopPath = GetDesktopEntryPath();
    if (fs::exists(desktopPath, ec)) {
        fs::remove(desktopPath, ec);
        RunDetachedNoShell({"update-desktop-database", desktopPath.parent_path().u8string()});
    }
    const auto& exts = MediaLibrary::GetSupportedExtensions();
    res.registered = static_cast<int>(exts.size());
    return res;
}

bool GetFileAssociationState() {
    std::error_code ec;
    return fs::exists(GetDesktopEntryPath(), ec);
}

// ============================================================================
// JumpList (XDG recently-used.xbel on Linux)
// ============================================================================

void UpdateJumpList(const std::vector<std::wstring>& /*recentPaths*/) {
    // No-op or XDG recently-used.xbel integration; safe and non-blocking on Linux
}

// ============================================================================
// Diagnostics (Structured JSONL Logs, Startup Milestones, Crash Handler, /proc/self/status)
// ============================================================================

namespace Diagnostics {

namespace {
std::mutex g_diagMutex;
std::atomic<bool> g_loggingEnabled{false};
std::atomic<bool> g_watchdogInstalled{false};
std::atomic<int64_t> g_lastHeartbeatMs{0};
bool g_initialized = false;
char g_crashLogPath[PATH_MAX] = {};

struct MilestoneEntry {
    std::wstring name;
    double elapsedMs = 0.0;
    uint64_t rssBytes = 0;
};
std::vector<MilestoneEntry> g_milestones;
std::chrono::steady_clock::time_point g_startTime = std::chrono::steady_clock::now();

uint64_t ReadProcessRssBytes() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            uint64_t kb = 0;
            std::sscanf(line.c_str() + 6, "%llu", reinterpret_cast<unsigned long long*>(&kb));
            return kb * 1024ULL;
        }
    }
    return 0;
}

std::wstring GetLogsDir() { return ResolveAppPath(L"data\\diagnostics\\logs"); }
std::wstring GetCrashesDir() { return ResolveAppPath(L"data\\diagnostics\\crashes"); }
std::wstring GetPerfDir() { return ResolveAppPath(L"data\\diagnostics\\perf"); }
void EnsureDir(const std::wstring& dir) {
    std::error_code ec;
    fs::create_directories(fs::u8path(LinuxWinPathToPosix(dir)), ec);
}

const char* ErrorSeverityToString(ErrorSeverity sev) {
    switch (sev) {
        case ErrorSeverity::Information: return "Information";
        case ErrorSeverity::Warning: return "Warning";
        case ErrorSeverity::Error: return "Error";
        case ErrorSeverity::Fatal: return "Fatal";
    }
    return "Error";
}

std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20) out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

void WriteRawStr(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = 0;
    while (s[len]) ++len;
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, s + off, len - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

void WriteRawUInt(int fd, uint64_t v, int base) {
    char buf[32];
    int pos = 0;
    if (v == 0) {
        buf[pos++] = '0';
    } else {
        const char* digits = "0123456789abcdef";
        char rev[32];
        int r = 0;
        while (v > 0) {
            rev[r++] = digits[v % static_cast<uint64_t>(base)];
            v /= static_cast<uint64_t>(base);
        }
        while (r > 0) buf[pos++] = rev[--r];
    }
    buf[pos] = '\0';
    WriteRawStr(fd, buf);
}

void CrashSignalHandler(int sig, siginfo_t* info, void* /*ucontext*/) {
    if (g_crashLogPath[0] != '\0') {
        int fd = ::open(g_crashLogPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) {
            WriteRawStr(fd, "\n=== Aperture Crash Signal ===\nsignal=");
            WriteRawUInt(fd, static_cast<uint64_t>(sig), 10);
            WriteRawStr(fd, " pid=");
            WriteRawUInt(fd, static_cast<uint64_t>(::getpid()), 10);
            if (info) {
                WriteRawStr(fd, " addr=0x");
                WriteRawUInt(fd, reinterpret_cast<uintptr_t>(info->si_addr), 16);
                WriteRawStr(fd, " code=");
                WriteRawUInt(fd, static_cast<uint64_t>(static_cast<unsigned int>(info->si_code)), 10);
            }
            WriteRawStr(fd, "\n");
#if __has_include(<execinfo.h>)
            void* frames[64];
            int count = ::backtrace(frames, 64);
            if (count > 0) {
                ::backtrace_symbols_fd(frames, count, fd);
            }
#endif
            ::close(fd);
        }
    }
    ::raise(sig);
}
} // namespace

bool Initialize(bool loggingEnabled) {
    std::lock_guard<std::mutex> lock(g_diagMutex);
    g_loggingEnabled.store(loggingEnabled, std::memory_order_release);
    if (!g_initialized) {
        g_startTime = std::chrono::steady_clock::now();
        g_initialized = true;
    }
    if (loggingEnabled) {
        EnsureDir(GetLogsDir());
        EnsureDir(GetCrashesDir());
        EnsureDir(GetPerfDir());
    }
    return true;
}

void StartWatchdog() {
    RecordHeartbeat();
    std::lock_guard<std::mutex> lock(g_diagMutex);
    if (g_watchdogInstalled.load(std::memory_order_acquire)) return;
    std::wstring crashDir = GetCrashesDir();
    EnsureDir(crashDir);
    std::string crashPath = LinuxWinPathToPosix(crashDir + L"/crash.log");
    if (!crashPath.empty() && crashPath.size() < sizeof(g_crashLogPath)) {
        std::memcpy(g_crashLogPath, crashPath.c_str(), crashPath.size() + 1);
    }
#if __has_include(<execinfo.h>)
    // Warm up libgcc_s before a crash so backtrace() inside signal handler does not dlopen.
    void* warm[2];
    (void)::backtrace(warm, 2);
#endif
    struct sigaction sa{};
    sa.sa_sigaction = &CrashSignalHandler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    g_watchdogInstalled.store(true, std::memory_order_release);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_diagMutex);
    g_initialized = false;
}

void SetLoggingEnabled(bool enabled) {
    g_loggingEnabled.store(enabled, std::memory_order_release);
    if (enabled) {
        EnsureDir(GetLogsDir());
        EnsureDir(GetCrashesDir());
        EnsureDir(GetPerfDir());
    }
}

void Log(const AppError& error) {
    if (!g_loggingEnabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_diagMutex);
    EnsureDir(GetLogsDir());
    std::string logPath = LinuxWinPathToPosix(GetLogsDir() + L"/aperture.jsonl");
    std::ofstream out(logPath, std::ios::binary | std::ios::app);
    if (!out) return;
    const std::wstring& msg = !error.technicalMessage.empty() ? error.technicalMessage : error.userMessage;
    out << "{\"code\":" << static_cast<int>(error.code)
        << ",\"severity\":\"" << ErrorSeverityToString(error.severity) << "\""
        << ",\"domain\":\"" << EscapeJsonString(LinuxWideToUtf8(ErrorDomainName(error.domain))) << "\""
        << ",\"message\":\"" << EscapeJsonString(LinuxWideToUtf8(msg)) << "\"}\n";
}

void RecordHeartbeat() {
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    g_lastHeartbeatMs.store(nowMs, std::memory_order_release);
}

void RecordStartupMilestone(const std::wstring& name, bool captureResources) {
    std::lock_guard<std::mutex> lock(g_diagMutex);
    if (g_milestones.size() >= 128) return;
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - g_startTime).count();
    uint64_t rss = captureResources ? ReadProcessRssBytes() : 0;
    g_milestones.push_back({name, ms, rss});
}

void WriteStartupReport() {
    if (!g_loggingEnabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_diagMutex);
    EnsureDir(GetPerfDir());
    std::string perfPath = LinuxWinPathToPosix(GetPerfDir() + L"/startup.json");
    std::ofstream out(perfPath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "{\"milestones\":[";
    for (size_t i = 0; i < g_milestones.size(); ++i) {
        if (i > 0) out << ",";
        out << "{\"name\":\"" << EscapeJsonString(LinuxWideToUtf8(g_milestones[i].name))
            << "\",\"elapsedMs\":" << g_milestones[i].elapsedMs
            << ",\"rssBytes\":" << g_milestones[i].rssBytes << "}";
    }
    out << "]}\n";
}

} // namespace Diagnostics

// ============================================================================
// MediaFoundation (Reference-Counted Lazy Lifecycle)
// ============================================================================

std::mutex& MediaFoundation::Mutex() {
    static std::mutex m;
    return m;
}
int& MediaFoundation::RefCount() {
    static int c = 0;
    return c;
}
bool& MediaFoundation::InitializedFlag() {
    static bool init = false;
    return init;
}

bool MediaFoundation::Acquire() {
    std::lock_guard<std::mutex> lock(Mutex());
    ++RefCount();
    InitializedFlag() = true;
    return true;
}

void MediaFoundation::Release() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (RefCount() > 0) --RefCount();
}

bool MediaFoundation::IsInitialized() {
    std::lock_guard<std::mutex> lock(Mutex());
    return InitializedFlag();
}

void MediaFoundation::ShutdownIfInitialized() {
    std::lock_guard<std::mutex> lock(Mutex());
    RefCount() = 0;
    InitializedFlag() = false;
}

// ============================================================================
// MediaProbeIsolation (Linux implementation using fork+execvp ffprobe/ffmpeg)
// ============================================================================

namespace {
thread_local MediaProbeIsolation* g_threadProbeIsolation = nullptr;

enum class SubprocessStatus { Ok, NonZeroExit, ExecUnavailable, Canceled, TimedOut, OutputTooLarge };

SubprocessStatus RunSubprocessCapture(const std::vector<std::string>& args,
                                      DWORD timeoutMs,
                                      size_t maxOutputBytes,
                                      const MediaProbeIsolation::CancelCheck& cancelCheck,
                                      DWORD& outHelperPid,
                                      std::vector<uint8_t>& outBytes,
                                      int& outExitCode) {
    outBytes.clear();
    outExitCode = -1;
    outHelperPid = 0;
    if (args.empty()) return SubprocessStatus::ExecUnavailable;

    int pfds[2] = {-1, -1};
    if (!CreateNonBlockingReadPipe(pfds)) return SubprocessStatus::NonZeroExit;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pfds[0]);
        ::close(pfds[1]);
        return SubprocessStatus::NonZeroExit;
    }
    if (pid == 0) {
        ::close(pfds[0]);
        int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }
        ::dup2(pfds[1], STDOUT_FILENO);
        if (pfds[1] > STDERR_FILENO) ::close(pfds[1]);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        ::_exit(errno == ENOENT ? 127 : 126);
    }

    ::close(pfds[1]);
    outHelperPid = static_cast<DWORD>(pid);

    const bool hasDeadline = (timeoutMs != INFINITE);
    const auto deadline = hasDeadline
                              ? (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs))
                              : std::chrono::steady_clock::time_point::max();

    auto abortChild = [&](SubprocessStatus reason) {
        ::close(pfds[0]);
        ::kill(pid, SIGKILL);
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        outHelperPid = 0;
        return reason;
    };

    uint8_t chunk[16384];
    while (true) {
        if (cancelCheck && cancelCheck()) {
            return abortChild(SubprocessStatus::Canceled);
        }
        int sliceMs = -1;
        if (hasDeadline) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return abortChild(SubprocessStatus::TimedOut);
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            sliceMs = static_cast<int>(std::clamp<int64_t>(rem, 1, cancelCheck ? 50 : INT_MAX));
        } else if (cancelCheck) {
            sliceMs = 50;
        }

        struct pollfd pfd { pfds[0], POLLIN, 0 };
        int pr = ::poll(&pfd, 1, sliceMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        bool hitEof = false;
        while (true) {
            ssize_t n = ::read(pfds[0], chunk, sizeof(chunk));
            if (n > 0) {
                if (outBytes.size() + static_cast<size_t>(n) > maxOutputBytes) {
                    return abortChild(SubprocessStatus::OutputTooLarge);
                }
                outBytes.insert(outBytes.end(), chunk, chunk + n);
            } else if (n == 0) {
                hitEof = true;
                break;
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                hitEof = true;
                break;
            }
        }
        if (hitEof) break;
    }

    ::close(pfds[0]);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) break;
    }
    outHelperPid = 0;

    if (WIFEXITED(st)) {
        outExitCode = WEXITSTATUS(st);
        if (outExitCode == 0) return SubprocessStatus::Ok;
        if (outExitCode == 127) return SubprocessStatus::ExecUnavailable;
    }
    return SubprocessStatus::NonZeroExit;
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t ReadLeI32(const uint8_t* p) {
    return static_cast<int32_t>(ReadLe32(p));
}

bool DecodeBmp32Bgra(const std::vector<uint8_t>& bmp, std::vector<uint8_t>& outPixels, uint32_t& outW, uint32_t& outH) {
    if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    uint32_t dataOffset = ReadLe32(bmp.data() + 10);
    uint32_t dibSize = ReadLe32(bmp.data() + 14);
    if (dibSize < 40 || dataOffset < 14 + dibSize || dataOffset > bmp.size()) return false;
    int32_t rawW = ReadLeI32(bmp.data() + 18);
    int32_t rawH = ReadLeI32(bmp.data() + 22);
    uint16_t bpp = ReadLe16(bmp.data() + 28);
    uint32_t compression = ReadLe32(bmp.data() + 30);
    if (rawW <= 0 || rawH == 0 || bpp != 32 || (compression != 0 && compression != 3)) return false;

    bool bottomUp = (rawH > 0);
    uint32_t w = static_cast<uint32_t>(rawW);
    uint32_t h = static_cast<uint32_t>(bottomUp ? rawH : -rawH);
    if (w > 4096 || h > 4096) return false;

    size_t rowBytes = static_cast<size_t>(w) * 4;
    size_t pixelBytes = rowBytes * h;
    if (bmp.size() - dataOffset < pixelBytes) return false;

    outPixels.resize(pixelBytes);
    const uint8_t* src = bmp.data() + dataOffset;
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t srcY = bottomUp ? (h - 1 - y) : y;
        std::memcpy(outPixels.data() + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(srcY) * rowBytes,
                    rowBytes);
    }
    outW = w;
    outH = h;
    return true;
}

} // namespace

struct MediaProbeIsolation::Impl {
    CancelCheck canceled;
    std::wstring executable;
    Outcome lastOutcome = Outcome::Unavailable;
    DWORD helperPid = 0;
};

MediaProbeIsolation::MediaProbeIsolation(CancelCheck canceled, std::wstring executable)
    : impl(std::make_unique<Impl>()) {
    impl->canceled = std::move(canceled);
    impl->executable = std::move(executable);
}

MediaProbeIsolation::~MediaProbeIsolation() = default;

bool MediaProbeIsolation::ProbeMetadata(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height, DWORD timeoutMs) {
    duration = 0.0;
    width = 0;
    height = 0;
    if (impl->canceled && impl->canceled()) {
        impl->lastOutcome = Outcome::Canceled;
        return false;
    }
    if (path.empty() || !FileExists(path)) {
        impl->lastOutcome = Outcome::Failed;
        return false;
    }

    std::string u8Path = LinuxWinPathToPosix(path);
    std::vector<uint8_t> output;
    int exitCode = -1;
    SubprocessStatus st = RunSubprocessCapture(
        {"ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1", u8Path},
        timeoutMs, 64 * 1024, impl->canceled, impl->helperPid, output, exitCode);

    if (st == SubprocessStatus::Canceled) { impl->lastOutcome = Outcome::Canceled; return false; }
    if (st == SubprocessStatus::TimedOut) { impl->lastOutcome = Outcome::TimedOut; return false; }
    if (st == SubprocessStatus::ExecUnavailable) { impl->lastOutcome = Outcome::Unavailable; return false; }

    if (st == SubprocessStatus::Ok && !output.empty()) {
        std::string text(reinterpret_cast<const char*>(output.data()), output.size());
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("width=", 0) == 0) {
                width = static_cast<uint32_t>(std::strtoul(line.c_str() + 6, nullptr, 10));
            } else if (line.rfind("height=", 0) == 0) {
                height = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
            } else if (line.rfind("duration=", 0) == 0) {
                double d = std::strtod(line.c_str() + 9, nullptr);
                if (std::isfinite(d) && d > 0.0) duration = d;
            }
        }
        if (duration > 0.0 || width > 0) {
            impl->lastOutcome = Outcome::Success;
            return true;
        }
    }

    impl->lastOutcome = Outcome::Failed;
    return false;
}

bool MediaProbeIsolation::GetThumbnail(const std::wstring& path, uint32_t requestedWidth, uint32_t requestedHeight,
                                       std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, DWORD timeoutMs) {
    pixels.clear();
    width = 0;
    height = 0;
    if (impl->canceled && impl->canceled()) {
        impl->lastOutcome = Outcome::Canceled;
        return false;
    }
    if (path.empty() || !FileExists(path)) {
        impl->lastOutcome = Outcome::Failed;
        return false;
    }

    const uint32_t maxW = requestedWidth ? requestedWidth : 320;
    const uint32_t maxH = requestedHeight ? requestedHeight : 180;

    // Try WIC image decoder first (covers PNG/BMP/JPEG cover art files) with aspect preservation
    IWICImagingFactory wic;
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(wic.CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
            UINT srcW = 0, srcH = 0;
            UINT w = maxW, h = maxH;
            if (SUCCEEDED(frame->GetSize(&srcW, &srcH)) && srcW > 0 && srcH > 0) {
                double scale = std::min(static_cast<double>(maxW) / srcW, static_cast<double>(maxH) / srcH);
                if (scale > 1.0) scale = 1.0;
                w = std::max<UINT>(1U, static_cast<UINT>(std::lround(srcW * scale)));
                h = std::max<UINT>(1U, static_cast<UINT>(std::lround(srcH * scale)));
            }
            IWICBitmapScaler scaler;
            if (SUCCEEDED(scaler.Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic))) {
                width = scaler.m_width;
                height = scaler.m_height;
                pixels = std::move(scaler.m_pixels);
                impl->lastOutcome = Outcome::Success;
                return true;
            }
        }
    }

    // Try ffmpeg aspect-preserving BGRA BMP extraction for video files
    std::string u8Path = LinuxWinPathToPosix(path);
    std::string vf = "scale=" + std::to_string(maxW) + ":" + std::to_string(maxH) + ":force_original_aspect_ratio=decrease";
    const size_t maxBmpBytes = static_cast<size_t>(maxW) * maxH * 4 + 4096;

    for (const char* seekSec : {"1", "0"}) {
        std::vector<uint8_t> bmpBytes;
        int exitCode = -1;
        SubprocessStatus st = RunSubprocessCapture(
            {"ffmpeg", "-loglevel", "error", "-ss", seekSec, "-i", u8Path,
             "-frames:v", "1", "-vf", vf, "-vcodec", "bmp", "-pix_fmt", "bgra",
             "-f", "image2pipe", "-"},
            timeoutMs, maxBmpBytes, impl->canceled, impl->helperPid, bmpBytes, exitCode);

        if (st == SubprocessStatus::Canceled) { impl->lastOutcome = Outcome::Canceled; return false; }
        if (st == SubprocessStatus::TimedOut) { impl->lastOutcome = Outcome::TimedOut; return false; }
        if (st == SubprocessStatus::ExecUnavailable) { impl->lastOutcome = Outcome::Unavailable; return false; }
        if (st == SubprocessStatus::Ok && DecodeBmp32Bgra(bmpBytes, pixels, width, height)) {
            impl->lastOutcome = Outcome::Success;
            return true;
        }
    }

    impl->lastOutcome = Outcome::Failed;
    return false;
}

MediaProbeIsolation::Outcome MediaProbeIsolation::LastOutcome() const { return impl->lastOutcome; }
DWORD MediaProbeIsolation::HelperProcessId() const { return impl->helperPid; }
void MediaProbeIsolation::ReleaseIdleResources() { impl->helperPid = 0; }

ScopedMediaProbeIsolation::ScopedMediaProbeIsolation(CancelCheck canceled)
    : MediaProbeIsolation(std::move(canceled)), previous(g_threadProbeIsolation) {
    g_threadProbeIsolation = this;
}

ScopedMediaProbeIsolation::~ScopedMediaProbeIsolation() {
    g_threadProbeIsolation = previous;
}

bool TryIsolatedMetadataProbe(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height, bool& handled) {
    if (!g_threadProbeIsolation) {
        handled = false;
        return false;
    }
    handled = true;
    return g_threadProbeIsolation->ProbeMetadata(path, duration, width, height);
}

bool IsMediaProbeHelperCommand(int argc, wchar_t** argv) {
    return argc >= 2 && argv && argv[1] && std::wcscmp(argv[1], L"--media-probe-helper") == 0;
}

int RunMediaProbeHelper(int /*argc*/, wchar_t** /*argv*/) {
    return 0;
}

} // namespace Aperture

#endif // !defined(_WIN32)
