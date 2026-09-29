#pragma once

#include "Common.h"
#include "Json.h"
#include "TextInputModel.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

namespace Aperture {

class MpvPlayer;

enum class NetworkStreamScheme { Unknown = 0, Http, Https, Rtsp, Rtmp };

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
    bool allowHttp = true, allowHttps = true, allowRtsp = true, allowRtmp = true;
};

class NetworkStreamValidator {
public:
    static NetworkStreamUrl ValidateAndParse( const std::wstring& rawUrl, const NetworkStreamValidationOptions& options = {});
    static bool IsValid(const std::wstring& rawUrl, const NetworkStreamValidationOptions& options = {}) {
        return ValidateAndParse(rawUrl, options).isValid;
    }
    static std::wstring RedactUrl(const std::wstring& rawUrl);
    static std::wstring GetSafeErrorMessage(NetworkStreamValidationError error, const std::wstring& rawUrl = L"");
    static std::string GetSafeErrorMessageUtf8(NetworkStreamValidationError error, const std::string& rawUrl = "");
    static bool IsAllowedScheme(const std::wstring& scheme, const NetworkStreamValidationOptions& options = {});
    static bool IsValidHost(const std::wstring& host);
    static bool IsValidIPv4(const std::wstring& host);
    static bool IsValidIPv6(const std::wstring& host);
    static bool IsValidDomain(const std::wstring& host);
    static bool ContainsControlCharacters(const std::wstring& text);
    // Dropped Text and OLE Payload Extraction & Validation
    static std::wstring ExtractCandidateUrl(const std::wstring& rawText);
    static std::string ExtractCandidateUrlUtf8(const std::string& rawText);
    static DroppedUrlResult ExtractAndValidateDroppedText( const std::wstring& rawText, const NetworkStreamValidationOptions& options = {});
    static DroppedUrlResult ExtractAndValidateDroppedTextUtf8( const std::string& rawText,
        const NetworkStreamValidationOptions& options = {});
    static DroppedUrlResult ExtractAndValidateFromDataObject( IDataObject* pDataObj, const NetworkStreamValidationOptions& options = {});
};

// Recent stream item for root-local persistence (when explicitly enabled)
struct RecentStreamEntry { std::wstring url; std::wstring title; int64_t lastPlayedTimestamp = 0; };

struct NetworkStreamConfig {
    bool enableRecentStreams = false; // explicitly disabled by default
    size_t maxRecentStreams = 20;
    NetworkStreamValidationOptions validationOptions;
};

// Immutable projection for rendering & testing
struct NetworkStreamProjection {
    bool isOpen = false, isOpening = false;
    std::wstring currentInput, redactedDisplayUrl, lastError;
    bool isValidInput = false, recentStreamsEnabled = false;
    std::vector<RecentStreamEntry> recentStreams;
    int selectedRecentIndex = -1;
};

// Async Open State & Coordinator with cancellation and generation tracking
class NetworkStreamLoader {
public:
    using OpenCallback = std::function<void(bool success, const std::wstring& safeUrl, const std::wstring& errorMessage, uint64_t generation)>;
    NetworkStreamLoader();
    ~NetworkStreamLoader();
    // Starts async open. Returns generation ID.
    uint64_t BeginOpen( const std::wstring& validatedUrl, MpvPlayer* player, OpenCallback onComplete);
    // Cancel in-flight open and invalidate pending callbacks
    void Cancel();
    // Check if an open is currently active
    bool IsOpening() const;
    uint64_t GetActiveGeneration() const;

private:
    struct SharedState {
        std::atomic<uint64_t> activeGeneration{0};
        std::atomic<bool> isOpening{false};
        std::atomic<bool> cancelled{false};
    };
    std::shared_ptr<SharedState> state_;
    std::atomic<uint64_t> activeGeneration_{0};
    std::thread workerThread_;
    mutable std::mutex loaderMutex_;
};

// Main state model managing input, validation, async lifecycle, and optional root-local persistence
class NetworkStreamModel {
public:
    static constexpr int kCurrentStreamSchemaVersion = 1;
    static constexpr const wchar_t* kDefaultRecentStreamsFileName = L"recent_streams.json";
    explicit NetworkStreamModel(const NetworkStreamConfig& config = {});
    ~NetworkStreamModel();
    // Dialog state
    void Open();
    void Close();
    void Toggle();
    bool IsOpen() const;
    // Text Input Model & Editing
    TextInputModel& GetTextInputModel() { return textInput_; }
    const TextInputModel& GetTextInputModel() const { return textInput_; }
    void SetInput(const std::wstring& url);
    std::wstring GetInput() const;
    // Validation & State
    bool IsValid() const;
    std::wstring GetLastError() const;
    // Configuration & Recent Streams
    bool IsRecentStreamsEnabled() const;
    std::vector<RecentStreamEntry> GetRecentStreams() const;
    void AddRecentStream(const std::wstring& url, const std::wstring& title = L"");
    // Persistence (strictly root-local, only if enableRecentStreams == true)
    StorageStatus LoadRecentStreams(const std::wstring& baseDir = GetExeDir(), const std::wstring& fileName = kDefaultRecentStreamsFileName);
    StorageStatus SaveRecentStreams(const std::wstring& baseDir = GetExeDir(), const std::wstring& fileName = kDefaultRecentStreamsFileName);
    // Async Opening
    uint64_t OpenStreamAsync( MpvPlayer* player,
        std::function<void(bool success, const std::wstring& displayUrl, const std::wstring& errorMsg)> onComplete);
    void CancelOpen();
    bool IsOpening() const { return loader_.IsOpening(); }
    // Keyboard & Selection in Recent Streams
    // Projection
    NetworkStreamProjection GetProjection() const;

private:
    mutable std::mutex modelMutex_;
    NetworkStreamConfig config_;
    TextInputModel textInput_;
    NetworkStreamUrl validatedUrl_;
    std::wstring lastError_;
    bool isOpen_ = false;
    int selectedRecentIndex_ = -1;
    std::vector<RecentStreamEntry> recentStreams_;
    NetworkStreamLoader loader_;
    void Revalidate();
    void RevalidateLocked();
    void AddRecentStreamLocked(const std::wstring& url, const std::wstring& title = L"");
};

} // namespace Aperture
