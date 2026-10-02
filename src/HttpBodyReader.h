#pragma once
#include "HttpTransport.h"
#include <winhttp.h>
#include <memory>
#include <optional>

namespace Aperture::HttpDetail {
using ReadBodyChunk = std::function<bool(char*, DWORD, DWORD&, DWORD&)>;
using BodySink = std::function<bool(const char*, DWORD)>;

inline bool ReadBody(HttpResponse& response, const HttpRequestOptions& options,
                     const ReadBodyChunk& read, const BodySink& sink = {}, std::optional<uint64_t> expectedBytes = {}) {
    constexpr DWORD chunkBytes = 64 * 1024;
    size_t total = 0;
    const auto fail = [&](HttpTransportStatus status, const std::wstring& message) {
        response.success = false; response.status = status; response.errorMessage = message;
        response.body.clear(); return false;
    };
    if (expectedBytes && *expectedBytes > options.maxResponseBytes)
        return fail(HttpTransportStatus::ResponseTooLarge, L"Response size exceeded configured ceiling");
    if (options.IsCancelled()) return fail(HttpTransportStatus::Cancelled, L"Request cancelled");
    std::unique_ptr<char[]> buffer;
    try { buffer.reset(new char[chunkBytes]); }
    catch (const std::bad_alloc&) {
        response.success = false; response.status = HttpTransportStatus::NetworkError;
        response.body.clear();
        try { response.errorMessage = L"Unable to allocate HTTPS response buffer"; } catch (...) {}
        return false;
    }
    for (;;) {
        if (options.IsCancelled()) return fail(HttpTransportStatus::Cancelled, L"Request cancelled");
        DWORD bytes = 0, error = ERROR_SUCCESS;
        if (!read(buffer.get(), chunkBytes, bytes, error))
            return fail(options.IsCancelled() ? HttpTransportStatus::Cancelled :
                        error == ERROR_WINHTTP_TIMEOUT ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError,
                        L"HTTPS response interrupted: error " + std::to_wstring(error));
        if (!bytes) break; // Only a successful zero-byte read is clean EOF.
        if (bytes > chunkBytes) return fail(HttpTransportStatus::NetworkError, L"Invalid response chunk size");
        if (bytes > options.maxResponseBytes - total)
            return fail(HttpTransportStatus::ResponseTooLarge, L"Response size exceeded configured ceiling");
        if (sink) {
            if (!sink(buffer.get(), bytes)) return fail(HttpTransportStatus::FileError, L"Failed to write downloaded response");
        } else response.body.append(buffer.get(), bytes);
        total += bytes;
    }
    if (options.IsCancelled()) return fail(HttpTransportStatus::Cancelled, L"Request cancelled");
    if (expectedBytes && total != *expectedBytes) return fail(HttpTransportStatus::NetworkError, L"HTTPS response ended before its declared size");
    response.success = true; response.status = HttpTransportStatus::Success; return true;
}
} // namespace Aperture::HttpDetail
