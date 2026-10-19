#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <optional>
#include <cmath>
#include <sstream>

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
    uint32_t version;       // IPC_CURRENT_VERSION (1)
    uint32_t headerSize;    // sizeof(EnvelopeHeader)
    uint32_t totalSize;     // Total message size in bytes (header + payload)
    uint32_t senderPid;     // GetCurrentProcessId()
    uint32_t command;       // Command::OpenFiles / Command::Activate
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
    static bool ValidateAndRecord(const EnvelopeHeader& header, uint64_t nowMs = 0) {
        if (nowMs == 0) { nowMs = GetTickCount64(); }
        if (header.timestamp != 0) {
            if (header.timestamp > nowMs + kMaxFutureClockSkewMs) { return false; }
            if (nowMs > header.timestamp && (nowMs - header.timestamp) > kMaxAgeMs) { return false; }
        }
        for (size_t i = 0; i < count; ++i) {
            if (history[i].senderPid == header.senderPid && history[i].timestamp == header.timestamp &&
                history[i].checksum == header.checksum) {
                return false;
            }
        }
        history[head] = { header.senderPid, header.timestamp, header.checksum };
        head = (head + 1) % kHistorySize;
        if (count < kHistorySize) { count++; }
        return true;
    }
    static void Reset() {
        head = 0;
        count = 0;
        for (size_t i = 0; i < kHistorySize; ++i) { history[i] = {}; }
    }

private:
    inline static ReplaySignature history[kHistorySize] = {};
    inline static size_t head = 0;
    inline static size_t count = 0;
};

// Compute Fletcher-32 checksum of payload bytes
inline uint32_t ComputeChecksum(const uint8_t* data, size_t len) {
    uint32_t sum1 = 0xffff, sum2 = 0xffff;
    while (len) {
        size_t tlen = len > 359 ? 359 : len;
        len -= tlen;
        do {
            sum1 += *data++;
            sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    }
    sum1 = (sum1 & 0xffff) + (sum1 >> 16);
    sum2 = (sum2 & 0xffff) + (sum2 >> 16);
    return (sum2 << 16) | sum1;
}

// Check if a process ID belongs to the same user and session
inline bool IsSameUserAndSession(DWORD senderPid) {
    if (senderPid == 0) return false;
    if (senderPid == GetCurrentProcessId()) return true;
    HANDLE hSenderProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, senderPid);
    if (!hSenderProc) { return false; }
    HANDLE hSenderToken = NULL;
    if (!OpenProcessToken(hSenderProc, TOKEN_QUERY, &hSenderToken)) { CloseHandle(hSenderProc); return false; }
    HANDLE hMyToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hMyToken)) {
        CloseHandle(hSenderToken);
        CloseHandle(hSenderProc);
        return false;
    }
    bool authenticated = false;
    // 1. Validate Session ID matches
    DWORD senderSession = 0, mySession = 0;
    DWORD retLen = 0;
    if (GetTokenInformation(hSenderToken, TokenSessionId, &senderSession, sizeof(senderSession), &retLen) &&
        GetTokenInformation(hMyToken, TokenSessionId, &mySession, sizeof(mySession), &retLen)) {
        if (senderSession == mySession) {
            // 2. Validate User SID matches
            BYTE senderUserBuf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = { 0 };
            BYTE myUserBuf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = { 0 };
            DWORD senderUserLen = 0, myUserLen = 0;
            if (GetTokenInformation(hSenderToken, TokenUser, senderUserBuf, sizeof(senderUserBuf), &senderUserLen) &&
                GetTokenInformation(hMyToken, TokenUser, myUserBuf, sizeof(myUserBuf), &myUserLen)) {
                PTOKEN_USER pSenderUser = reinterpret_cast<PTOKEN_USER>(senderUserBuf);
                PTOKEN_USER pMyUser = reinterpret_cast<PTOKEN_USER>(myUserBuf);
                if (IsValidSid(pSenderUser->User.Sid) && IsValidSid(pMyUser->User.Sid) &&
                    EqualSid(pSenderUser->User.Sid, pMyUser->User.Sid)) {
                    authenticated = true;
                }
            }
        }
    }
    CloseHandle(hMyToken);
    CloseHandle(hSenderToken);
    CloseHandle(hSenderProc);
    return authenticated;
}

// Long-path-safe path resolution without MAX_PATH limitations
inline std::wstring ResolveFullPathLong(const std::wstring& path) {
    if (path.empty()) return {};
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring fullPath(required, L'\0');
    const DWORD written = GetFullPathNameW( path.c_str(), required, fullPath.data(), nullptr);
    if (written == 0 || written >= required) return path;
    fullPath.resize(written);
    return fullPath;
}
inline bool IsValidMediaUrl(const std::wstring& str) {
    if (str.size() < 10 || str.size() > 2048) return false;
    for (wchar_t c : str) { if (c < 32 || c == 127) return false; }
    std::wstring lower;
    lower.reserve(str.size());
    for (wchar_t c : str) lower.push_back(static_cast<wchar_t>(towlower(c)));
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 || lower.rfind(L"rtsp://", 0) == 0 ||
        lower.rfind(L"rtmp://", 0) == 0) {
        size_t schemeEnd = lower.find(L"://");
        if (schemeEnd == std::wstring::npos) return false;
        size_t hostStart = schemeEnd + 3;
        size_t hostEnd = lower.find_first_of(L":/?#", hostStart);
        if (hostEnd == std::wstring::npos) hostEnd = lower.size();
        if (hostEnd <= hostStart) return false;
        return true;
    }
    return false;
}

inline bool ParseTimeValue(const std::wstring& str, double& outSeconds, std::wstring& outError) {
    if (str.empty()) { outError = L"Time value cannot be empty."; return false; }
    if (str[0] == L'-' || str[0] == L'+') { outError = L"Time value cannot have leading signs: '" + str + L"'."; return false; }
    size_t colonCount = 0;
    for (wchar_t c : str) { if (c == L':') colonCount++; }
    if (colonCount == 0) {
        size_t dotCount = 0;
        for (wchar_t c : str) {
            if (c == L'.') {
                dotCount++;
                if (dotCount > 1) { outError = L"Multiple decimal points in time value: '" + str + L"'."; return false; }
            } else if (c < L'0' || c > L'9') { outError = L"Invalid characters in time value: '" + str + L"'."; return false; }
        }
        wchar_t* endptr = nullptr;
        double val = wcstod(str.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(val) || val < 0.0) {
            outError = L"Invalid numeric time value: '" + str + L"'.";
            return false;
        }
        if (val > 8640000.0) { outError = L"Time value exceeds maximum limit (8640000 seconds)."; return false; }
        outSeconds = val;
        return true;
    } else if (colonCount == 1) {
        size_t colonPos = str.find(L':');
        std::wstring mmStr = str.substr(0, colonPos);
        std::wstring ssStr = str.substr(colonPos + 1);
        if (mmStr.empty() || ssStr.empty()) { outError = L"Missing minutes or seconds in '" + str + L"'."; return false; }
        for (wchar_t c : mmStr) {
            if (c < L'0' || c > L'9') { outError = L"Invalid characters in minutes: '" + mmStr + L"'."; return false; }
        }
        size_t dotCount = 0;
        for (wchar_t c : ssStr) {
            if (c == L'.') {
                dotCount++;
                if (dotCount > 1) { outError = L"Multiple decimal points in seconds: '" + ssStr + L"'."; return false; }
            } else if (c < L'0' || c > L'9') { outError = L"Invalid characters in seconds: '" + ssStr + L"'."; return false; }
        }
        wchar_t* endptr = nullptr;
        double mm = wcstod(mmStr.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(mm) || mm < 0.0) {
            outError = L"Invalid minutes: '" + mmStr + L"'.";
            return false;
        }
        double ss = wcstod(ssStr.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(ss) || ss < 0.0 || ss >= 60.0) {
            outError = L"Seconds must be in [0, 60): '" + ssStr + L"'.";
            return false;
        }
        double total = mm * 60.0 + ss;
        if (!std::isfinite(total) || total > 8640000.0) { outError = L"Time value exceeds maximum limit."; return false; }
        outSeconds = total;
        return true;
    } else if (colonCount == 2) {
        size_t colon1 = str.find(L':');
        size_t colon2 = str.find(L':', colon1 + 1);
        std::wstring hhStr = str.substr(0, colon1);
        std::wstring mmStr = str.substr(colon1 + 1, colon2 - colon1 - 1);
        std::wstring ssStr = str.substr(colon2 + 1);
        if (hhStr.empty() || mmStr.empty() || ssStr.empty()) {
            outError = L"Missing hours, minutes, or seconds in '" + str + L"'.";
            return false;
        }
        for (wchar_t c : hhStr) {
            if (c < L'0' || c > L'9') { outError = L"Invalid characters in hours: '" + hhStr + L"'."; return false; }
        }
        for (wchar_t c : mmStr) {
            if (c < L'0' || c > L'9') { outError = L"Invalid characters in minutes: '" + mmStr + L"'."; return false; }
        }
        size_t dotCount = 0;
        for (wchar_t c : ssStr) {
            if (c == L'.') {
                dotCount++;
                if (dotCount > 1) { outError = L"Multiple decimal points in seconds: '" + ssStr + L"'."; return false; }
            } else if (c < L'0' || c > L'9') { outError = L"Invalid characters in seconds: '" + ssStr + L"'."; return false; }
        }
        wchar_t* endptr = nullptr;
        double hh = wcstod(hhStr.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(hh) || hh < 0.0) { outError = L"Invalid hours: '" + hhStr + L"'."; return false; }
        double mm = wcstod(mmStr.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(mm) || mm < 0.0 || mm >= 60.0) {
            outError = L"Minutes must be in [0, 60): '" + mmStr + L"'.";
            return false;
        }
        double ss = wcstod(ssStr.c_str(), &endptr);
        if (!endptr || *endptr != L'\0' || !std::isfinite(ss) || ss < 0.0 || ss >= 60.0) {
            outError = L"Seconds must be in [0, 60): '" + ssStr + L"'.";
            return false;
        }
        double total = hh * 3600.0 + mm * 60.0 + ss;
        if (!std::isfinite(total) || total > 8640000.0) { outError = L"Time value exceeds maximum limit."; return false; }
        outSeconds = total;
        return true;
    } else { outError = L"Too many colons in time value: '" + str + L"'."; return false; }
}

inline bool ParseVolumeValue(const std::wstring& str, double& outVolume, std::wstring& outError) {
    if (str.empty()) { outError = L"Volume value cannot be empty."; return false; }
    if (str[0] == L'-' || str[0] == L'+') { outError = L"Volume value cannot have leading signs: '" + str + L"'."; return false; }
    size_t dotCount = 0;
    for (wchar_t c : str) {
        if (c == L'.') {
            dotCount++;
            if (dotCount > 1) { outError = L"Multiple decimal points in volume: '" + str + L"'."; return false; }
        } else if (c < L'0' || c > L'9') {
            outError = L"Invalid characters in volume: '" + str + L"'. Expected number in [0, 100].";
            return false;
        }
    }
    wchar_t* endptr = nullptr;
    double val = wcstod(str.c_str(), &endptr);
    if (!endptr || *endptr != L'\0' || !std::isfinite(val) || val < 0.0 || val > 100.0) {
        outError = L"Volume value must be a number between 0 and 100: '" + str + L"'.";
        return false;
    }
    outVolume = val;
    return true;
}

inline bool ParseBooleanFlag(const std::wstring& str, bool& outVal, std::wstring& outError) {
    if (str.empty()) { outVal = true; return true; }
    std::wstring lower;
    lower.reserve(str.size());
    for (wchar_t c : str) lower.push_back(static_cast<wchar_t>(towlower(c)));
    if (lower == L"true" || lower == L"1" || lower == L"yes" || lower == L"on") { outVal = true; return true; }
    if (lower == L"false" || lower == L"0" || lower == L"no" || lower == L"off") { outVal = false; return true; }
    outError = L"Invalid boolean value: '" + str + L"'. Expected true or false.";
    return false;
}

inline CommandLineParseOutput ParseCommandLineArgs(const std::vector<std::wstring>& args) {
    CommandLineParseOutput output;
    if (args.size() > MAX_IPC_FILE_COUNT) {
        output.result = CommandLineParseResult::TooManyArguments;
        output.errorMessage = L"Argument count exceeds limit (" + std::to_wstring(MAX_IPC_FILE_COUNT) + L").";
        return output;
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& arg = args[i];
        if (arg.empty()) continue;
        if (arg.size() > MAX_IPC_PATH_CHARS) {
            output.result = CommandLineParseResult::ArgumentTooLong;
            output.errorMessage = L"Argument exceeds maximum length limit (" + std::to_wstring(MAX_IPC_PATH_CHARS) + L").";
            return output;
        }
        bool isFlag = false;
        std::wstring flagContent;
        if (arg.rfind(L"--", 0) == 0) { isFlag = true; flagContent = arg.substr(2); } else if (arg.rfind(L"-", 0) == 0 && arg.size() > 1) {
            std::wstring testContent = arg.substr(1);
            size_t eq = testContent.find(L'=');
            std::wstring testName = (eq == std::wstring::npos) ? testContent : testContent.substr(0, eq);
            for (auto& c : testName) c = static_cast<wchar_t>(towlower(c));
            if (testName == L"start" || testName == L"fullscreen" || testName == L"paused" || testName == L"loop" ||
                testName == L"volume" || testName == L"subtitle" || testName == L"enqueue" || testName == L"help" || testName == L"h" ||
                testName == L"?" || testName == L"benchmark-probe" || testName == L"benchmark") {
                isFlag = true;
                flagContent = testContent;
            } else {
                output.result = CommandLineParseResult::UnknownFlag;
                output.errorMessage = L"Unrecognized command-line flag: '" + arg + L"'.";
                return output;
            }
        } else if (arg.rfind(L"/", 0) == 0 && arg.size() > 1) {
            std::wstring testContent = arg.substr(1);
            size_t eq = testContent.find(L'=');
            std::wstring testName = (eq == std::wstring::npos) ? testContent : testContent.substr(0, eq);
            for (auto& c : testName) c = static_cast<wchar_t>(towlower(c));
            if (testName == L"start" || testName == L"fullscreen" || testName == L"paused" || testName == L"loop" ||
                testName == L"volume" || testName == L"subtitle" || testName == L"enqueue" || testName == L"help" || testName == L"h" ||
                testName == L"?" || testName == L"benchmark-probe" || testName == L"benchmark") {
                isFlag = true;
                flagContent = testContent;
            } else {
                DWORD attr = GetFileAttributesW(arg.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES) {
                    output.result = CommandLineParseResult::UnknownFlag;
                    output.errorMessage = L"Unrecognized command-line flag: '" + arg + L"'.";
                    return output;
                }
            }
        }
        if (isFlag) {
            size_t eqPos = flagContent.find(L'=');
            std::wstring flagName = (eqPos == std::wstring::npos) ? flagContent : flagContent.substr(0, eqPos);
            std::wstring flagVal = (eqPos == std::wstring::npos) ? L"" : flagContent.substr(eqPos + 1);
            bool hasVal = (eqPos != std::wstring::npos);
            for (auto& c : flagName) c = static_cast<wchar_t>(towlower(c));
            if (flagName == L"start") {
                if (!hasVal || flagVal.empty()) {
                    output.result = CommandLineParseResult::MalformedFlag;
                    output.errorMessage = L"Missing value for --start. Expected --start=<seconds|hh:mm:ss>.";
                    return output;
                }
                double seconds = 0.0;
                std::wstring err;
                if (!ParseTimeValue(flagVal, seconds, err)) {
                    output.result = CommandLineParseResult::MalformedFlag;
                    output.errorMessage = L"Invalid --start value: " + err;
                    return output;
                }
                output.options.startPositionSec = seconds;
            } else if (flagName == L"fullscreen") {
                bool bVal = true;
                if (hasVal) {
                    std::wstring err;
                    if (!ParseBooleanFlag(flagVal, bVal, err)) {
                        output.result = CommandLineParseResult::MalformedFlag;
                        output.errorMessage = L"Invalid --fullscreen value: " + err;
                        return output;
                    }
                }
                output.options.fullscreen = bVal;
            } else if (flagName == L"paused") {
                bool bVal = true;
                if (hasVal) {
                    std::wstring err;
                    if (!ParseBooleanFlag(flagVal, bVal, err)) {
                        output.result = CommandLineParseResult::MalformedFlag;
                        output.errorMessage = L"Invalid --paused value: " + err;
                        return output;
                    }
                }
                output.options.paused = bVal;
            } else if (flagName == L"loop") {
                bool bVal = true;
                if (hasVal) {
                    std::wstring err;
                    if (!ParseBooleanFlag(flagVal, bVal, err)) {
                        output.result = CommandLineParseResult::MalformedFlag;
                        output.errorMessage = L"Invalid --loop value: " + err;
                        return output;
                    }
                }
                output.options.loop = bVal;
            } else if (flagName == L"volume") {
                if (!hasVal || flagVal.empty()) {
                    output.result = CommandLineParseResult::MalformedFlag;
                    output.errorMessage = L"Missing value for --volume. Expected --volume=<0..100>.";
                    return output;
                }
                double vol = 0.0;
                std::wstring err;
                if (!ParseVolumeValue(flagVal, vol, err)) {
                    output.result = CommandLineParseResult::MalformedFlag;
                    output.errorMessage = L"Invalid --volume value: " + err;
                    return output;
                }
                output.options.volume = vol;
            } else if (flagName == L"subtitle") {
                if (!hasVal || flagVal.empty()) {
                    output.result = CommandLineParseResult::MalformedFlag;
                    output.errorMessage = L"Missing value for --subtitle. Expected --subtitle=<path>.";
                    return output;
                }
                if (IsValidMediaUrl(flagVal)) { output.options.subtitlePath = flagVal; } else {
                    std::wstring fullSub = ResolveFullPathLong(flagVal);
                    output.options.subtitlePath = fullSub.empty() ? flagVal : fullSub;
                }
            } else if (flagName == L"enqueue") {
                bool bVal = true;
                if (hasVal) {
                    std::wstring err;
                    if (!ParseBooleanFlag(flagVal, bVal, err)) {
                        output.result = CommandLineParseResult::MalformedFlag;
                        output.errorMessage = L"Invalid --enqueue value: " + err;
                        return output;
                    }
                }
                output.options.enqueue = bVal;
            } else if (flagName == L"benchmark-probe" || flagName == L"benchmark") {
                output.options.benchmarkProbe = true;
                if (hasVal && !flagVal.empty()) {
                    bool bVal = true;
                    std::wstring boolErr;
                    if (ParseBooleanFlag(flagVal, bVal, boolErr)) {
                        output.options.benchmarkProbe = bVal;
                    } else { output.options.benchmarkOutputFile = flagVal; }
                }
            } else if (flagName == L"visual-capture-probe" || flagName == L"visual-probe" || flagName == L"visual-capture") {
                output.options.visualCaptureProbe = true;
                if (hasVal && !flagVal.empty()) {
                    bool bVal = true;
                    std::wstring boolErr;
                    if (ParseBooleanFlag(flagVal, bVal, boolErr)) {
                        output.options.visualCaptureProbe = bVal;
                    } else { output.options.visualCaptureOutputFile = flagVal; }
                }
            } else if (flagName == L"visual-capture-scene" || flagName == L"capture-scene" || flagName == L"scene") {
                if (hasVal && !flagVal.empty()) { output.options.visualCaptureScene = flagVal; }
            } else if (flagName == L"visual-capture-output" || flagName == L"capture-output") {
                if (hasVal && !flagVal.empty()) {
                    output.options.visualCaptureOutputFile = flagVal;
                    output.options.visualCaptureProbe = true;
                }
            } else if (flagName == L"help" || flagName == L"h" || flagName == L"?") {
                // Help switch
            } else {
                output.result = CommandLineParseResult::UnknownFlag;
                output.errorMessage = L"Unrecognized command-line flag: '" + arg + L"'.";
                return output;
            }
        } else {
            if (IsValidMediaUrl(arg)) { output.options.mediaTargets.push_back(arg); } else {
                std::wstring fullPath = ResolveFullPathLong(arg);
                if (!fullPath.empty()) {
                    output.options.mediaTargets.push_back(fullPath);
                } else { output.options.mediaTargets.push_back(arg); }
            }
        }
    }
    if (output.options.mediaTargets.size() > MAX_IPC_FILE_COUNT) {
        output.result = CommandLineParseResult::TooManyArguments;
        output.errorMessage = L"Media targets count exceeds limit (" + std::to_wstring(MAX_IPC_FILE_COUNT) + L").";
        return output;
    }
    output.result = CommandLineParseResult::Success;
    return output;
}

inline CommandLineParseOutput ParseCommandLine(const std::wstring& cmdLine) {
    if (cmdLine.empty()) { return CommandLineParseOutput{}; }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv || argc <= 1) {
        if (argv) LocalFree(argv);
        return CommandLineParseOutput{};
    }
    std::vector<std::wstring> args;
    args.reserve(argc - 1);
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) { args.push_back(argv[i]); }
    }
    LocalFree(argv);
    return ParseCommandLineArgs(args);
}

// Serialize command-line options into bounded versioned envelope (Version 2)
inline std::vector<uint8_t> SerializeEnvelope( const PlaybackCommandLineOptions& options, Command cmd = Command::OpenFiles,
    uint32_t flags = 1) // default: activate
{
    if (options.mediaTargets.size() > MAX_IPC_FILE_COUNT) { return {}; }
    if (options.subtitlePath.size() > MAX_IPC_PATH_CHARS) { return {}; }
    std::vector<uint8_t> payload;
    uint32_t optFlags = 0;
    if (options.startPositionSec.has_value()) optFlags |= OptionFlags::HasStartPos;
    if (options.fullscreen) optFlags |= OptionFlags::Fullscreen;
    if (options.paused) optFlags |= OptionFlags::Paused;
    if (options.loop) optFlags |= OptionFlags::Loop;
    if (options.volume.has_value()) optFlags |= OptionFlags::HasVolume;
    if (!options.subtitlePath.empty()) optFlags |= OptionFlags::HasSubtitle;
    if (options.enqueue) optFlags |= OptionFlags::Enqueue;
    const uint8_t* optFlagsBytes = reinterpret_cast<const uint8_t*>(&optFlags);
    payload.insert(payload.end(), optFlagsBytes, optFlagsBytes + sizeof(uint32_t));
    double startSec = options.startPositionSec.value_or(0.0);
    const uint8_t* startBytes = reinterpret_cast<const uint8_t*>(&startSec);
    payload.insert(payload.end(), startBytes, startBytes + sizeof(double));
    double vol = options.volume.value_or(100.0);
    const uint8_t* volBytes = reinterpret_cast<const uint8_t*>(&vol);
    payload.insert(payload.end(), volBytes, volBytes + sizeof(double));
    uint32_t subCharCount = static_cast<uint32_t>(options.subtitlePath.size());
    const uint8_t* subCharCountBytes = reinterpret_cast<const uint8_t*>(&subCharCount);
    payload.insert(payload.end(), subCharCountBytes, subCharCountBytes + sizeof(uint32_t));
    if (subCharCount > 0) {
        const uint8_t* subBytes = reinterpret_cast<const uint8_t*>(options.subtitlePath.data());
        payload.insert(payload.end(), subBytes, subBytes + (subCharCount * sizeof(wchar_t)));
    }
    for (const auto& path : options.mediaTargets) {
        if (path.size() > MAX_IPC_PATH_CHARS) { return {}; }
        uint32_t charCount = static_cast<uint32_t>(path.size());
        const uint8_t* charCountBytes = reinterpret_cast<const uint8_t*>(&charCount);
        payload.insert(payload.end(), charCountBytes, charCountBytes + sizeof(uint32_t));
        if (charCount > 0) {
            const uint8_t* pathBytes = reinterpret_cast<const uint8_t*>(path.data());
            payload.insert(payload.end(), pathBytes, pathBytes + (charCount * sizeof(wchar_t)));
        }
    }
    uint32_t totalSize = static_cast<uint32_t>(sizeof(EnvelopeHeader) + payload.size());
    if (totalSize > MAX_IPC_PAYLOAD_SIZE) { return {}; }
    EnvelopeHeader header = {};
    header.magic = IPC_MAGIC_V2;
    header.version = IPC_CURRENT_VERSION;
    header.headerSize = sizeof(EnvelopeHeader);
    header.totalSize = totalSize;
    header.senderPid = GetCurrentProcessId();
    header.command = static_cast<uint32_t>(cmd);
    header.fileCount = static_cast<uint32_t>(options.mediaTargets.size());
    header.flags = flags;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    header.timestamp = static_cast<uint64_t>(qpc.QuadPart);
    header.checksum = payload.empty() ? 0 : ComputeChecksum(payload.data(), payload.size());
    std::vector<uint8_t> result;
    result.resize(totalSize);
    std::memcpy(result.data(), &header, sizeof(EnvelopeHeader));
    if (!payload.empty()) { std::memcpy(result.data() + sizeof(EnvelopeHeader), payload.data(), payload.size()); }
    return result;
}

// Overload for vector of paths (legacy / convenience wrapper)
inline std::vector<uint8_t> SerializeEnvelope( const std::vector<std::wstring>& paths, Command cmd = Command::OpenFiles, uint32_t flags = 1)
{ PlaybackCommandLineOptions opts; opts.mediaTargets = paths; return SerializeEnvelope(opts, cmd, flags); }

// Validate and deserialize envelope into PlaybackCommandLineOptions
inline ValidationResult DeserializeEnvelope( const void* data, size_t size, PlaybackCommandLineOptions& outOptions,
    EnvelopeHeader *outHeader = nullptr, bool authenticateSender = true) {
    outOptions = {};
    if (!data) { return ValidationResult::NullData; }
    if (size < sizeof(EnvelopeHeader)) { return ValidationResult::PayloadTooSmall; }
    if (size > MAX_IPC_PAYLOAD_SIZE) { return ValidationResult::PayloadOversized; }
    const EnvelopeHeader* header = reinterpret_cast<const EnvelopeHeader*>(data);
    if (header->magic != IPC_MAGIC_V2) { return ValidationResult::InvalidMagic; }
    if (header->version != 1 && header->version != 2) { return ValidationResult::UnsupportedVersion; }
    if (header->headerSize != sizeof(EnvelopeHeader)) { return ValidationResult::InvalidHeaderSize; }
    if (header->totalSize != size) { return ValidationResult::TotalSizeMismatch; }
    if (header->fileCount > MAX_IPC_FILE_COUNT) { return ValidationResult::FileCountExceeded; }
    if (authenticateSender && !IsSameUserAndSession(header->senderPid)) { return ValidationResult::SenderUnauthorized; }
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(EnvelopeHeader);
    size_t payloadSize = size - sizeof(EnvelopeHeader);
    uint32_t calcChecksum = payloadSize == 0 ? 0 : ComputeChecksum(payload, payloadSize);
    if (calcChecksum != header->checksum) { return ValidationResult::ChecksumMismatch; }
    size_t offset = 0;
    if (header->version == 2) {
        // Version 2 layout: options header followed by paths
        if (offset + sizeof(uint32_t) + sizeof(double) + sizeof(double) + sizeof(uint32_t) > payloadSize)
            return ValidationResult::MalformedPayload;
        uint32_t optFlags = 0;
        std::memcpy(&optFlags, payload + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        double startSec = 0.0;
        std::memcpy(&startSec, payload + offset, sizeof(double));
        offset += sizeof(double);
        double vol = 100.0;
        std::memcpy(&vol, payload + offset, sizeof(double));
        offset += sizeof(double);
        uint32_t subCharCount = 0;
        std::memcpy(&subCharCount, payload + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        if (subCharCount > MAX_IPC_PATH_CHARS) { return ValidationResult::PathTooLong; }
        size_t subByteCount = subCharCount * sizeof(wchar_t);
        if (offset + subByteCount > payloadSize) { return ValidationResult::MalformedPayload; }
        std::wstring subPath;
        if (subCharCount > 0) {
            subPath.resize(subCharCount);
            std::memcpy(&subPath[0], payload + offset, subByteCount);
            offset += subByteCount;
            if (subPath.find(L'\0') != std::wstring::npos) { return ValidationResult::MalformedPayload; }
        }
        if (optFlags & OptionFlags::HasStartPos) {
            if (!std::isfinite(startSec) || startSec < 0.0) { return ValidationResult::MalformedPayload; }
            outOptions.startPositionSec = startSec;
        }
        outOptions.fullscreen = (optFlags & OptionFlags::Fullscreen) != 0;
        outOptions.paused = (optFlags & OptionFlags::Paused) != 0;
        outOptions.loop = (optFlags & OptionFlags::Loop) != 0;
        if (optFlags & OptionFlags::HasVolume) {
            if (!std::isfinite(vol) || vol < 0.0 || vol > 100.0) { return ValidationResult::MalformedPayload; }
            outOptions.volume = vol;
        }
        if (optFlags & OptionFlags::HasSubtitle) { outOptions.subtitlePath = subPath; }
        outOptions.enqueue = (optFlags & OptionFlags::Enqueue) != 0;
    }
    // Parse media paths
    for (uint32_t i = 0; i < header->fileCount; ++i) {
        if (offset + sizeof(uint32_t) > payloadSize) { return ValidationResult::MalformedPayload; }
        uint32_t charCount = 0;
        std::memcpy(&charCount, payload + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        if (charCount > MAX_IPC_PATH_CHARS) { return ValidationResult::PathTooLong; }
        size_t byteCount = charCount * sizeof(wchar_t);
        if (offset + byteCount > payloadSize) { return ValidationResult::MalformedPayload; }
        if (charCount > 0) {
            std::wstring path(charCount, L'\0');
            std::memcpy(&path[0], payload + offset, byteCount);
            offset += byteCount;
            if (path.find(L'\0') != std::wstring::npos) { return ValidationResult::MalformedPayload; }
            outOptions.mediaTargets.push_back(std::move(path));
        } else { outOptions.mediaTargets.push_back(L""); }
    }
    if (offset != payloadSize) { return ValidationResult::MalformedPayload; }
    if (outHeader) { *outHeader = *header; }
    return ValidationResult::Success;
}

// Overload for vector of paths (legacy / convenience wrapper)
inline ValidationResult DeserializeEnvelope( const void* data, size_t size, std::vector<std::wstring>& outPaths,
    EnvelopeHeader *outHeader = nullptr, bool authenticateSender = true) {
    PlaybackCommandLineOptions opts;
    ValidationResult res = DeserializeEnvelope(data, size, opts, outHeader, authenticateSender);
    if (res == ValidationResult::Success) { outPaths = std::move(opts.mediaTargets); } else { outPaths.clear(); }
    return res;
}

// Convert vector of paths to the newline-delimited legacy format expected by App::HandleMessage
inline std::wstring PathsToAppPayload(const std::vector<std::wstring>& paths) {
    std::wstring payload;
    for (const auto& path : paths) {
        if (!path.empty()) { payload += path; payload += L'\n'; }
    }
    return payload;
}


inline std::vector<uint8_t> SerializeAutomationEnvelope(const AutomationCommand &cmd, uint32_t flags = 0) {
    if (cmd.op == AutomationOp::None || static_cast<uint32_t>(cmd.op) > static_cast<uint32_t>(AutomationOp::QueryStatus)) { return {}; }
    if (cmd.op == AutomationOp::Seek) {
        if (!std::isfinite(cmd.seekPositionSec)) return {};
        if (cmd.seekRelative) {
            if (cmd.seekPositionSec < -8640000.0 || cmd.seekPositionSec > 8640000.0) return {};
        } else {
            if (cmd.seekPositionSec < 0.0 || cmd.seekPositionSec > 8640000.0) return {};
        }
    } else if (cmd.op == AutomationOp::Volume) {
        if (!std::isfinite(cmd.volume) || cmd.volume < 0.0 || cmd.volume > 100.0) return {};
    } else if (cmd.op == AutomationOp::Open) {
        if (cmd.mediaTargets.empty() || cmd.mediaTargets.size() > MAX_IPC_FILE_COUNT) return {};
        if (!std::isfinite(cmd.startPositionSec) || cmd.startPositionSec < 0.0 || cmd.startPositionSec > 8640000.0) return {};
        for (const auto& target : cmd.mediaTargets) {
            if (target.empty() || target.size() > MAX_IPC_PATH_CHARS || target.find(L'\0') != std::wstring::npos) return {};
        }
    } else if (cmd.op == AutomationOp::Enqueue) {
        if (cmd.mediaTargets.empty() || cmd.mediaTargets.size() > MAX_IPC_FILE_COUNT) return {};
        for (const auto& target : cmd.mediaTargets) {
            if (target.empty() || target.size() > MAX_IPC_PATH_CHARS || target.find(L'\0') != std::wstring::npos) return {};
        }
    }
    std::vector<uint8_t> payload;
    uint32_t opCode = static_cast<uint32_t>(cmd.op);
    const uint8_t* opBytes = reinterpret_cast<const uint8_t*>(&opCode);
    payload.insert(payload.end(), opBytes, opBytes + sizeof(uint32_t));
    uint32_t autoFlags = 0;
    if (cmd.seekRelative) autoFlags |= AutomationFlags::SeekRelative;
    if (cmd.paused) autoFlags |= AutomationFlags::Paused;
    if (cmd.fullscreen) autoFlags |= AutomationFlags::Fullscreen;
    const uint8_t* flagsBytes = reinterpret_cast<const uint8_t*>(&autoFlags);
    payload.insert(payload.end(), flagsBytes, flagsBytes + sizeof(uint32_t));
    double seekPos = cmd.seekPositionSec;
    const uint8_t* seekBytes = reinterpret_cast<const uint8_t*>(&seekPos);
    payload.insert(payload.end(), seekBytes, seekBytes + sizeof(double));
    double vol = cmd.volume;
    const uint8_t* volBytes = reinterpret_cast<const uint8_t*>(&vol);
    payload.insert(payload.end(), volBytes, volBytes + sizeof(double));
    double startPos = cmd.startPositionSec;
    const uint8_t* startBytes = reinterpret_cast<const uint8_t*>(&startPos);
    payload.insert(payload.end(), startBytes, startBytes + sizeof(double));
    uint32_t pathCount = static_cast<uint32_t>(cmd.mediaTargets.size());
    const uint8_t* pathCountBytes = reinterpret_cast<const uint8_t*>(&pathCount);
    payload.insert(payload.end(), pathCountBytes, pathCountBytes + sizeof(uint32_t));
    for (const auto& path : cmd.mediaTargets) {
        uint32_t charCount = static_cast<uint32_t>(path.size());
        const uint8_t* charCountBytes = reinterpret_cast<const uint8_t*>(&charCount);
        payload.insert(payload.end(), charCountBytes, charCountBytes + sizeof(uint32_t));
        if (charCount > 0) {
            const uint8_t* pathBytes = reinterpret_cast<const uint8_t*>(path.data());
            payload.insert(payload.end(), pathBytes, pathBytes + (charCount * sizeof(wchar_t)));
        }
    }
    uint32_t totalSize = static_cast<uint32_t>(sizeof(EnvelopeHeader) + payload.size());
    if (totalSize > MAX_IPC_PAYLOAD_SIZE) { return {}; }
    EnvelopeHeader header = {};
    header.magic = IPC_MAGIC_V2;
    header.version = IPC_CURRENT_VERSION;
    header.headerSize = sizeof(EnvelopeHeader);
    header.totalSize = totalSize;
    header.senderPid = GetCurrentProcessId();
    header.command = static_cast<uint32_t>(Command::Automation);
    header.fileCount = pathCount;
    header.flags = flags;
    header.timestamp = GetTickCount64();
    header.checksum = payload.empty() ? 0 : ComputeChecksum(payload.data(), payload.size());
    std::vector<uint8_t> result;
    result.resize(totalSize);
    std::memcpy(result.data(), &header, sizeof(EnvelopeHeader));
    if (!payload.empty()) { std::memcpy(result.data() + sizeof(EnvelopeHeader), payload.data(), payload.size()); }
    return result;
}

inline ValidationResult DeserializeAutomationEnvelope( const void* data, size_t size, AutomationCommand& outCmd,
    EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true, bool checkReplay = false, uint64_t nowMs = 0)
{
    outCmd = {};
    if (!data) { return ValidationResult::NullData; }
    if (size < sizeof(EnvelopeHeader)) { return ValidationResult::PayloadTooSmall; }
    if (size > MAX_IPC_PAYLOAD_SIZE) { return ValidationResult::PayloadOversized; }
    const EnvelopeHeader* header = reinterpret_cast<const EnvelopeHeader*>(data);
    if (header->magic != IPC_MAGIC_V2) { return ValidationResult::InvalidMagic; }
    if (header->version != 2) { return ValidationResult::UnsupportedVersion; }
    if (header->headerSize != sizeof(EnvelopeHeader)) { return ValidationResult::InvalidHeaderSize; }
    if (header->totalSize != size) { return ValidationResult::TotalSizeMismatch; }
    if (header->command != static_cast<uint32_t>(Command::Automation)) { return ValidationResult::InvalidOperation; }
    if (header->fileCount > MAX_IPC_FILE_COUNT) { return ValidationResult::FileCountExceeded; }
    if (authenticateSender && !IsSameUserAndSession(header->senderPid)) { return ValidationResult::SenderUnauthorized; }
    if (checkReplay && !ReplayFilter::ValidateAndRecord(*header, nowMs)) { return ValidationResult::ReplayDetected; }
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(EnvelopeHeader);
    size_t payloadSize = size - sizeof(EnvelopeHeader);
    uint32_t calcChecksum = payloadSize == 0 ? 0 : ComputeChecksum(payload, payloadSize);
    if (calcChecksum != header->checksum) { return ValidationResult::ChecksumMismatch; }
    size_t offset = 0;
    if (offset + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(double) + sizeof(double) + sizeof(double) + sizeof(uint32_t) > payloadSize)
        return ValidationResult::MalformedPayload;
    uint32_t opCode = 0;
    std::memcpy(&opCode, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (opCode == 0 || opCode > static_cast<uint32_t>(AutomationOp::QueryStatus)) { return ValidationResult::InvalidOperation; }
    outCmd.op = static_cast<AutomationOp>(opCode);
    uint32_t autoFlags = 0;
    std::memcpy(&autoFlags, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    outCmd.seekRelative = (autoFlags & AutomationFlags::SeekRelative) != 0;
    outCmd.paused = (autoFlags & AutomationFlags::Paused) != 0;
    outCmd.fullscreen = (autoFlags & AutomationFlags::Fullscreen) != 0;
    double seekPos = 0.0;
    std::memcpy(&seekPos, payload + offset, sizeof(double));
    offset += sizeof(double);
    if (!std::isfinite(seekPos)) { return ValidationResult::InvalidArgument; }
    outCmd.seekPositionSec = seekPos;
    double vol = 0.0;
    std::memcpy(&vol, payload + offset, sizeof(double));
    offset += sizeof(double);
    if (!std::isfinite(vol)) { return ValidationResult::InvalidArgument; }
    outCmd.volume = vol;
    double startPos = 0.0;
    std::memcpy(&startPos, payload + offset, sizeof(double));
    offset += sizeof(double);
    if (!std::isfinite(startPos)) { return ValidationResult::InvalidArgument; }
    outCmd.startPositionSec = startPos;
    uint32_t pathCount = 0;
    std::memcpy(&pathCount, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (pathCount > MAX_IPC_FILE_COUNT || pathCount != header->fileCount) { return ValidationResult::FileCountExceeded; }
    for (uint32_t i = 0; i < pathCount; ++i) {
        if (offset + sizeof(uint32_t) > payloadSize) { return ValidationResult::MalformedPayload; }
        uint32_t charCount = 0;
        std::memcpy(&charCount, payload + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        if (charCount > MAX_IPC_PATH_CHARS) { return ValidationResult::PathTooLong; }
        size_t byteCount = charCount * sizeof(wchar_t);
        if (offset + byteCount > payloadSize) { return ValidationResult::MalformedPayload; }
        if (charCount > 0) {
            std::wstring path(charCount, L'\0');
            std::memcpy(&path[0], payload + offset, byteCount);
            offset += byteCount;
            if (path.find(L'\0') != std::wstring::npos) { return ValidationResult::MalformedPayload; }
            if (IsValidMediaUrl(path)) {
                outCmd.mediaTargets.push_back(path);
            } else { std::wstring fullPath = ResolveFullPathLong(path); outCmd.mediaTargets.push_back(fullPath.empty() ? path : fullPath); }
        } else { outCmd.mediaTargets.push_back(L""); }
    }
    if (offset != payloadSize) { return ValidationResult::MalformedPayload; }
    if (outCmd.op == AutomationOp::Seek) {
        if (outCmd.seekRelative) {
            if (outCmd.seekPositionSec < -8640000.0 || outCmd.seekPositionSec > 8640000.0) { return ValidationResult::InvalidArgument; }
        } else {
            if (outCmd.seekPositionSec < 0.0 || outCmd.seekPositionSec > 8640000.0) { return ValidationResult::InvalidArgument; }
        }
    } else if (outCmd.op == AutomationOp::Volume) {
        if (outCmd.volume < 0.0 || outCmd.volume > 100.0) { return ValidationResult::InvalidArgument; }
    } else if (outCmd.op == AutomationOp::Open) {
        if (outCmd.mediaTargets.empty()) { return ValidationResult::InvalidArgument; }
        if (outCmd.startPositionSec < 0.0 || outCmd.startPositionSec > 8640000.0) { return ValidationResult::InvalidArgument; }
    } else if (outCmd.op == AutomationOp::Enqueue) {
        if (outCmd.mediaTargets.empty()) { return ValidationResult::InvalidArgument; }
    }
    if (outHeader) { *outHeader = *header; }
    return ValidationResult::Success;
}

inline std::vector<uint8_t> SerializeAutomationResponse(const AutomationStatus &status, uint32_t statusResult = 0) {
    if (status.currentFilePath.size() > MAX_IPC_PATH_CHARS) { return {}; }
    std::vector<uint8_t> payload;
    const uint8_t* resBytes = reinterpret_cast<const uint8_t*>(&statusResult);
    payload.insert(payload.end(), resBytes, resBytes + sizeof(uint32_t));
    uint32_t stateVal = static_cast<uint32_t>(status.state);
    const uint8_t* stateBytes = reinterpret_cast<const uint8_t*>(&stateVal);
    payload.insert(payload.end(), stateBytes, stateBytes + sizeof(uint32_t));
    double pos = status.positionSec;
    const uint8_t* posBytes = reinterpret_cast<const uint8_t*>(&pos);
    payload.insert(payload.end(), posBytes, posBytes + sizeof(double));
    double dur = status.durationSec;
    const uint8_t* durBytes = reinterpret_cast<const uint8_t*>(&dur);
    payload.insert(payload.end(), durBytes, durBytes + sizeof(double));
    double vol = status.volume;
    const uint8_t* volBytes = reinterpret_cast<const uint8_t*>(&vol);
    payload.insert(payload.end(), volBytes, volBytes + sizeof(double));
    uint32_t statusFlags = 0;
    if (status.isMuted) statusFlags |= StatusFlags::Muted;
    if (status.isLoop) statusFlags |= StatusFlags::Loop;
    if (status.isFullscreen) statusFlags |= StatusFlags::Fullscreen;
    const uint8_t* flagsBytes = reinterpret_cast<const uint8_t*>(&statusFlags);
    payload.insert(payload.end(), flagsBytes, flagsBytes + sizeof(uint32_t));
    uint32_t charCount = static_cast<uint32_t>(status.currentFilePath.size());
    const uint8_t* charCountBytes = reinterpret_cast<const uint8_t*>(&charCount);
    payload.insert(payload.end(), charCountBytes, charCountBytes + sizeof(uint32_t));
    if (charCount > 0) {
        const uint8_t* pathBytes = reinterpret_cast<const uint8_t*>(status.currentFilePath.data());
        payload.insert(payload.end(), pathBytes, pathBytes + (charCount * sizeof(wchar_t)));
    }
    uint32_t totalSize = static_cast<uint32_t>(sizeof(EnvelopeHeader) + payload.size());
    if (totalSize > MAX_IPC_PAYLOAD_SIZE) { return {}; }
    EnvelopeHeader header = {};
    header.magic = IPC_MAGIC_V2;
    header.version = IPC_CURRENT_VERSION;
    header.headerSize = sizeof(EnvelopeHeader);
    header.totalSize = totalSize;
    header.senderPid = GetCurrentProcessId();
    header.command = static_cast<uint32_t>(Command::AutomationResponse);
    header.fileCount = charCount > 0 ? 1 : 0;
    header.flags = 0;
    header.timestamp = GetTickCount64();
    header.checksum = payload.empty() ? 0 : ComputeChecksum(payload.data(), payload.size());
    std::vector<uint8_t> result;
    result.resize(totalSize);
    std::memcpy(result.data(), &header, sizeof(EnvelopeHeader));
    if (!payload.empty()) { std::memcpy(result.data() + sizeof(EnvelopeHeader), payload.data(), payload.size()); }
    return result;
}

inline ValidationResult DeserializeAutomationResponse( const void* data, size_t size, AutomationStatus& outStatus,
    uint32_t* outStatusResult = nullptr, EnvelopeHeader* outHeader = nullptr, bool authenticateSender = true)
{
    outStatus = {};
    if (outStatusResult) *outStatusResult = 1;
    if (!data) return ValidationResult::NullData;
    if (size < sizeof(EnvelopeHeader)) return ValidationResult::PayloadTooSmall;
    if (size > MAX_IPC_PAYLOAD_SIZE) return ValidationResult::PayloadOversized;
    const EnvelopeHeader* header = reinterpret_cast<const EnvelopeHeader*>(data);
    if (header->magic != IPC_MAGIC_V2) return ValidationResult::InvalidMagic;
    if (header->version != 2) return ValidationResult::UnsupportedVersion;
    if (header->headerSize != sizeof(EnvelopeHeader)) return ValidationResult::InvalidHeaderSize;
    if (header->totalSize != size) return ValidationResult::TotalSizeMismatch;
    if (header->command != static_cast<uint32_t>(Command::AutomationResponse)) return ValidationResult::InvalidOperation;
    if (authenticateSender && !IsSameUserAndSession(header->senderPid)) { return ValidationResult::SenderUnauthorized; }
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(data) + sizeof(EnvelopeHeader);
    size_t payloadSize = size - sizeof(EnvelopeHeader);
    uint32_t calcChecksum = payloadSize == 0 ? 0 : ComputeChecksum(payload, payloadSize);
    if (calcChecksum != header->checksum) { return ValidationResult::ChecksumMismatch; }
    size_t offset = 0;
    if (offset + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(double) + sizeof(double) + sizeof(double) + sizeof(uint32_t) + sizeof(uint32_t) > payloadSize)
        return ValidationResult::MalformedPayload;
    uint32_t resCode = 0;
    std::memcpy(&resCode, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (outStatusResult) *outStatusResult = resCode;
    uint32_t stateVal = 0;
    std::memcpy(&stateVal, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    outStatus.state = static_cast<AutomationPlaybackState>(stateVal);
    double pos = 0.0;
    std::memcpy(&pos, payload + offset, sizeof(double));
    offset += sizeof(double);
    outStatus.positionSec = pos;
    double dur = 0.0;
    std::memcpy(&dur, payload + offset, sizeof(double));
    offset += sizeof(double);
    outStatus.durationSec = dur;
    double vol = 0.0;
    std::memcpy(&vol, payload + offset, sizeof(double));
    offset += sizeof(double);
    outStatus.volume = vol;
    uint32_t statusFlags = 0;
    std::memcpy(&statusFlags, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    outStatus.isMuted = (statusFlags & StatusFlags::Muted) != 0;
    outStatus.isLoop = (statusFlags & StatusFlags::Loop) != 0;
    outStatus.isFullscreen = (statusFlags & StatusFlags::Fullscreen) != 0;
    uint32_t charCount = 0;
    std::memcpy(&charCount, payload + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (charCount > MAX_IPC_PATH_CHARS) { return ValidationResult::PathTooLong; }
    size_t byteCount = charCount * sizeof(wchar_t);
    if (offset + byteCount > payloadSize) { return ValidationResult::MalformedPayload; }
    if (charCount > 0) {
        outStatus.currentFilePath.resize(charCount);
        std::memcpy(&outStatus.currentFilePath[0], payload + offset, byteCount);
        offset += byteCount;
        if (outStatus.currentFilePath.find(L'\0') != std::wstring::npos) { return ValidationResult::MalformedPayload; }
    }
    if (offset != payloadSize) { return ValidationResult::MalformedPayload; }
    if (outHeader) { *outHeader = *header; }
    return ValidationResult::Success;
}

inline bool SendAutomationCommand( HWND hTargetWnd, const AutomationCommand& cmd, AutomationStatus* outStatus = nullptr,
    DWORD timeoutMs = 3000) {
    if (!hTargetWnd || !IsWindow(hTargetWnd)) { return false; }
    DWORD targetPid = 0;
    GetWindowThreadProcessId(hTargetWnd, &targetPid);
    if (!IsSameUserAndSession(targetPid)) { return false; }
    std::vector<uint8_t> envelope = SerializeAutomationEnvelope(cmd);
    if (envelope.empty()) { return false; }
    HWND hReplyWnd = NULL;
    AutomationStatus receivedStatus = {};
    struct ReplyContext {
        AutomationStatus* pStatus;
        bool received;
    } replyCtx = { &receivedStatus, false };
    if (cmd.op == AutomationOp::QueryStatus && outStatus != nullptr) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (msg == WM_COPYDATA) {
                PCOPYDATASTRUCT pCDS = reinterpret_cast<PCOPYDATASTRUCT>(lParam);
                ReplyContext* ctx = reinterpret_cast<ReplyContext*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
                if (pCDS && ctx && ctx->pStatus && pCDS->dwData == IPC_MAGIC_V2) {
                    uint32_t resCode = 0;
                    ValidationResult vRes = DeserializeAutomationResponse(
                        pCDS->lpData, pCDS->cbData, *ctx->pStatus, &resCode, nullptr, true );
                    if (vRes == ValidationResult::Success && resCode == 0) { ctx->received = true; return TRUE; }
                }
                return FALSE;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"Aperture_Automation_Reply_Wnd";
        RegisterClassExW(&wc);
        hReplyWnd = CreateWindowExW( 0, L"Aperture_Automation_Reply_Wnd", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL );
        if (hReplyWnd) { SetWindowLongPtrW(hReplyWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&replyCtx)); }
    }
    COPYDATASTRUCT cds = {};
    cds.dwData = IPC_MAGIC_V2;
    cds.cbData = static_cast<DWORD>(envelope.size());
    cds.lpData = envelope.data();
    DWORD_PTR dwResult = 0;
    BOOL sent = SendMessageTimeoutW( hTargetWnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hReplyWnd), reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &dwResult );
    if (hReplyWnd) {
        if (replyCtx.received && outStatus) { *outStatus = receivedStatus; }
        DestroyWindow(hReplyWnd);
        UnregisterClassW(L"Aperture_Automation_Reply_Wnd", GetModuleHandleW(NULL));
    }
    return sent && dwResult != 0;
}
} // namespace Ipc
} // namespace Aperture
