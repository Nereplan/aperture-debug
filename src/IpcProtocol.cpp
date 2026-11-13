#ifndef APERTURE_IPC_INLINE
#define APERTURE_IPC_CPP_UNIT 1
#define APERTURE_IPC_INLINE
#endif

#include "IpcProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <shellapi.h>
#include <string_view>
#include <utility>

namespace Aperture {
namespace Ipc {

namespace Detail {

APERTURE_IPC_INLINE std::wstring ToLowerWide(std::wstring_view text) {
    std::wstring lower(text.size(), L'\0');
    for (size_t i = 0; i < text.size(); ++i) lower[i] = static_cast<wchar_t>(towlower(text[i]));
    return lower;
}

template <typename T>
APERTURE_IPC_INLINE void AppendPod(std::vector<uint8_t>& out, const T& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

APERTURE_IPC_INLINE void AppendLengthPrefixedWString(std::vector<uint8_t>& out, const std::wstring& str) {
    const uint32_t charCount = static_cast<uint32_t>(str.size());
    AppendPod(out, charCount);
    if (charCount > 0) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(str.data());
        out.insert(out.end(), bytes, bytes + (charCount * sizeof(wchar_t)));
    }
}

template <typename T>
APERTURE_IPC_INLINE bool ReadPod(const uint8_t* payload, size_t payloadSize, size_t& offset, T& out) {
    if (offset + sizeof(T) > payloadSize) return false;
    std::memcpy(&out, payload + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

APERTURE_IPC_INLINE ValidationResult ReadLengthPrefixedWString(
    const uint8_t* payload, size_t payloadSize, size_t& offset, std::wstring& out) {
    uint32_t charCount = 0;
    if (!ReadPod(payload, payloadSize, offset, charCount)) return ValidationResult::MalformedPayload;
    if (charCount > MAX_IPC_PATH_CHARS) return ValidationResult::PathTooLong;
    const size_t byteCount = static_cast<size_t>(charCount) * sizeof(wchar_t);
    if (offset + byteCount > payloadSize) return ValidationResult::MalformedPayload;
    if (charCount > 0) {
        out.resize(charCount);
        std::memcpy(&out[0], payload + offset, byteCount);
        offset += byteCount;
        if (out.find(L'\0') != std::wstring::npos) return ValidationResult::MalformedPayload;
    } else {
        out.clear();
    }
    return ValidationResult::Success;
}

APERTURE_IPC_INLINE std::vector<uint8_t> FinalizeEnvelope(
    Command cmd, uint32_t fileCount, uint32_t flags, uint64_t timestamp, const std::vector<uint8_t>& payload) {
    const size_t total = sizeof(EnvelopeHeader) + payload.size();
    if (total > MAX_IPC_PAYLOAD_SIZE) return {};
    const uint32_t totalSize = static_cast<uint32_t>(total);
    EnvelopeHeader header = {};
    header.magic = IPC_MAGIC_V2;
    header.version = IPC_CURRENT_VERSION;
    header.headerSize = sizeof(EnvelopeHeader);
    header.totalSize = totalSize;
    header.senderPid = GetCurrentProcessId();
    header.command = static_cast<uint32_t>(cmd);
    header.fileCount = fileCount;
    header.flags = flags;
    header.timestamp = timestamp;
    header.checksum = payload.empty() ? 0 : ComputeChecksum(payload.data(), payload.size());
    std::vector<uint8_t> result(totalSize);
    std::memcpy(result.data(), &header, sizeof(EnvelopeHeader));
    if (!payload.empty()) std::memcpy(result.data() + sizeof(EnvelopeHeader), payload.data(), payload.size());
    return result;
}

APERTURE_IPC_INLINE ValidationResult ValidateEnvelopeFrame(const void* data, size_t size,
    bool requireVersion2, bool checkCommand, Command expectedCommand,
    bool enforceFileCount, bool authenticateSender, bool checkReplay, uint64_t nowMs,
    const EnvelopeHeader*& outHeaderPtr, const uint8_t*& outPayload, size_t& outPayloadSize) {
    if (!data) return ValidationResult::NullData;
    if (size < sizeof(EnvelopeHeader)) return ValidationResult::PayloadTooSmall;
    if (size > MAX_IPC_PAYLOAD_SIZE) return ValidationResult::PayloadOversized;
    const EnvelopeHeader* header = reinterpret_cast<const EnvelopeHeader*>(data);
    if (header->magic != IPC_MAGIC_V2) return ValidationResult::InvalidMagic;
    if (requireVersion2 ? (header->version != 2) : (header->version != 1 && header->version != 2))
        return ValidationResult::UnsupportedVersion;
    if (header->headerSize != sizeof(EnvelopeHeader)) return ValidationResult::InvalidHeaderSize;
    if (header->totalSize != size) return ValidationResult::TotalSizeMismatch;
    if (checkCommand && header->command != static_cast<uint32_t>(expectedCommand))
        return ValidationResult::InvalidOperation;
    if (enforceFileCount && header->fileCount > MAX_IPC_FILE_COUNT) return ValidationResult::FileCountExceeded;
    if (authenticateSender && !IsSameUserAndSession(header->senderPid)) return ValidationResult::SenderUnauthorized;
    if (checkReplay && !ReplayFilter::ValidateAndRecord(*header, nowMs)) return ValidationResult::ReplayDetected;
    outPayload = reinterpret_cast<const uint8_t*>(data) + sizeof(EnvelopeHeader);
    outPayloadSize = size - sizeof(EnvelopeHeader);
    const uint32_t calcChecksum = outPayloadSize == 0 ? 0 : ComputeChecksum(outPayload, outPayloadSize);
    if (calcChecksum != header->checksum) return ValidationResult::ChecksumMismatch;
    outHeaderPtr = header;
    return ValidationResult::Success;
}

APERTURE_IPC_INLINE bool ParseUnsignedSegment(
    const std::wstring& segment, bool allowDot, const wchar_t* label, double& outValue, std::wstring& outError) {
    size_t dotCount = 0;
    for (wchar_t c : segment) {
        if (allowDot && c == L'.') {
            if (++dotCount > 1) { outError = L"Multiple decimal points in " + std::wstring(label) + L": '" + segment + L"'."; return false; }
        } else if (c < L'0' || c > L'9') {
            outError = L"Invalid characters in " + std::wstring(label) + L": '" + segment + L"'.";
            return false;
        }
    }
    wchar_t* endptr = nullptr;
    const double val = wcstod(segment.c_str(), &endptr);
    if (!endptr || *endptr != L'\0' || !std::isfinite(val) || val < 0.0) {
        outError = L"Invalid " + std::wstring(label) + L": '" + segment + L"'.";
        return false;
    }
    outValue = val;
    return true;
}

APERTURE_IPC_INLINE bool IsKnownShortFlag(std::wstring_view flagContent) {
    const size_t eq = flagContent.find(L'=');
    const std::wstring testName = ToLowerWide(eq == std::wstring_view::npos ? flagContent : flagContent.substr(0, eq));
    static constexpr const wchar_t* kKnownFlags[] = {
        L"start", L"fullscreen", L"paused", L"loop", L"volume", L"subtitle",
        L"enqueue", L"help", L"h", L"?", L"benchmark-probe", L"benchmark"
    };
    for (const wchar_t* name : kKnownFlags) {
        if (testName == name) return true;
    }
    return false;
}

} // namespace Detail

APERTURE_IPC_INLINE bool ReplayFilter::ValidateAndRecord(const EnvelopeHeader& header, uint64_t nowMs) {
    if (nowMs == 0) nowMs = GetTickCount64();
    if (header.timestamp != 0) {
        if (header.timestamp > nowMs + kMaxFutureClockSkewMs) return false;
        if (nowMs > header.timestamp && (nowMs - header.timestamp) > kMaxAgeMs) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (history[i].senderPid == header.senderPid && history[i].timestamp == header.timestamp &&
            history[i].checksum == header.checksum) {
            return false;
        }
    }
    history[head] = { header.senderPid, header.timestamp, header.checksum };
    head = (head + 1) % kHistorySize;
    if (count < kHistorySize) count++;
    return true;
}

APERTURE_IPC_INLINE void ReplayFilter::Reset() {
    head = 0;
    count = 0;
    for (size_t i = 0; i < kHistorySize; ++i) history[i] = {};
}

APERTURE_IPC_INLINE uint32_t ComputeChecksum(const uint8_t* data, size_t len) {
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

APERTURE_IPC_INLINE bool IsSameUserAndSession(DWORD senderPid) {
    if (senderPid == 0) return false;
    if (senderPid == GetCurrentProcessId()) return true;
    HANDLE hSenderProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, senderPid);
    if (!hSenderProc) return false;
    HANDLE hSenderToken = NULL;
    if (!OpenProcessToken(hSenderProc, TOKEN_QUERY, &hSenderToken)) { CloseHandle(hSenderProc); return false; }
    HANDLE hMyToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hMyToken)) {
        CloseHandle(hSenderToken);
        CloseHandle(hSenderProc);
        return false;
    }
    bool authenticated = false;
    DWORD senderSession = 0, mySession = 0, retLen = 0;
    if (GetTokenInformation(hSenderToken, TokenSessionId, &senderSession, sizeof(senderSession), &retLen) &&
        GetTokenInformation(hMyToken, TokenSessionId, &mySession, sizeof(mySession), &retLen) &&
        senderSession == mySession) {
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
    CloseHandle(hMyToken);
    CloseHandle(hSenderToken);
    CloseHandle(hSenderProc);
    return authenticated;
}

APERTURE_IPC_INLINE std::wstring ResolveFullPathLong(const std::wstring& path) {
    if (path.empty()) return {};
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::wstring fullPath(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, fullPath.data(), nullptr);
    if (written == 0 || written >= required) return path;
    fullPath.resize(written);
    return fullPath;
}

APERTURE_IPC_INLINE bool IsValidMediaUrl(const std::wstring& str) {
    if (str.size() < 10 || str.size() > 2048) return false;
    for (wchar_t c : str) { if (c < 32 || c == 127) return false; }
    const std::wstring lower = Detail::ToLowerWide(str);
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 || lower.rfind(L"rtsp://", 0) == 0 ||
        lower.rfind(L"rtmp://", 0) == 0 || lower.rfind(L"udp://", 0) == 0 || lower.rfind(L"rtp://", 0) == 0) {
        const size_t schemeEnd = lower.find(L"://");
        if (schemeEnd == std::wstring::npos) return false;
        const size_t hostStart = schemeEnd + 3;
        size_t hostEnd = lower.find_first_of(L":/?#", hostStart);
        if (hostEnd == std::wstring::npos) hostEnd = lower.size();
        return hostEnd > hostStart;
    }
    return false;
}

APERTURE_IPC_INLINE bool ParseTimeValue(const std::wstring& str, double& outSeconds, std::wstring& outError) {
    if (str.empty()) { outError = L"Time value cannot be empty."; return false; }
    if (str[0] == L'-' || str[0] == L'+') { outError = L"Time value cannot have leading signs: '" + str + L"'."; return false; }
    const size_t colonCount = static_cast<size_t>(std::count(str.begin(), str.end(), L':'));
    if (colonCount > 2) { outError = L"Too many colons in time value: '" + str + L"'."; return false; }
    if (colonCount == 0) {
        double val = 0.0;
        if (!Detail::ParseUnsignedSegment(str, true, L"time value", val, outError)) return false;
        if (val > 8640000.0) { outError = L"Time value exceeds maximum limit (8640000 seconds)."; return false; }
        outSeconds = val;
        return true;
    }
    const size_t colon1 = str.find(L':');
    const size_t colon2 = colonCount == 2 ? str.find(L':', colon1 + 1) : std::wstring::npos;
    std::wstring hhStr, mmStr, ssStr;
    if (colonCount == 1) {
        mmStr = str.substr(0, colon1);
        ssStr = str.substr(colon1 + 1);
        if (mmStr.empty() || ssStr.empty()) { outError = L"Missing minutes or seconds in '" + str + L"'."; return false; }
    } else {
        hhStr = str.substr(0, colon1);
        mmStr = str.substr(colon1 + 1, colon2 - colon1 - 1);
        ssStr = str.substr(colon2 + 1);
        if (hhStr.empty() || mmStr.empty() || ssStr.empty()) {
            outError = L"Missing hours, minutes, or seconds in '" + str + L"'.";
            return false;
        }
    }
    double hh = 0.0, mm = 0.0, ss = 0.0;
    if (colonCount == 2 && !Detail::ParseUnsignedSegment(hhStr, false, L"hours", hh, outError)) return false;
    if (!Detail::ParseUnsignedSegment(mmStr, false, L"minutes", mm, outError)) return false;
    if (colonCount == 2 && mm >= 60.0) { outError = L"Minutes must be in [0, 60): '" + mmStr + L"'."; return false; }
    if (!Detail::ParseUnsignedSegment(ssStr, true, L"seconds", ss, outError)) return false;
    if (ss >= 60.0) { outError = L"Seconds must be in [0, 60): '" + ssStr + L"'."; return false; }
    const double total = hh * 3600.0 + mm * 60.0 + ss;
    if (!std::isfinite(total) || total > 8640000.0) { outError = L"Time value exceeds maximum limit."; return false; }
    outSeconds = total;
    return true;
}

APERTURE_IPC_INLINE bool ParseVolumeValue(const std::wstring& str, double& outVolume, std::wstring& outError) {
    if (str.empty()) { outError = L"Volume value cannot be empty."; return false; }
    if (str[0] == L'-' || str[0] == L'+') { outError = L"Volume value cannot have leading signs: '" + str + L"'."; return false; }
    double val = 0.0;
    if (!Detail::ParseUnsignedSegment(str, true, L"volume", val, outError) || val > 100.0) {
        if (outError.empty() || val > 100.0)
            outError = L"Volume value must be a number between 0 and 100: '" + str + L"'.";
        return false;
    }
    outVolume = val;
    return true;
}

APERTURE_IPC_INLINE bool ParseBooleanFlag(const std::wstring& str, bool& outVal, std::wstring& outError) {
    if (str.empty()) { outVal = true; return true; }
    const std::wstring lower = Detail::ToLowerWide(str);
    if (lower == L"true" || lower == L"1" || lower == L"yes" || lower == L"on") { outVal = true; return true; }
    if (lower == L"false" || lower == L"0" || lower == L"no" || lower == L"off") { outVal = false; return true; }
    outError = L"Invalid boolean value: '" + str + L"'. Expected true or false.";
    return false;
}

APERTURE_IPC_INLINE CommandLineParseOutput ParseCommandLineArgs(const std::vector<std::wstring>& args) {
    CommandLineParseOutput output;
    if (args.size() > MAX_IPC_FILE_COUNT) {
        output.result = CommandLineParseResult::TooManyArguments;
        output.errorMessage = L"Argument count exceeds limit (" + std::to_wstring(MAX_IPC_FILE_COUNT) + L").";
        return output;
    }
    for (const std::wstring& arg : args) {
        if (arg.empty()) continue;
        if (arg.size() > MAX_IPC_PATH_CHARS) {
            output.result = CommandLineParseResult::ArgumentTooLong;
            output.errorMessage = L"Argument exceeds maximum length limit (" + std::to_wstring(MAX_IPC_PATH_CHARS) + L").";
            return output;
        }
        bool isFlag = false;
        std::wstring flagContent;
        if (arg.rfind(L"--", 0) == 0) {
            isFlag = true;
            flagContent = arg.substr(2);
        } else if (arg.rfind(L"-", 0) == 0 && arg.size() > 1) {
            std::wstring testContent = arg.substr(1);
            if (!Detail::IsKnownShortFlag(testContent)) {
                output.result = CommandLineParseResult::UnknownFlag;
                output.errorMessage = L"Unrecognized command-line flag: '" + arg + L"'.";
                return output;
            }
            isFlag = true;
            flagContent = std::move(testContent);
        } else if (arg.rfind(L"/", 0) == 0 && arg.size() > 1) {
            std::wstring testContent = arg.substr(1);
            if (Detail::IsKnownShortFlag(testContent)) {
                isFlag = true;
                flagContent = std::move(testContent);
            }
#if defined(_WIN32)
            else if (GetFileAttributesW(arg.c_str()) == INVALID_FILE_ATTRIBUTES) {
                output.result = CommandLineParseResult::UnknownFlag;
                output.errorMessage = L"Unrecognized command-line flag: '" + arg + L"'.";
                return output;
            }
#endif
        }
        if (isFlag) {
            const size_t eqPos = flagContent.find(L'=');
            const std::wstring flagName = Detail::ToLowerWide(
                eqPos == std::wstring::npos ? flagContent : flagContent.substr(0, eqPos));
            const bool hasVal = (eqPos != std::wstring::npos);
            const std::wstring flagVal = hasVal ? flagContent.substr(eqPos + 1) : L"";
            auto parseBoolOpt = [&](const wchar_t* optName, bool& target) -> bool {
                bool bVal = true;
                if (hasVal) {
                    std::wstring err;
                    if (!ParseBooleanFlag(flagVal, bVal, err)) {
                        output.result = CommandLineParseResult::MalformedFlag;
                        output.errorMessage = L"Invalid --" + std::wstring(optName) + L" value: " + err;
                        return false;
                    }
                }
                target = bVal;
                return true;
            };
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
                if (!parseBoolOpt(L"fullscreen", output.options.fullscreen)) return output;
            } else if (flagName == L"paused") {
                if (!parseBoolOpt(L"paused", output.options.paused)) return output;
            } else if (flagName == L"loop") {
                if (!parseBoolOpt(L"loop", output.options.loop)) return output;
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
                if (IsValidMediaUrl(flagVal)) {
                    output.options.subtitlePath = flagVal;
                } else {
                    std::wstring fullSub = ResolveFullPathLong(flagVal);
                    output.options.subtitlePath = fullSub.empty() ? flagVal : std::move(fullSub);
                }
            } else if (flagName == L"enqueue") {
                if (!parseBoolOpt(L"enqueue", output.options.enqueue)) return output;
            } else if (flagName == L"benchmark-probe" || flagName == L"benchmark") {
                output.options.benchmarkProbe = true;
                if (hasVal && !flagVal.empty()) {
                    bool bVal = true;
                    std::wstring boolErr;
                    if (ParseBooleanFlag(flagVal, bVal, boolErr)) output.options.benchmarkProbe = bVal;
                    else output.options.benchmarkOutputFile = flagVal;
                }
            } else if (flagName == L"visual-capture-probe" || flagName == L"visual-probe" || flagName == L"visual-capture") {
                output.options.visualCaptureProbe = true;
                if (hasVal && !flagVal.empty()) {
                    bool bVal = true;
                    std::wstring boolErr;
                    if (ParseBooleanFlag(flagVal, bVal, boolErr)) output.options.visualCaptureProbe = bVal;
                    else output.options.visualCaptureOutputFile = flagVal;
                }
            } else if (flagName == L"visual-capture-scene" || flagName == L"capture-scene" || flagName == L"scene") {
                if (hasVal && !flagVal.empty()) output.options.visualCaptureScene = flagVal;
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
            if (IsValidMediaUrl(arg)) {
                output.options.mediaTargets.push_back(arg);
            } else {
                std::wstring fullPath = ResolveFullPathLong(arg);
                output.options.mediaTargets.push_back(fullPath.empty() ? arg : std::move(fullPath));
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

APERTURE_IPC_INLINE CommandLineParseOutput ParseCommandLine(const std::wstring& cmdLine) {
    if (cmdLine.empty()) return CommandLineParseOutput{};
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv || argc <= 1) {
        if (argv) LocalFree(argv);
        return CommandLineParseOutput{};
    }
    std::vector<std::wstring> args;
    args.reserve(argc - 1);
    for (int i = 1; i < argc; ++i) {
        if (argv[i]) args.push_back(argv[i]);
    }
    LocalFree(argv);
    return ParseCommandLineArgs(args);
}

APERTURE_IPC_INLINE std::vector<uint8_t> SerializeEnvelope(const PlaybackCommandLineOptions& options, Command cmd, uint32_t flags) {
    if (options.mediaTargets.size() > MAX_IPC_FILE_COUNT || options.subtitlePath.size() > MAX_IPC_PATH_CHARS) return {};
    std::vector<uint8_t> payload;
    uint32_t optFlags = 0;
    if (options.startPositionSec.has_value()) optFlags |= OptionFlags::HasStartPos;
    if (options.fullscreen) optFlags |= OptionFlags::Fullscreen;
    if (options.paused) optFlags |= OptionFlags::Paused;
    if (options.loop) optFlags |= OptionFlags::Loop;
    if (options.volume.has_value()) optFlags |= OptionFlags::HasVolume;
    if (!options.subtitlePath.empty()) optFlags |= OptionFlags::HasSubtitle;
    if (options.enqueue) optFlags |= OptionFlags::Enqueue;
    Detail::AppendPod(payload, optFlags);
    Detail::AppendPod(payload, options.startPositionSec.value_or(0.0));
    Detail::AppendPod(payload, options.volume.value_or(100.0));
    Detail::AppendLengthPrefixedWString(payload, options.subtitlePath);
    for (const auto& path : options.mediaTargets) {
        if (path.size() > MAX_IPC_PATH_CHARS) return {};
        Detail::AppendLengthPrefixedWString(payload, path);
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return Detail::FinalizeEnvelope(
        cmd, static_cast<uint32_t>(options.mediaTargets.size()), flags, static_cast<uint64_t>(qpc.QuadPart), payload);
}

APERTURE_IPC_INLINE std::vector<uint8_t> SerializeEnvelope(const std::vector<std::wstring>& paths, Command cmd, uint32_t flags) {
    PlaybackCommandLineOptions opts;
    opts.mediaTargets = paths;
    return SerializeEnvelope(opts, cmd, flags);
}

APERTURE_IPC_INLINE ValidationResult DeserializeEnvelope(const void* data, size_t size,
    PlaybackCommandLineOptions& outOptions, EnvelopeHeader* outHeader, bool authenticateSender) {
    outOptions = {};
    const EnvelopeHeader* header = nullptr;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    const ValidationResult frameStatus = Detail::ValidateEnvelopeFrame(
        data, size, false, false, Command::OpenFiles, true, authenticateSender, false, 0, header, payload, payloadSize);
    if (frameStatus != ValidationResult::Success) return frameStatus;
    size_t offset = 0;
    if (header->version == 2) {
        uint32_t optFlags = 0;
        double startSec = 0.0, vol = 100.0;
        std::wstring subPath;
        if (!Detail::ReadPod(payload, payloadSize, offset, optFlags) ||
            !Detail::ReadPod(payload, payloadSize, offset, startSec) ||
            !Detail::ReadPod(payload, payloadSize, offset, vol)) {
            return ValidationResult::MalformedPayload;
        }
        const ValidationResult subRes = Detail::ReadLengthPrefixedWString(payload, payloadSize, offset, subPath);
        if (subRes != ValidationResult::Success) return subRes;
        if (optFlags & OptionFlags::HasStartPos) {
            if (!std::isfinite(startSec) || startSec < 0.0) return ValidationResult::MalformedPayload;
            outOptions.startPositionSec = startSec;
        }
        outOptions.fullscreen = (optFlags & OptionFlags::Fullscreen) != 0;
        outOptions.paused = (optFlags & OptionFlags::Paused) != 0;
        outOptions.loop = (optFlags & OptionFlags::Loop) != 0;
        if (optFlags & OptionFlags::HasVolume) {
            if (!std::isfinite(vol) || vol < 0.0 || vol > 100.0) return ValidationResult::MalformedPayload;
            outOptions.volume = vol;
        }
        if (optFlags & OptionFlags::HasSubtitle) outOptions.subtitlePath = std::move(subPath);
        outOptions.enqueue = (optFlags & OptionFlags::Enqueue) != 0;
    }
    for (uint32_t i = 0; i < header->fileCount; ++i) {
        std::wstring path;
        const ValidationResult pRes = Detail::ReadLengthPrefixedWString(payload, payloadSize, offset, path);
        if (pRes != ValidationResult::Success) return pRes;
        outOptions.mediaTargets.push_back(std::move(path));
    }
    if (offset != payloadSize) return ValidationResult::MalformedPayload;
    if (outHeader) *outHeader = *header;
    return ValidationResult::Success;
}

APERTURE_IPC_INLINE ValidationResult DeserializeEnvelope(const void* data, size_t size,
    std::vector<std::wstring>& outPaths, EnvelopeHeader* outHeader, bool authenticateSender) {
    PlaybackCommandLineOptions opts;
    const ValidationResult res = DeserializeEnvelope(data, size, opts, outHeader, authenticateSender);
    if (res == ValidationResult::Success) outPaths = std::move(opts.mediaTargets);
    else outPaths.clear();
    return res;
}

APERTURE_IPC_INLINE std::wstring PathsToAppPayload(const std::vector<std::wstring>& paths) {
    std::wstring payload;
    for (const auto& path : paths) {
        if (!path.empty()) { payload += path; payload += L'\n'; }
    }
    return payload;
}

APERTURE_IPC_INLINE std::vector<uint8_t> SerializeAutomationEnvelope(const AutomationCommand& cmd, uint32_t flags) {
    if (cmd.op == AutomationOp::None || static_cast<uint32_t>(cmd.op) > static_cast<uint32_t>(AutomationOp::QueryStatus)) return {};
    if (cmd.op == AutomationOp::Seek) {
        if (!std::isfinite(cmd.seekPositionSec)) return {};
        if (cmd.seekRelative ? (cmd.seekPositionSec < -8640000.0 || cmd.seekPositionSec > 8640000.0)
                             : (cmd.seekPositionSec < 0.0 || cmd.seekPositionSec > 8640000.0)) return {};
    } else if (cmd.op == AutomationOp::Volume) {
        if (!std::isfinite(cmd.volume) || cmd.volume < 0.0 || cmd.volume > 100.0) return {};
    } else if (cmd.op == AutomationOp::Open || cmd.op == AutomationOp::Enqueue) {
        if (cmd.mediaTargets.empty() || cmd.mediaTargets.size() > MAX_IPC_FILE_COUNT) return {};
        if (cmd.op == AutomationOp::Open &&
            (!std::isfinite(cmd.startPositionSec) || cmd.startPositionSec < 0.0 || cmd.startPositionSec > 8640000.0)) return {};
        for (const auto& target : cmd.mediaTargets) {
            if (target.empty() || target.size() > MAX_IPC_PATH_CHARS || target.find(L'\0') != std::wstring::npos) return {};
        }
    }
    std::vector<uint8_t> payload;
    Detail::AppendPod(payload, static_cast<uint32_t>(cmd.op));
    uint32_t autoFlags = 0;
    if (cmd.seekRelative) autoFlags |= AutomationFlags::SeekRelative;
    if (cmd.paused) autoFlags |= AutomationFlags::Paused;
    if (cmd.fullscreen) autoFlags |= AutomationFlags::Fullscreen;
    Detail::AppendPod(payload, autoFlags);
    Detail::AppendPod(payload, cmd.seekPositionSec);
    Detail::AppendPod(payload, cmd.volume);
    Detail::AppendPod(payload, cmd.startPositionSec);
    const uint32_t pathCount = static_cast<uint32_t>(cmd.mediaTargets.size());
    Detail::AppendPod(payload, pathCount);
    for (const auto& path : cmd.mediaTargets) Detail::AppendLengthPrefixedWString(payload, path);
    return Detail::FinalizeEnvelope(Command::Automation, pathCount, flags, GetTickCount64(), payload);
}

APERTURE_IPC_INLINE ValidationResult DeserializeAutomationEnvelope(const void* data, size_t size,
    AutomationCommand& outCmd, EnvelopeHeader* outHeader, bool authenticateSender, bool checkReplay, uint64_t nowMs) {
    outCmd = {};
    const EnvelopeHeader* header = nullptr;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    const ValidationResult frameStatus = Detail::ValidateEnvelopeFrame(
        data, size, true, true, Command::Automation, true, authenticateSender, checkReplay, nowMs, header, payload, payloadSize);
    if (frameStatus != ValidationResult::Success) return frameStatus;
    size_t offset = 0;
    uint32_t opCode = 0, autoFlags = 0, pathCount = 0;
    double seekPos = 0.0, vol = 0.0, startPos = 0.0;
    if (!Detail::ReadPod(payload, payloadSize, offset, opCode) ||
        !Detail::ReadPod(payload, payloadSize, offset, autoFlags) ||
        !Detail::ReadPod(payload, payloadSize, offset, seekPos) ||
        !Detail::ReadPod(payload, payloadSize, offset, vol) ||
        !Detail::ReadPod(payload, payloadSize, offset, startPos) ||
        !Detail::ReadPod(payload, payloadSize, offset, pathCount)) {
        return ValidationResult::MalformedPayload;
    }
    if (opCode == 0 || opCode > static_cast<uint32_t>(AutomationOp::QueryStatus)) return ValidationResult::InvalidOperation;
    outCmd.op = static_cast<AutomationOp>(opCode);
    outCmd.seekRelative = (autoFlags & AutomationFlags::SeekRelative) != 0;
    outCmd.paused = (autoFlags & AutomationFlags::Paused) != 0;
    outCmd.fullscreen = (autoFlags & AutomationFlags::Fullscreen) != 0;
    if (!std::isfinite(seekPos) || !std::isfinite(vol) || !std::isfinite(startPos)) return ValidationResult::InvalidArgument;
    outCmd.seekPositionSec = seekPos;
    outCmd.volume = vol;
    outCmd.startPositionSec = startPos;
    if (pathCount > MAX_IPC_FILE_COUNT || pathCount != header->fileCount) return ValidationResult::FileCountExceeded;
    for (uint32_t i = 0; i < pathCount; ++i) {
        std::wstring path;
        const ValidationResult pRes = Detail::ReadLengthPrefixedWString(payload, payloadSize, offset, path);
        if (pRes != ValidationResult::Success) return pRes;
        if (path.empty() || IsValidMediaUrl(path)) {
            outCmd.mediaTargets.push_back(std::move(path));
        } else {
            std::wstring fullPath = ResolveFullPathLong(path);
            outCmd.mediaTargets.push_back(fullPath.empty() ? std::move(path) : std::move(fullPath));
        }
    }
    if (offset != payloadSize) return ValidationResult::MalformedPayload;
    if (outCmd.op == AutomationOp::Seek) {
        if (outCmd.seekRelative ? (outCmd.seekPositionSec < -8640000.0 || outCmd.seekPositionSec > 8640000.0)
                                : (outCmd.seekPositionSec < 0.0 || outCmd.seekPositionSec > 8640000.0)) {
            return ValidationResult::InvalidArgument;
        }
    } else if (outCmd.op == AutomationOp::Volume) {
        if (outCmd.volume < 0.0 || outCmd.volume > 100.0) return ValidationResult::InvalidArgument;
    } else if (outCmd.op == AutomationOp::Open) {
        if (outCmd.mediaTargets.empty() || outCmd.startPositionSec < 0.0 || outCmd.startPositionSec > 8640000.0)
            return ValidationResult::InvalidArgument;
    } else if (outCmd.op == AutomationOp::Enqueue) {
        if (outCmd.mediaTargets.empty()) return ValidationResult::InvalidArgument;
    }
    if (outHeader) *outHeader = *header;
    return ValidationResult::Success;
}

APERTURE_IPC_INLINE std::vector<uint8_t> SerializeAutomationResponse(const AutomationStatus& status, uint32_t statusResult) {
    if (status.currentFilePath.size() > MAX_IPC_PATH_CHARS) return {};
    std::vector<uint8_t> payload;
    Detail::AppendPod(payload, statusResult);
    Detail::AppendPod(payload, static_cast<uint32_t>(status.state));
    Detail::AppendPod(payload, status.positionSec);
    Detail::AppendPod(payload, status.durationSec);
    Detail::AppendPod(payload, status.volume);
    uint32_t statusFlags = 0;
    if (status.isMuted) statusFlags |= StatusFlags::Muted;
    if (status.isLoop) statusFlags |= StatusFlags::Loop;
    if (status.isFullscreen) statusFlags |= StatusFlags::Fullscreen;
    Detail::AppendPod(payload, statusFlags);
    Detail::AppendLengthPrefixedWString(payload, status.currentFilePath);
    return Detail::FinalizeEnvelope(
        Command::AutomationResponse, status.currentFilePath.empty() ? 0u : 1u, 0, GetTickCount64(), payload);
}

APERTURE_IPC_INLINE ValidationResult DeserializeAutomationResponse(const void* data, size_t size,
    AutomationStatus& outStatus, uint32_t* outStatusResult, EnvelopeHeader* outHeader, bool authenticateSender) {
    outStatus = {};
    if (outStatusResult) *outStatusResult = 1;
    const EnvelopeHeader* header = nullptr;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    const ValidationResult frameStatus = Detail::ValidateEnvelopeFrame(
        data, size, true, true, Command::AutomationResponse, false, authenticateSender, false, 0, header, payload, payloadSize);
    if (frameStatus != ValidationResult::Success) return frameStatus;
    size_t offset = 0;
    uint32_t resCode = 0, stateVal = 0, statusFlags = 0;
    if (!Detail::ReadPod(payload, payloadSize, offset, resCode) ||
        !Detail::ReadPod(payload, payloadSize, offset, stateVal) ||
        !Detail::ReadPod(payload, payloadSize, offset, outStatus.positionSec) ||
        !Detail::ReadPod(payload, payloadSize, offset, outStatus.durationSec) ||
        !Detail::ReadPod(payload, payloadSize, offset, outStatus.volume) ||
        !Detail::ReadPod(payload, payloadSize, offset, statusFlags)) {
        return ValidationResult::MalformedPayload;
    }
    if (outStatusResult) *outStatusResult = resCode;
    outStatus.state = static_cast<AutomationPlaybackState>(stateVal);
    outStatus.isMuted = (statusFlags & StatusFlags::Muted) != 0;
    outStatus.isLoop = (statusFlags & StatusFlags::Loop) != 0;
    outStatus.isFullscreen = (statusFlags & StatusFlags::Fullscreen) != 0;
    const ValidationResult pathRes = Detail::ReadLengthPrefixedWString(payload, payloadSize, offset, outStatus.currentFilePath);
    if (pathRes != ValidationResult::Success) return pathRes;
    if (offset != payloadSize) return ValidationResult::MalformedPayload;
    if (outHeader) *outHeader = *header;
    return ValidationResult::Success;
}

APERTURE_IPC_INLINE bool SendAutomationCommand(
    HWND hTargetWnd, const AutomationCommand& cmd, AutomationStatus* outStatus, DWORD timeoutMs) {
    if (!hTargetWnd || !IsWindow(hTargetWnd)) return false;
    DWORD targetPid = 0;
    GetWindowThreadProcessId(hTargetWnd, &targetPid);
    if (!IsSameUserAndSession(targetPid)) return false;
    std::vector<uint8_t> envelope = SerializeAutomationEnvelope(cmd);
    if (envelope.empty()) return false;
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
                    const ValidationResult vRes = DeserializeAutomationResponse(
                        pCDS->lpData, pCDS->cbData, *ctx->pStatus, &resCode, nullptr, true);
                    if (vRes == ValidationResult::Success && resCode == 0) { ctx->received = true; return TRUE; }
                }
                return FALSE;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"Aperture_Automation_Reply_Wnd";
        RegisterClassExW(&wc);
        hReplyWnd = CreateWindowExW(
            0, L"Aperture_Automation_Reply_Wnd", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
        if (hReplyWnd) SetWindowLongPtrW(hReplyWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&replyCtx));
    }
    COPYDATASTRUCT cds = {};
    cds.dwData = IPC_MAGIC_V2;
    cds.cbData = static_cast<DWORD>(envelope.size());
    cds.lpData = envelope.data();
    DWORD_PTR dwResult = 0;
    const BOOL sent = SendMessageTimeoutW(
        hTargetWnd, WM_COPYDATA, reinterpret_cast<WPARAM>(hReplyWnd), reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &dwResult);
    if (hReplyWnd) {
        if (replyCtx.received && outStatus) *outStatus = receivedStatus;
        DestroyWindow(hReplyWnd);
        UnregisterClassW(L"Aperture_Automation_Reply_Wnd", GetModuleHandleW(NULL));
    }
    return sent && dwResult != 0;
}

} // namespace Ipc
} // namespace Aperture
