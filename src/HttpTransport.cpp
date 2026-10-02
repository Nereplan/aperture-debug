#include "HttpTransport.h"
#include "HttpBodyReader.h"
#include "WinHttpAsyncRequest.h"
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>
#include <cctype>
#include <optional>

namespace Aperture {
namespace {
struct InternetCloser { void operator()(HINTERNET handle) const { if (handle) WinHttpCloseHandle(handle); } };
using InternetHandle = std::unique_ptr<void, InternetCloser>;
using FileHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

HttpResponse HttpFailure(HttpTransportStatus status, const std::wstring& message, const std::wstring& url) {
    HttpResponse result; result.status = status; result.errorMessage = message; result.finalUrl = url; return result;
}
bool ParseDestination(const std::wstring& url, const HttpRequestOptions& options, URL_COMPONENTS& components, HttpResponse& result) {
    if (url.empty() || url.size() > 32768 || std::any_of(url.begin(), url.end(), [](wchar_t c) { return c < 32 || c == 127; })) {
        result = HttpFailure(HttpTransportStatus::InvalidUrl, L"Invalid HTTPS URL", url); return false;
    }
    components = {}; components.dwStructSize = sizeof(components);
    components.dwSchemeLength = components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = DWORD(-1);
    components.dwUserNameLength = components.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) || !components.dwHostNameLength) {
        result = HttpFailure(HttpTransportStatus::InvalidUrl, L"Malformed URL structure", url); return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        result = HttpFailure(HttpTransportStatus::HttpsRequired, L"HTTPS is strictly required", url); return false;
    }
    if (components.dwUserNameLength || components.dwPasswordLength) {
        result = HttpFailure(HttpTransportStatus::InvalidUrl, L"Embedded URL credentials are not supported", url); return false;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    if (!options.allowedHosts.empty() && !std::any_of(options.allowedHosts.begin(), options.allowedHosts.end(), [&](const auto& allowed) {
        return CompareStringOrdinal(host.c_str(), -1, allowed.c_str(), -1, TRUE) == CSTR_EQUAL;
    })) { result = HttpFailure(HttpTransportStatus::DestinationRejected, L"HTTPS destination is not allowed", url); return false; }
    return true;
}
bool QueryHeaderString(HINTERNET request, DWORD query, std::wstring& output) {
    DWORD bytes = 0;
    if (WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 64 * 1024) return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, 0);
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return false;
    output.assign(buffer.data()); return true;
}
HttpResponse PerformGet(const std::wstring& initialUrl, const HttpRequestOptions& options, const HttpDetail::BodySink& sink = {}) {
    if (options.IsCancelled()) return HttpFailure(HttpTransportStatus::Cancelled, L"Request cancelled", initialUrl);
    if (!SystemDllPresent(L"winhttp.dll")) return HttpFailure(HttpTransportStatus::NetworkError, L"WinHTTP is unavailable", initialUrl);
    URL_COMPONENTS components{}; HttpResponse result;
    if (!ParseDestination(initialUrl, options, components, result)) return result;
    InternetHandle session(WinHttpOpen(options.userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, options.cancellation ? WINHTTP_FLAG_ASYNC : 0));
    const auto networkFailure = [&](const wchar_t* operation, const std::wstring& url) {
        const DWORD error = GetLastError();
        return HttpFailure(options.IsCancelled() ? HttpTransportStatus::Cancelled :
                           error == ERROR_WINHTTP_TIMEOUT ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError,
                           std::wstring(operation) + L": error " + std::to_wstring(error), url);
    };
    if (!session) return networkFailure(L"Failed to initialize HTTPS session", initialUrl);
    const int timeout = std::clamp(options.timeoutMs, 1, 300000);
    if (!WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) return networkFailure(L"Failed to configure HTTPS timeouts", initialUrl);
    std::wstring currentUrl = initialUrl;
    const int maxRedirects = std::clamp(options.maxRedirects, 0, 20);
    for (int redirects = 0;; ++redirects) {
        if (options.IsCancelled()) return HttpFailure(HttpTransportStatus::Cancelled, L"Request cancelled", currentUrl);
        if (!ParseDestination(currentUrl, options, components, result)) return result;
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
        if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        const auto fragment = path.find(L'#'); if (fragment != std::wstring::npos) path.resize(fragment);
        if (path.empty()) path = L"/";
        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (!connection) return networkFailure(L"Failed to connect to HTTPS host", currentUrl);
        InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request) return networkFailure(L"Failed to open HTTPS request", currentUrl);
        // Follow redirects explicitly so neither the count nor destination policy
        // can be bypassed by WinHTTP's automatic redirect handling.
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
            return networkFailure(L"Failed to configure redirect policy", currentUrl);
        std::unique_ptr<HttpDetail::AsyncRequest> asyncRequest;
        if (options.cancellation) {
            asyncRequest = std::make_unique<HttpDetail::AsyncRequest>(request.release(), options.cancellation);
            if (!asyncRequest->IsReady()) return networkFailure(L"Failed to configure asynchronous HTTPS request", currentUrl);
        }
        const auto use = [&](const auto& operation) { return asyncRequest ? asyncRequest->Use(operation) : operation(request.get()) != FALSE; };
        const auto send = [&](HINTERNET handle) { return WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
            asyncRequest ? asyncRequest->Context() : 0); };
        if (!(asyncRequest ? asyncRequest->Call(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, send) : use(send)))
            return networkFailure(L"Failed to send HTTPS request", currentUrl);
        const auto receive = [](HINTERNET handle) { return WinHttpReceiveResponse(handle, nullptr); };
        if (!(asyncRequest ? asyncRequest->Call(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, receive) : use(receive)))
            return networkFailure(L"Failed to receive HTTPS response", currentUrl);
        DWORD code = 0, codeSize = sizeof(code);
        if (!use([&](HINTERNET handle) { return WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX); })) return networkFailure(L"Failed to read HTTP status", currentUrl);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            if (redirects >= maxRedirects) return HttpFailure(HttpTransportStatus::TooManyRedirects, L"HTTPS redirect limit exceeded", currentUrl);
            std::wstring location;
            if (!use([&](HINTERNET handle) { return QueryHeaderString(handle, WINHTTP_QUERY_LOCATION, location); }) || location.empty())
                return HttpFailure(HttpTransportStatus::InvalidUrl, L"Missing or invalid redirect destination", currentUrl);
            std::vector<wchar_t> combined(32769, 0); DWORD combinedSize = static_cast<DWORD>(combined.size());
            if (FAILED(UrlCombineW(currentUrl.c_str(), location.c_str(), combined.data(), &combinedSize, 0)))
                return HttpFailure(HttpTransportStatus::InvalidUrl, L"Invalid redirect destination", currentUrl);
            currentUrl.assign(combined.data()); continue;
        }
        result = {}; result.finalUrl = currentUrl; result.statusCode = static_cast<int>(code);
        if (code < 200 || code >= 300) {
            result.status = HttpTransportStatus::HttpError; result.errorMessage = L"HTTP request returned status " + std::to_wstring(code); return result;
        }
        std::wstring length; std::optional<uint64_t> expected;
        if (use([&](HINTERNET handle) { return QueryHeaderString(handle, WINHTTP_QUERY_CONTENT_LENGTH, length); })) {
            try { size_t consumed = 0; auto value = std::stoull(length, &consumed);
                  if (consumed == length.size() && !length.empty() && length.front() != L'-') expected = value;
                  else return HttpFailure(HttpTransportStatus::NetworkError, L"Invalid response content length", currentUrl);
            } catch (...) { return HttpFailure(HttpTransportStatus::NetworkError, L"Invalid response content length", currentUrl); }
        }
        HttpDetail::ReadBody(result, options, [&](char* buffer, DWORD capacity, DWORD& bytes, DWORD& error) {
            const auto read = [&](HINTERNET handle) { return WinHttpReadData(handle, buffer, capacity, asyncRequest ? nullptr : &bytes); };
            if (asyncRequest ? asyncRequest->Call(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, read, &bytes) : use(read)) return true;
            error = GetLastError(); return false;
        }, sink, expected);
        return result;
    }
}
HttpResponse DownloadFile(const std::wstring& url, const std::wstring& destination, const HttpRequestOptions& options,
                          const std::function<HttpResponse(const HttpDetail::BodySink&)>& download) {
    if (options.IsCancelled()) return HttpFailure(HttpTransportStatus::Cancelled, L"Request cancelled", url);
    FileHandle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr), &CloseHandle);
    if (output.get() == INVALID_HANDLE_VALUE) {
        output.release(); return HttpFailure(HttpTransportStatus::FileError, L"Failed to create download staging file", url);
    }
    auto result = download([&](const char* bytes, DWORD count) {
        DWORD written = 0; return WriteFile(output.get(), bytes, count, &written, nullptr) && written == count;
    });
    if (options.IsCancelled()) result = HttpFailure(HttpTransportStatus::Cancelled, L"Request cancelled", result.finalUrl);
    if (result.success && !FlushFileBuffers(output.get())) {
        result.success = false; result.status = HttpTransportStatus::FileError; result.errorMessage = L"Failed to flush download staging file";
    }
    output.reset();
    if (!result.success) DeleteFileW(destination.c_str());
    return result;
}
} // namespace

HttpResponse WinHttpTransport::Get(const std::wstring& url, const HttpRequestOptions& options) { return PerformGet(url, options); }
HttpResponse WinHttpTransport::GetToFile(const std::wstring& url, const std::wstring& destination, const HttpRequestOptions& options) {
    return DownloadFile(url, destination, options, [&](const auto& sink) { return PerformGet(url, options, sink); });
}
HttpResponse IHttpTransport::GetToFile(const std::wstring& url, const std::wstring& destination, const HttpRequestOptions& options) {
    // Compatibility for injected transports; the production WinHTTP override
    // streams directly and never constructs a package-sized string.
    return DownloadFile(url, destination, options, [&](const auto& sink) {
        auto result = Get(url, options);
        if (result.success) {
            URL_COMPONENTS components{}; HttpResponse policyFailure;
            const auto finalUrl = result.finalUrl.empty() ? url : result.finalUrl;
            if (!ParseDestination(finalUrl, options, components, policyFailure)) return policyFailure;
            if (result.body.size() > options.maxResponseBytes)
                return HttpFailure(HttpTransportStatus::ResponseTooLarge, L"Response size exceeded configured ceiling", finalUrl);
            size_t offset = 0;
            while (offset < result.body.size()) {
                if (options.IsCancelled()) {
                    result.success = false; result.status = HttpTransportStatus::Cancelled; result.errorMessage = L"Request cancelled"; break;
                }
                const DWORD bytes = static_cast<DWORD>(std::min<size_t>(64 * 1024, result.body.size() - offset));
                if (!sink(result.body.data() + offset, bytes)) {
                    result.success = false; result.status = HttpTransportStatus::FileError; result.errorMessage = L"Failed to write downloaded response"; break;
                }
                offset += bytes;
            }
        }
        result.body.clear(); result.body.shrink_to_fit(); return result;
    });
}

// --- MockHttpTransport Implementation ---

void MockHttpTransport::SetResponse(const std::wstring& url, const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mockMutex_);
    responses_[url] = response;
}

void MockHttpTransport::SetHandler(const std::wstring& urlPrefix, MockHandler handler) {
    std::lock_guard<std::mutex> lock(mockMutex_);
    handlers_.push_back({ urlPrefix, handler });
}

void MockHttpTransport::Clear() {
    std::lock_guard<std::mutex> lock(mockMutex_);
    responses_.clear();
    handlers_.clear();
    requestedUrls_.clear();
}

HttpResponse MockHttpTransport::Get(const std::wstring& httpsUrl, const HttpRequestOptions& options) {
    std::vector<std::pair<std::wstring, MockHandler>> handlersCopy;
    HttpResponse matchedResponse;
    bool hasDirectMatch = false;
    HttpResponse fallbackResponse;
    {
        std::lock_guard<std::mutex> lock(mockMutex_);
        requestedUrls_.push_back(httpsUrl);
        if (httpsUrl.rfind(L"https://", 0) != 0) {
            HttpResponse res;
            res.success = false;
            res.status = HttpTransportStatus::HttpsRequired;
            res.errorMessage = L"HTTPS is strictly required";
            return res;
        }
        auto it = responses_.find(httpsUrl);
        if (it != responses_.end()) {
            matchedResponse = it->second;
            hasDirectMatch = true;
        } else { handlersCopy = handlers_; fallbackResponse = defaultResponse_; }
    }
    if (!hasDirectMatch) {
        bool handled = false;
        for (const auto& pair : handlersCopy) {
            if (httpsUrl.rfind(pair.first, 0) == 0) { matchedResponse = pair.second(httpsUrl, options); handled = true; break; }
        }
        if (!handled) { matchedResponse = fallbackResponse; }
    }
    if (matchedResponse.finalUrl.empty()) matchedResponse.finalUrl = httpsUrl;
    if (matchedResponse.success) {
        URL_COMPONENTS components{}; HttpResponse policyFailure;
        if (!ParseDestination(matchedResponse.finalUrl, options, components, policyFailure)) return policyFailure;
    }
    if (matchedResponse.body.size() > options.maxResponseBytes) {
        matchedResponse.success = false;
        matchedResponse.status = HttpTransportStatus::ResponseTooLarge;
        matchedResponse.errorMessage = L"Response size exceeded configured ceiling";
        matchedResponse.body.clear();
        matchedResponse.body.shrink_to_fit();
    }
    return matchedResponse;
}

} // namespace Aperture
