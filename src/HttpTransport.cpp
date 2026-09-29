#include "HttpTransport.h"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>

namespace Aperture {

// --- WinHttpTransport Implementation ---

HttpResponse WinHttpTransport::Get(const std::wstring& httpsUrl, const HttpRequestOptions& options) {
    HttpResponse response;
    response.finalUrl = httpsUrl;
    // WinHTTP is delay-loaded: probe system-only presence before touching any
    // delay import so a missing DLL yields NetworkError, never an exception.
    if (!SystemDllPresent(L"winhttp.dll")) {
        response.status = HttpTransportStatus::NetworkError;
        response.errorMessage = L"WinHTTP is unavailable on this system";
        return response;
    }
    if (httpsUrl.empty()) { response.status = HttpTransportStatus::InvalidUrl; response.errorMessage = L"URL is empty"; return response; }
    URL_COMPONENTS urlComp = { 0 };
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwExtraInfoLength = (DWORD)-1;
    std::wstring mutableUrl = httpsUrl;
    if (!WinHttpCrackUrl(mutableUrl.c_str(), (DWORD)mutableUrl.length(), 0, &urlComp)) {
        response.status = HttpTransportStatus::InvalidUrl;
        response.errorMessage = L"Malformed URL structure";
        return response;
    }
    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
        response.status = HttpTransportStatus::HttpsRequired;
        response.errorMessage = L"HTTPS is strictly required";
        return response;
    }
    std::wstring hostName(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring urlPath(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    if (urlComp.dwExtraInfoLength > 0 && urlComp.lpszExtraInfo) { urlPath.append(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength); }
    INTERNET_PORT port = urlComp.nPort;
    HINTERNET hSession = WinHttpOpen( options.userAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        DWORD err = GetLastError();
        response.status = HttpTransportStatus::NetworkError;
        response.errorMessage = L"Failed to initialize WinHTTP session: error " + std::to_wstring(err);
        return response;
    }
    WinHttpSetTimeouts(hSession, options.timeoutMs, options.timeoutMs, options.timeoutMs, options.timeoutMs);
    HINTERNET hConnect = WinHttpConnect(hSession, hostName.c_str(), port, 0);
    if (!hConnect) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hSession);
        response.status = (err == ERROR_WINHTTP_TIMEOUT) ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError;
        response.errorMessage = L"Failed to connect to host: error " + std::to_wstring(err);
        return response;
    }
    HINTERNET hRequest = WinHttpOpenRequest( hConnect, L"GET", urlPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.status = HttpTransportStatus::NetworkError;
        response.errorMessage = L"Failed to open HTTPS request: error " + std::to_wstring(err);
        return response;
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    BOOL sent = WinHttpSendRequest( hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.status = (err == ERROR_WINHTTP_TIMEOUT) ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError;
        response.errorMessage = L"Failed to send HTTPS request: error " + std::to_wstring(err);
        return response;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.status = (err == ERROR_WINHTTP_TIMEOUT) ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError;
        response.errorMessage = L"Failed to receive HTTPS response: error " + std::to_wstring(err);
        return response;
    }
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders( hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
        &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = static_cast<int>(statusCode);
    if (statusCode < 200 || statusCode >= 300) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.status = HttpTransportStatus::HttpError;
        response.errorMessage = L"HTTP request returned status code " + std::to_wstring(statusCode);
        return response;
    }
    std::string responseBody;
    DWORD bytesAvailable = 0;
    size_t totalBytesRead = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (totalBytesRead + bytesAvailable > options.maxResponseBytes) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            response.status = HttpTransportStatus::ResponseTooLarge;
            response.errorMessage = L"Response size exceeded configured ceiling";
            return response;
        }
        std::vector<char> buffer(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
            responseBody.append(buffer.data(), bytesRead);
            totalBytesRead += bytesRead;
        } else { break; }
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    response.success = true;
    response.body = responseBody;
    response.status = HttpTransportStatus::Success;
    return response;
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
