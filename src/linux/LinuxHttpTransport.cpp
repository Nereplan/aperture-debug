#if !defined(_WIN32)

#include "../HttpTransport.h"
#include "../HttpBodyReader.h"
#include <dlfcn.h>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace Aperture {

namespace {

bool CrackUrlComponents(const std::wstring& url, bool allowHttp, std::wstring& outHost, HttpResponse& errResp) {
    if (url.empty()) {
        errResp = {false, 0, "", L"", L"Empty URL", HttpTransportStatus::InvalidUrl};
        return false;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        errResp = {false, 0, "", L"", L"Invalid URL syntax", HttpTransportStatus::InvalidUrl};
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && !(allowHttp && uc.nScheme == INTERNET_SCHEME_HTTP)) {
        errResp = {false, 0, "", L"", L"HTTPS required", HttpTransportStatus::HttpsRequired};
        return false;
    }
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) {
        errResp = {false, 0, "", L"", L"Missing host", HttpTransportStatus::InvalidUrl};
        return false;
    }
    outHost.assign(uc.lpszHostName, uc.dwHostNameLength);
    return true;
}

bool IsAllowedHost(const std::wstring& host, const std::vector<std::wstring>& allowedHosts, HttpResponse& errResp) {
    if (allowedHosts.empty()) return true;
    for (const auto& allowed : allowedHosts) {
        if (!allowed.empty() && _wcsicmp(host.c_str(), allowed.c_str()) == 0) {
            return true;
        }
    }
    errResp = {false, 0, "", L"", L"Destination host is not allowed", HttpTransportStatus::DestinationRejected};
    return false;
}

bool ValidateSafeDestinationPath(const std::wstring& destination) {
    if (destination.empty()) return false;
    DWORD attr = GetFileAttributesW(destination.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        return false;
    }
    return true;
}

// Dynamic libcurl loader
struct LibCurlApi {
    void* lib = nullptr;
    bool loaded = false;

    using CURL = void;
    using CURLcode = int;
    using curl_off_t = int64_t;

    CURL* (*curl_easy_init)() = nullptr;
    void (*curl_easy_cleanup)(CURL* curl) = nullptr;
    CURLcode (*curl_easy_setopt)(CURL* curl, int option, ...) = nullptr;
    CURLcode (*curl_easy_perform)(CURL* curl) = nullptr;
    CURLcode (*curl_easy_getinfo)(CURL* curl, int info, ...) = nullptr;

    LibCurlApi() {
        for (const char* name : {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4"}) {
            lib = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return;
        curl_easy_init = reinterpret_cast<decltype(curl_easy_init)>(::dlsym(lib, "curl_easy_init"));
        curl_easy_cleanup = reinterpret_cast<decltype(curl_easy_cleanup)>(::dlsym(lib, "curl_easy_cleanup"));
        curl_easy_setopt = reinterpret_cast<decltype(curl_easy_setopt)>(::dlsym(lib, "curl_easy_setopt"));
        curl_easy_perform = reinterpret_cast<decltype(curl_easy_perform)>(::dlsym(lib, "curl_easy_perform"));
        curl_easy_getinfo = reinterpret_cast<decltype(curl_easy_getinfo)>(::dlsym(lib, "curl_easy_getinfo"));
        loaded = (curl_easy_init && curl_easy_cleanup && curl_easy_setopt && curl_easy_perform && curl_easy_getinfo);
    }
};

LibCurlApi& GetCurl() {
    static LibCurlApi instance;
    return instance;
}

struct CurlWriteCtx {
    std::string* body = nullptr;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    size_t maxBytes = 0;
    size_t totalBytes = 0;
    bool tooLarge = false;
    bool fileError = false;
    const HttpRequestOptions* options = nullptr;
    bool cancelled = false;
};

size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t bytes = size * nmemb;
    auto* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (!ctx) return 0;
    if (ctx->options && ctx->options->IsCancelled()) {
        ctx->cancelled = true;
        return 0;
    }
    if (ctx->maxBytes > 0 && (ctx->totalBytes + bytes > ctx->maxBytes)) {
        ctx->tooLarge = true;
        return 0;
    }
    ctx->totalBytes += bytes;
    if (ctx->body) {
        ctx->body->append(ptr, bytes);
    } else if (ctx->hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!WriteFile(ctx->hFile, ptr, static_cast<DWORD>(bytes), &written, nullptr) || written != bytes) {
            ctx->fileError = true;
            return 0;
        }
    }
    return bytes;
}

int CurlProgressCallback(void* clientp, int64_t /*dltotal*/, int64_t /*dlnow*/, int64_t /*ultotal*/, int64_t /*ulnow*/) {
    auto* ctx = static_cast<CurlWriteCtx*>(clientp);
    if (ctx && ctx->options && ctx->options->IsCancelled()) {
        ctx->cancelled = true;
        return 1; // abort transfer
    }
    return 0;
}

} // namespace

HttpResponse IHttpTransport::GetToFile(const std::wstring& httpsUrl, const std::wstring& destination, const HttpRequestOptions& options) {
    if (!ValidateSafeDestinationPath(destination)) {
        return {false, 0, "", L"", L"Destination path is invalid", HttpTransportStatus::FileError};
    }
    HttpResponse resp = Get(httpsUrl, options);
    if (!resp.success) return resp;
    std::wstring tmpPath = destination + L".part";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return {false, resp.statusCode, "", resp.finalUrl, L"Failed to create destination file", HttpTransportStatus::FileError};
    }
    DWORD written = 0;
    BOOL ok = resp.body.empty() ? TRUE : WriteFile(hFile, resp.body.data(), static_cast<DWORD>(resp.body.size()), &written, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    if (!ok || written != resp.body.size()) {
        DeleteFileW(tmpPath.c_str());
        return {false, resp.statusCode, "", resp.finalUrl, L"Failed to write destination file", HttpTransportStatus::FileError};
    }
    if (!MoveFileExW(tmpPath.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return {false, resp.statusCode, "", resp.finalUrl, L"Failed to commit destination file", HttpTransportStatus::FileError};
    }
    resp.body.clear();
    return resp;
}

static HttpResponse PerformCurlTransfer(const std::wstring& httpsUrl, const HttpRequestOptions& options, const std::wstring* destFile) {
    if (options.IsCancelled()) {
        return {false, 0, "", L"", L"Operation cancelled", HttpTransportStatus::Cancelled};
    }

    std::wstring currentUrl = httpsUrl;
    for (int hop = 0; hop <= options.maxRedirects; ++hop) {
        if (options.IsCancelled()) {
            return {false, 0, "", L"", L"Operation cancelled", HttpTransportStatus::Cancelled};
        }
        std::wstring host;
        HttpResponse errResp;
        if (!CrackUrlComponents(currentUrl, options.allowHttp, host, errResp)) return errResp;
        if (!IsAllowedHost(host, options.allowedHosts, errResp)) return errResp;

        auto& curl = GetCurl();
        if (!curl.loaded) {
            return {false, 0, "", currentUrl, L"libcurl.so.4 not available", HttpTransportStatus::NetworkError};
        }

        void* h = curl.curl_easy_init();
        if (!h) {
            return {false, 0, "", currentUrl, L"curl_easy_init failed", HttpTransportStatus::NetworkError};
        }

        std::string u8Url = LinuxWideToUtf8(currentUrl);
        std::string u8Ua = LinuxWideToUtf8(options.userAgent.empty() ? L"Aperture/2.0" : options.userAgent);
        std::string responseBody;
        std::wstring tmpPath;
        HANDLE hOutFile = INVALID_HANDLE_VALUE;

        if (destFile) {
            if (!ValidateSafeDestinationPath(*destFile)) {
                curl.curl_easy_cleanup(h);
                return {false, 0, "", currentUrl, L"Destination path rejected", HttpTransportStatus::FileError};
            }
            tmpPath = *destFile + L".part";
            hOutFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOutFile == INVALID_HANDLE_VALUE) {
                curl.curl_easy_cleanup(h);
                return {false, 0, "", currentUrl, L"Failed to open output file", HttpTransportStatus::FileError};
            }
        }

        CurlWriteCtx ctx{};
        ctx.body = destFile ? nullptr : &responseBody;
        ctx.hFile = hOutFile;
        ctx.maxBytes = options.maxResponseBytes;
        ctx.options = &options;

        // CURLOPT constants from curl/curl.h
        curl.curl_easy_setopt(h, 10002 /*CURLOPT_URL*/, u8Url.c_str());
        curl.curl_easy_setopt(h, 10018 /*CURLOPT_USERAGENT*/, u8Ua.c_str());
        curl.curl_easy_setopt(h, 20011 /*CURLOPT_WRITEFUNCTION*/, &CurlWriteCallback);
        curl.curl_easy_setopt(h, 10001 /*CURLOPT_WRITEDATA*/, &ctx);
        curl.curl_easy_setopt(h, 20219 /*CURLOPT_XFERINFOFUNCTION*/, &CurlProgressCallback);
        curl.curl_easy_setopt(h, 10057 /*CURLOPT_XFERINFODATA*/, &ctx);
        curl.curl_easy_setopt(h, 43 /*CURLOPT_NOPROGRESS*/, 0L);
        curl.curl_easy_setopt(h, 52 /*CURLOPT_FOLLOWLOCATION*/, 0L); // We handle redirects hop-by-hop to enforce host & scheme policy
        curl.curl_easy_setopt(h, 155 /*CURLOPT_TIMEOUT_MS*/, static_cast<long>(std::max(100, options.timeoutMs)));
        curl.curl_easy_setopt(h, 156 /*CURLOPT_CONNECTTIMEOUT_MS*/, static_cast<long>(std::max(100, options.timeoutMs)));
        curl.curl_easy_setopt(h, 64 /*CURLOPT_SSL_VERIFYPEER*/, 1L);
        curl.curl_easy_setopt(h, 81 /*CURLOPT_SSL_VERIFYHOST*/, 2L);

        int res = curl.curl_easy_perform(h);
        long httpCode = 0;
        curl.curl_easy_getinfo(h, 0x200002 /*CURLINFO_RESPONSE_CODE*/, &httpCode);
        const char* redirectUrlPtr = nullptr;
        curl.curl_easy_getinfo(h, 0x10001F /*CURLINFO_REDIRECT_URL*/, &redirectUrlPtr);
        std::string redirectUrl = redirectUrlPtr ? redirectUrlPtr : "";
        curl.curl_easy_cleanup(h);

        if (hOutFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hOutFile);
            CloseHandle(hOutFile);
        }

        if (ctx.cancelled || options.IsCancelled()) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            return {false, 0, "", currentUrl, L"Operation cancelled", HttpTransportStatus::Cancelled};
        }
        if (ctx.tooLarge) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            return {false, static_cast<int>(httpCode), "", currentUrl, L"Response exceeded maximum size", HttpTransportStatus::ResponseTooLarge};
        }
        if (ctx.fileError) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            return {false, static_cast<int>(httpCode), "", currentUrl, L"Failed writing to file", HttpTransportStatus::FileError};
        }
        if (res != 0) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            if (res == 28 /*CURLE_OPERATION_TIMEDOUT*/) {
                return {false, 0, "", currentUrl, L"Request timed out", HttpTransportStatus::Timeout};
            }
            return {false, 0, "", currentUrl, L"Network error", HttpTransportStatus::NetworkError};
        }

        if (httpCode == 301 || httpCode == 302 || httpCode == 303 || httpCode == 307 || httpCode == 308) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            if (hop >= options.maxRedirects) {
                return {false, static_cast<int>(httpCode), "", currentUrl, L"Too many redirects", HttpTransportStatus::TooManyRedirects};
            }
            if (redirectUrl.empty()) {
                return {false, static_cast<int>(httpCode), "", currentUrl, L"Redirect missing Location", HttpTransportStatus::NetworkError};
            }
            currentUrl = LinuxUtf8ToWide(redirectUrl);
            continue;
        }

        if (httpCode < 200 || httpCode >= 300) {
            if (!tmpPath.empty()) DeleteFileW(tmpPath.c_str());
            return {false, static_cast<int>(httpCode), "", currentUrl, L"HTTP error " + std::to_wstring(httpCode), HttpTransportStatus::HttpError};
        }

        if (destFile) {
            if (!MoveFileExW(tmpPath.c_str(), destFile->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(tmpPath.c_str());
                return {false, static_cast<int>(httpCode), "", currentUrl, L"Failed to move downloaded file", HttpTransportStatus::FileError};
            }
            return {true, static_cast<int>(httpCode), "", currentUrl, L"", HttpTransportStatus::Success};
        }

        return {true, static_cast<int>(httpCode), std::move(responseBody), currentUrl, L"", HttpTransportStatus::Success};
    }

    return {false, 0, "", currentUrl, L"Too many redirects", HttpTransportStatus::TooManyRedirects};
}

HttpResponse WinHttpTransport::Get(const std::wstring& httpsUrl, const HttpRequestOptions& options) {
    return PerformCurlTransfer(httpsUrl, options, nullptr);
}

HttpResponse WinHttpTransport::GetToFile(const std::wstring& httpsUrl, const std::wstring& destination, const HttpRequestOptions& options) {
    return PerformCurlTransfer(httpsUrl, options, &destination);
}

// ============================================================================
// MockHttpTransport Implementation (identical to Windows for unit tests)
// ============================================================================

void MockHttpTransport::SetResponse(const std::wstring& url, const HttpResponse& response) {
    std::lock_guard<std::mutex> lock(mockMutex_);
    responses_[url] = response;
}

void MockHttpTransport::SetHandler(const std::wstring& urlPrefix, MockHandler handler) {
    std::lock_guard<std::mutex> lock(mockMutex_);
    handlers_.push_back({urlPrefix, std::move(handler)});
}

void MockHttpTransport::Clear() {
    std::lock_guard<std::mutex> lock(mockMutex_);
    responses_.clear();
    handlers_.clear();
    requestedUrls_.clear();
}

HttpResponse MockHttpTransport::Get(const std::wstring& httpsUrl, const HttpRequestOptions& options) {
    if (options.IsCancelled()) {
        return {false, 0, "", L"", L"Operation cancelled", HttpTransportStatus::Cancelled};
    }
    std::wstring host;
    HttpResponse errResp;
    if (!CrackUrlComponents(httpsUrl, options.allowHttp, host, errResp)) return errResp;
    if (!IsAllowedHost(host, options.allowedHosts, errResp)) return errResp;

    MockHandler matchedHandler;
    HttpResponse matchedResp = defaultResponse_;
    bool foundExact = false;
    {
        std::lock_guard<std::mutex> lock(mockMutex_);
        requestedUrls_.push_back(httpsUrl);
        auto it = responses_.find(httpsUrl);
        if (it != responses_.end()) {
            matchedResp = it->second;
            foundExact = true;
        } else {
            for (const auto& [prefix, h] : handlers_) {
                if (httpsUrl.rfind(prefix, 0) == 0) {
                    matchedHandler = h;
                    break;
                }
            }
        }
    }
    if (matchedHandler) {
        matchedResp = matchedHandler(httpsUrl, options);
    }
    if (options.IsCancelled()) {
        return {false, 0, "", L"", L"Operation cancelled", HttpTransportStatus::Cancelled};
    }
    if (matchedResp.body.size() > options.maxResponseBytes) {
        return {false, matchedResp.statusCode, "", httpsUrl, L"Response exceeded maximum size", HttpTransportStatus::ResponseTooLarge};
    }
    if (matchedResp.finalUrl.empty()) matchedResp.finalUrl = httpsUrl;
    return matchedResp;
}

} // namespace Aperture

#endif // !defined(_WIN32)
