#if !defined(_WIN32)

#include "../Common.h"
#include "../FileAssociations.h"
#include "../JumpList.h"
#include "../Diagnostics.h"
#include "../MediaFoundation.h"
#include "../MediaProbeIsolation.h"
#include "../MediaLibrary.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace Aperture {

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
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    fs::path base;
    if (xdgData && *xdgData) {
        base = fs::u8path(xdgData);
    } else {
        const char* home = std::getenv("HOME");
        base = fs::u8path(home && *home ? home : "/tmp") / ".local" / "share";
    }
    return base / "applications" / "aperture.desktop";
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
        << "Icon=aperture\n"
        << "Terminal=false\n"
        << "Categories=AudioVideo;Video;Audio;Player;\n"
        << "MimeType=video/mp4;video/x-matroska;video/webm;video/x-msvideo;video/quicktime;video/mp2t;audio/mpeg;audio/flac;audio/ogg;audio/opus;audio/mp4;audio/x-wav;\n";
    out.close();

    const auto& exts = MediaLibrary::GetSupportedExtensions();
    res.registered = static_cast<int>(exts.size());
    (void)std::system("update-desktop-database ~/.local/share/applications >/dev/null 2>&1");
    return res;
}

FileAssociationResult UnregisterFileAssociations() {
    FileAssociationResult res{};
    std::error_code ec;
    fs::path desktopPath = GetDesktopEntryPath();
    if (fs::exists(desktopPath, ec)) {
        fs::remove(desktopPath, ec);
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
// Diagnostics (Structured JSONL Logs, Startup Milestones, /proc/self/status)
// ============================================================================

namespace Diagnostics {

namespace {
std::mutex g_diagMutex;
std::atomic<bool> g_loggingEnabled{false};
bool g_initialized = false;

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

void StartWatchdog() {}

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

void RecordHeartbeat() {}

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
// MediaProbeIsolation (Linux implementation using ffprobe/ffmpeg + safe fallback)
// ============================================================================

namespace {
thread_local MediaProbeIsolation* g_threadProbeIsolation = nullptr;

std::string ShellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
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

bool MediaProbeIsolation::ProbeMetadata(const std::wstring& path, double& duration, uint32_t& width, uint32_t& height, DWORD /*timeoutMs*/) {
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
    std::string cmd = "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -show_entries format=duration -of default=noprint_wrappers=1 " + ShellQuote(u8Path) + " 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) {
        char line[256];
        while (std::fgets(line, sizeof(line), pipe)) {
            if (std::strncmp(line, "width=", 6) == 0) {
                width = static_cast<uint32_t>(std::strtoul(line + 6, nullptr, 10));
            } else if (std::strncmp(line, "height=", 7) == 0) {
                height = static_cast<uint32_t>(std::strtoul(line + 7, nullptr, 10));
            } else if (std::strncmp(line, "duration=", 9) == 0) {
                double d = std::strtod(line + 9, nullptr);
                if (std::isfinite(d) && d > 0.0) duration = d;
            }
        }
        int status = ::pclose(pipe);
        if (status == 0 && (duration > 0.0 || width > 0)) {
            impl->lastOutcome = Outcome::Success;
            return true;
        }
    }

    impl->lastOutcome = Outcome::Failed;
    return false;
}

bool MediaProbeIsolation::GetThumbnail(const std::wstring& path, uint32_t requestedWidth, uint32_t requestedHeight,
                                       std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height, DWORD /*timeoutMs*/) {
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

    // Try WIC image decoder first (covers PNG/BMP/JPEG cover art files)
    IWICImagingFactory wic;
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(wic.CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
            UINT w = requestedWidth ? requestedWidth : 320;
            UINT h = requestedHeight ? requestedHeight : 180;
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

    // Try ffmpeg raw BGRA frame extraction for video files on Linux
    uint32_t targetW = requestedWidth ? requestedWidth : 320;
    uint32_t targetH = requestedHeight ? requestedHeight : 180;
    std::string u8Path = LinuxWinPathToPosix(path);
    std::ostringstream cmd;
    cmd << "ffmpeg -loglevel error -ss 1 -i " << ShellQuote(u8Path)
        << " -frames:v 1 -vf scale=" << targetW << ":" << targetH
        << " -f rawvideo -pix_fmt bgra - 2>/dev/null";
    FILE* pipe = ::popen(cmd.str().c_str(), "r");
    if (pipe) {
        size_t expectedBytes = static_cast<size_t>(targetW) * targetH * 4;
        std::vector<uint8_t> buf(expectedBytes);
        size_t totalRead = 0;
        while (totalRead < expectedBytes) {
            size_t n = std::fread(buf.data() + totalRead, 1, expectedBytes - totalRead, pipe);
            if (n == 0) break;
            totalRead += n;
        }
        int status = ::pclose(pipe);
        if (status == 0 && totalRead == expectedBytes) {
            width = targetW;
            height = targetH;
            pixels = std::move(buf);
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
