#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

namespace Aperture {

enum class NetworkStreamScheme { Unknown = 0, Http, Https, Rtsp, Rtmp, Udp, Rtp };

enum class NetworkStreamValidationError {
    None = 0, EmptyUrl, UrlTooLong, ContainsControlCharacters, UnsupportedScheme, MissingSchemeDelimiter, InvalidHost, EmptyHost,
    InvalidPort, EmbeddedCredentialsNotAllowed, InvalidCharacters
};

struct NetworkStreamUrl {
    std::wstring rawUrl;
    NetworkStreamScheme scheme = NetworkStreamScheme::Unknown;
    std::wstring schemeStr, host;
    int port = 0; // 0 if not specified
    std::wstring path, query, fragment, username;
    std::wstring password;
    bool hasCredentials = false, isValid = false;
    NetworkStreamValidationError error = NetworkStreamValidationError::None;
    std::wstring GetDisplayUrl(bool redactCredentials = true) const;
    std::wstring GetRedactedUrl() const { return GetDisplayUrl(true); }
};

struct DroppedUrlResult { bool hasUrl = false; std::wstring extractedUrl; NetworkStreamUrl parsedUrl; std::wstring safeErrorMessage; };

struct NetworkStreamValidationOptions {
    size_t maxLength = 2048;
    bool allowCredentials = false; // default: no embedded credentials
    bool allowHttp = true, allowHttps = true, allowRtsp = true, allowRtmp = true, allowUdp = true, allowRtp = true;
};

class NetworkStreamValidator {
public:
    static NetworkStreamUrl ValidateAndParse( const std::wstring& rawUrl, const NetworkStreamValidationOptions& options = {});
    static bool IsValid(const std::wstring& rawUrl, const NetworkStreamValidationOptions& options = {}) {
        return ValidateAndParse(rawUrl, options).isValid;
    }
    static std::wstring RedactUrl(const std::wstring& rawUrl);
    static std::wstring GetSafeErrorMessage(NetworkStreamValidationError error, const std::wstring& rawUrl = L"");
    static bool IsAllowedScheme(const std::wstring& scheme, const NetworkStreamValidationOptions& options = {});
    static bool IsValidHost(const std::wstring& host);
    static bool IsValidIPv4(const std::wstring& host);
    static bool IsValidIPv6(const std::wstring& host);
    static bool IsValidDomain(const std::wstring& host);
    static bool ContainsControlCharacters(const std::wstring& text);
    // Dropped Text and OLE Payload Extraction & Validation
    static std::wstring ExtractCandidateUrl(const std::wstring& rawText);
    static DroppedUrlResult ExtractAndValidateDroppedText( const std::wstring& rawText, const NetworkStreamValidationOptions& options = {});
    static DroppedUrlResult ExtractAndValidateFromDataObject( IDataObject* pDataObj, const NetworkStreamValidationOptions& options = {});
};

// Recent stream item for root-local persistence (when explicitly enabled)
struct RecentStreamEntry { std::wstring url; std::wstring title; int64_t lastPlayedTimestamp = 0; };

struct NetworkStreamConfig {
    bool enableRecentStreams = false; // explicitly disabled by default
    size_t maxRecentStreams = 20;
    NetworkStreamValidationOptions validationOptions;
};

// Optional root-local recent-stream persistence. Playback and its input dialog
// are owned by App and MpvPlayer.
class NetworkStreamModel {
public:
    static constexpr int kCurrentStreamSchemaVersion = 1;
    static constexpr const wchar_t* kDefaultRecentStreamsFileName = L"recent_streams.json";
    explicit NetworkStreamModel(const NetworkStreamConfig& config = {}) : config_(config) {}
    bool IsRecentStreamsEnabled() const { return config_.enableRecentStreams; }
    void AddRecentStream(const std::wstring& url, const std::wstring& title = L"");
    StorageStatus LoadRecentStreams(const std::wstring& baseDir = GetExeDir(), const std::wstring& fileName = kDefaultRecentStreamsFileName);
    StorageStatus SaveRecentStreams(const std::wstring& baseDir = GetExeDir(), const std::wstring& fileName = kDefaultRecentStreamsFileName);

private:
    std::mutex modelMutex_;
    const NetworkStreamConfig config_;
    std::vector<RecentStreamEntry> recentStreams_;
};

} // namespace Aperture
