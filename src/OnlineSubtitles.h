#pragma once

#include "Common.h"
#include "HttpTransport.h"
#include "Json.h"
#include "Subtitles.h"
#include "SubtitleDiscovery.h"
#include "Settings.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include <map>

#if defined(_WIN32)
#pragma comment(lib, "winhttp.lib")
#endif

namespace Aperture {

enum class OnlineSubtitleStatus {
    Success = 0, Disabled, EmptyQuery, InvalidEndpoint, HttpsRequired, InvalidUrl, NetworkError, Timeout, TooManyRedirects, HttpError,
    ResponseTooLarge, InvalidResponseFormat, NoResultsFound, InvalidSubtitleEncoding, InvalidSubtitleContent, StorageError, Cancelled
};

struct OnlineSubtitleSearchResult {
    std::string id;
    std::wstring title, language;
    std::string downloadUrl;
    std::string format = "srt";
    int score = 0;
    uint64_t fileSizeBytes = 0;
};

inline bool operator==(HttpTransportStatus h, OnlineSubtitleStatus s) {
    switch (s) {
        case OnlineSubtitleStatus::Success: return h == HttpTransportStatus::Success;
        case OnlineSubtitleStatus::InvalidUrl: return h == HttpTransportStatus::InvalidUrl;
        case OnlineSubtitleStatus::HttpsRequired: return h == HttpTransportStatus::HttpsRequired;
        case OnlineSubtitleStatus::NetworkError: return h == HttpTransportStatus::NetworkError;
        case OnlineSubtitleStatus::Timeout: return h == HttpTransportStatus::Timeout;
        case OnlineSubtitleStatus::TooManyRedirects: return h == HttpTransportStatus::TooManyRedirects;
        case OnlineSubtitleStatus::ResponseTooLarge: return h == HttpTransportStatus::ResponseTooLarge;
        case OnlineSubtitleStatus::HttpError: return h == HttpTransportStatus::HttpError;
        case OnlineSubtitleStatus::Cancelled: return h == HttpTransportStatus::Cancelled;
        default: return false;
    }
}
inline bool operator==(OnlineSubtitleStatus s, HttpTransportStatus h) { return h == s; }
inline bool operator!=(HttpTransportStatus h, OnlineSubtitleStatus s) { return !(h == s); }
inline bool operator!=(OnlineSubtitleStatus s, HttpTransportStatus h) { return !(h == s); }

struct OnlineSubtitleResult {
    bool success = false;
    OnlineSubtitleStatus status = OnlineSubtitleStatus::Success;
    int httpStatusCode = 0;
    std::wstring errorMessage, savedFilePath;
    std::vector<OnlineSubtitleSearchResult> searchResults;
};

class OnlineSubtitleService {
public:
    static constexpr const char* kDefaultEndpointTemplate = "https://api.opensubtitles.com/api/v1/subtitles?query={query}&languages={lang}";
    static constexpr size_t kMaxQueryLength = 128, kMaxSearchResults = 50;
    static constexpr size_t kMaxSubtitleFileBytes = 4 * 1024 * 1024; // 4 MB
    OnlineSubtitleService();
    ~OnlineSubtitleService() = default;
    static OnlineSubtitleService& Instance();
    // Query derivation: conservative cleaning of media filename / title
    static std::wstring DeriveSearchQuery(const std::wstring& mediaPath, const std::wstring& overrideTitle = L"");
    // Endpoint and URL validation
    static bool IsValidEndpointTemplate(const std::string& endpointTemplate);
    static std::wstring BuildSearchUrl(const std::string& endpointTemplate, const std::wstring& query, const std::string& lang);
    static std::string UrlEncodeUtf8(const std::wstring& text);
    // Response parsing
    static std::vector<OnlineSubtitleSearchResult> ParseSearchResultsJson(const std::string& jsonText, const std::wstring& preferredLang = L"");
    // Content validation
    static bool ValidateSubtitleContent(const std::string& rawBytes, std::string& outNormalizedText, SubtitleEncoding& outEncoding);
    // Atomic storage under root directory
    static StorageStatus SaveSubtitleAtomically( const std::string& validatedContent, const std::wstring& queryTitle,
        const std::wstring& langCode, const std::string& idOrHash, const std::string& formatExt, std::wstring& outSavedPath,
        const std::wstring& rootDir = GetExeDir());
    // High-level operations
    OnlineSubtitleResult Search( const std::wstring& mediaPath, const std::wstring& overrideTitle, const Settings& settings,
        IHttpTransport* customTransport = nullptr, std::shared_ptr<OperationCancellation> cancellation = {});
    OnlineSubtitleResult DownloadAndLoad( const OnlineSubtitleSearchResult& item, const std::wstring& mediaPath, const Settings& settings,
        IHttpTransport* customTransport = nullptr, const std::wstring& rootDir = GetExeDir(),
        std::shared_ptr<OperationCancellation> cancellation = {});

private:
    std::unique_ptr<IHttpTransport> defaultTransport_;
};

} // namespace Aperture
