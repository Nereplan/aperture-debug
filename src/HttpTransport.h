#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>

#pragma comment(lib, "winhttp.lib")

namespace Aperture {

enum class HttpTransportStatus {
    Success = 0, InvalidUrl, HttpsRequired, NetworkError, Timeout, TooManyRedirects, ResponseTooLarge, HttpError, Cancelled,
    DestinationRejected, FileError
};

struct HttpRequestOptions {
    size_t maxResponseBytes = 150 * 1024 * 1024; // 150 MB default ceiling
    int timeoutMs = 5000;                       // 5 seconds default timeout
    int maxRedirects = 3;                       // Max 3 redirects
    std::wstring userAgent = L"Aperture/2.0";
    std::vector<std::wstring> allowedHosts; // Exact, case-insensitive destinations, including every redirect hop.
    std::function<bool()> isCancelled;
};

struct HttpResponse {
    bool success = false;
    int statusCode = 0;
    std::string body;
    std::wstring finalUrl, errorMessage;
    HttpTransportStatus status = HttpTransportStatus::Success;
};

class IHttpTransport {
public:
    virtual ~IHttpTransport() = default;
    virtual HttpResponse Get(const std::wstring& httpsUrl, const HttpRequestOptions& options = {}) = 0;
    virtual HttpResponse GetToFile(const std::wstring& httpsUrl, const std::wstring& destination, const HttpRequestOptions& options = {});
};

class WinHttpTransport : public IHttpTransport {
public:
    WinHttpTransport() = default;
    ~WinHttpTransport() override = default;
    HttpResponse Get(const std::wstring& httpsUrl, const HttpRequestOptions& options = {}) override;
    HttpResponse GetToFile(const std::wstring& httpsUrl, const std::wstring& destination, const HttpRequestOptions& options = {}) override;
};

class MockHttpTransport : public IHttpTransport {
public:
    using MockHandler = std::function<HttpResponse(const std::wstring& url, const HttpRequestOptions& options)>;
    MockHttpTransport() = default;
    ~MockHttpTransport() override = default;
    void SetResponse(const std::wstring& url, const HttpResponse& response);
    void SetHandler(const std::wstring& urlPrefix, MockHandler handler);
    void Clear();
    HttpResponse Get(const std::wstring& httpsUrl, const HttpRequestOptions& options = {}) override;
private:
    mutable std::mutex mockMutex_;
    std::map<std::wstring, HttpResponse> responses_;
    std::vector<std::pair<std::wstring, MockHandler>> handlers_;
    HttpResponse defaultResponse_ = { false, 404, "", L"", L"Not Found", HttpTransportStatus::HttpError };
    std::vector<std::wstring> requestedUrls_;
};
} // namespace Aperture
