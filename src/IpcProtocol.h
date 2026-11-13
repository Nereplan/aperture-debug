#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Aperture {

struct PlaybackCommandLineOptions {
    std::vector<std::wstring> mediaTargets;
    std::optional<double> startPositionSec; // In seconds (from --start=<seconds|hh:mm:ss>)
    bool fullscreen = false;                // From --fullscreen
    bool paused = false;                    // From --paused
    bool loop = false;                      // From --loop
    std::optional<double> volume;           // In range [0..100] (from --volume=<0..100>)
    std::wstring subtitlePath;              // From --subtitle=<path>
    bool enqueue = false;                   // From --enqueue
    bool benchmarkProbe = false;            // From --benchmark-probe or --benchmark
    std::wstring benchmarkOutputFile;       // Optional output JSON path from --benchmark-probe=<path> or --benchmark=<path>
    bool visualCaptureProbe = false;        // From --visual-capture-probe, --visual-probe, or --visual-capture
    std::wstring visualCaptureScene;        // From --visual-capture-scene=<scene> or --scene=<scene>
    std::wstring visualCaptureOutputFile;   // Output PNG path from --visual-capture-output=<path> or --capture-output=<path>
    bool HasOptions() const {
        return startPositionSec.has_value() || fullscreen || paused || loop || volume.has_value() || !subtitlePath.empty() || enqueue ||
               benchmarkProbe || visualCaptureProbe || !visualCaptureScene.empty() || !visualCaptureOutputFile.empty();
    }
};

enum class CommandLineParseResult { Success = 0, Empty, UnknownFlag, MalformedFlag, ArgumentTooLong, TooManyArguments };

struct CommandLineParseOutput {
    CommandLineParseResult result = CommandLineParseResult::Success;
    std::wstring errorMessage;
    PlaybackCommandLineOptions options;
    bool IsSuccess() const { return result == CommandLineParseResult::Success; }
};

namespace Ipc {
using ::Aperture::PlaybackCommandLineOptions;
using ::Aperture::CommandLineParseResult;
using ::Aperture::CommandLineParseOutput;

namespace OptionFlags {
    constexpr uint32_t HasStartPos   = 1u << 0;
    constexpr uint32_t Fullscreen    = 1u << 1;
    constexpr uint32_t Paused        = 1u << 2;
    constexpr uint32_t Loop          = 1u << 3;
    constexpr uint32_t HasVolume     = 1u << 4;
    constexpr uint32_t HasSubtitle   = 1u << 5;
    constexpr uint32_t Enqueue       = 1u << 6;
}

// Magic constants
constexpr uint32_t IPC_MAGIC_V2 = 0x41505456; // 'APTV' (Aperture Versioned)
constexpr uint32_t IPC_LEGACY_MAGIC = 0x41505452; // 'APTR'
constexpr uint32_t IPC_CURRENT_VERSION = 2;
// Bounded safety limits
constexpr size_t MAX_IPC_PAYLOAD_SIZE = 2 * 1024 * 1024; // 2 MB max total envelope
constexpr size_t MAX_IPC_FILE_COUNT = 1024;              // Max 1024 files per forward
constexpr size_t MAX_IPC_PATH_CHARS = 32768;             // Win32 long path limit (32K wchar_t)

enum class Command : uint32_t { OpenFiles = 1, Activate = 2, Automation = 3, AutomationResponse = 4 };

enum class AutomationOp : uint32_t {
    None = 0, Play = 1, Pause = 2, Stop = 3, Next = 4, Previous = 5, Seek = 6, Volume = 7, Open = 8, Enqueue = 9, QueryStatus = 10
};

namespace AutomationFlags {
    constexpr uint32_t SeekRelative = 1u << 0;
    constexpr uint32_t Paused       = 1u << 1;
    constexpr uint32_t Fullscreen   = 1u << 2;
}

namespace StatusFlags {
    constexpr uint32_t Muted        = 1u << 0;
    constexpr uint32_t Loop         = 1u << 1;
    constexpr uint32_t Fullscreen   = 1u << 2;
}

enum class AutomationPlaybackState : uint32_t { Closed = 0, Initializing = 1, Playing = 2, Paused = 3, Error = 4 };

struct AutomationCommand {
    AutomationOp op = AutomationOp::None;
    double seekPositionSec = 0.0;
    bool seekRelative = false;
    double volume = 0.0; // [0.0 .. 100.0]
    double startPositionSec = 0.0;
    bool paused = false, fullscreen = false;
    std::vector<std::wstring> mediaTargets;
};

struct AutomationStatus {
    AutomationPlaybackState state = AutomationPlaybackState::Closed;
    double positionSec = 0.0, durationSec = 0.0;
    double volume = 100.0; // [0.0 .. 100.0]
    bool isMuted = false, isLoop = false, isFullscreen = false;
    std::wstring currentFilePath;
};

enum class ValidationResult {
    Success = 0, NullData, PayloadTooSmall, PayloadOversized, InvalidMagic, UnsupportedVersion, InvalidHeaderSize, TotalSizeMismatch,
    FileCountExceeded, PathTooLong, ChecksumMismatch, SenderUnauthorized, MalformedPayload, InvalidOperation, InvalidArgument,
    ReplayDetected
};

#pragma pack(push, 1)
struct EnvelopeHeader {
    uint32_t magic;         // IPC_MAGIC_V2 ('APTV')
    uint32_t version;       // IPC_CURRENT_VERSION (2)
    uint32_t headerSize;    // sizeof(EnvelopeHeader)
    uint32_t totalSize;     // Total message size in bytes (header + payload)
    uint32_t senderPid;     // GetCurrentProcessId()
    uint32_t command;       // Command::OpenFiles / Command::Activate / Automation / AutomationResponse
    uint32_t fileCount;     // Number of path records following header
    uint32_t flags;         // Bit 0: Activate window
    uint64_t timestamp;     // QPC or UTC epoch ms
    uint32_t checksum;      // Fletcher-32 checksum of payload
};
#pragma pack(pop)

struct ReplaySignature { uint32_t senderPid = 0; uint64_t timestamp = 0; uint32_t checksum = 0; };

class ReplayFilter {
public:
    static constexpr size_t kHistorySize = 64;
    static constexpr uint64_t kMaxAgeMs = 30000, kMaxFutureClockSkewMs = 5000;
    static bool ValidateAndRecord(const EnvelopeHeader& header, uint64_t nowMs = 0);
    static void Reset();

private:
    inline static ReplaySignature history[kHistorySize] = {};
    inline static size_t head = 0;
    inline static size_t count = 0;
};

uint32_t ComputeChecksum(const uint8_t* data, size_t len);
bool IsSameUserAndSession(DWORD senderPid);
std::wstring ResolveFullPathLong(const std::wstring& path);
bool IsValidMediaUrl(const std::wstring& str);
bool ParseTimeValue(const std::wstring& str, double& outSeconds, std::wstring& outError);
bool ParseVolumeValue(const std::wstring& str, double& outVolume, std::wstring& outError);
bool ParseBooleanFlag(const std::wstring& str, bool& outVal, std::wstring& outError);
CommandLineParseOutput ParseCommandLineArgs(const std::vector<std::wstring>& args);
CommandLineParseOutput ParseCommandLine(const std::wstring& cmdLine);

std::vector<uint8_t> SerializeEnvelope(const PlaybackCommandLineOptions& options, Command cmd = Command::OpenFiles, uint32_t flags = 1);
std::vector<uint8_t> SerializeEnvelope(const std::vector<std::wstring>& paths, Command cmd = Command::OpenFiles, uint32_t flags = 1);
ValidationResult DeserializeEnvelope(const void* data, size_t size, PlaybackCommandLineOptions& outOptions,
    EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true);
ValidationResult DeserializeEnvelope(const void* data, size_t size, std::vector<std::wstring>& outPaths,
    EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true);
std::wstring PathsToAppPayload(const std::vector<std::wstring>& paths);

std::vector<uint8_t> SerializeAutomationEnvelope(const AutomationCommand& cmd, uint32_t flags = 0);
ValidationResult DeserializeAutomationEnvelope(const void* data, size_t size, AutomationCommand& outCmd,
    EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true, bool checkReplay = false, uint64_t nowMs = 0);
std::vector<uint8_t> SerializeAutomationResponse(const AutomationStatus& status, uint32_t statusResult = 0);
ValidationResult DeserializeAutomationResponse(const void* data, size_t size, AutomationStatus& outStatus,
    uint32_t* outStatusResult = nullptr, EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true);
bool SendAutomationCommand(HWND hTargetWnd, const AutomationCommand& cmd, AutomationStatus* outStatus = nullptr, DWORD timeoutMs = 3000);

} // namespace Ipc
} // namespace Aperture

#if !defined(APERTURE_UPDATER_MANIFEST_URL) && !defined(APERTURE_IPC_CPP_UNIT) && defined(_WIN32)
// Standalone Windows test harnesses compile single/direct TU lists without IpcProtocol.cpp.
#define APERTURE_IPC_INLINE inline
#include "IpcProtocol.cpp"
#undef APERTURE_IPC_INLINE
#endif
