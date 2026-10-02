#pragma once
#include "HttpTransport.h"
#include <winhttp.h>
#include <array>
#include <optional>

namespace Aperture::HttpDetail {
using ReadBodyChunk = std::function<bool(char*, DWORD, DWORD&, DWORD&)>;
using BodySink = std::function<bool(const char*, DWORD)>;

inline bool ReadBody(HttpResponse& response, const HttpRequestOptions& options,
                     const ReadBodyChunk& read, const BodySink& sink = {}, std::optional<uint64_t> expectedBytes = {}) {
    std::array<char, 64 * 1024> buffer;
    size_t total = 0;
    const auto fail = [&](HttpTransportStatus status, const std::wstring& message) {
        response.success = false; response.status = status; response.errorMessage = message;
        response.body.clear(); return false;
    };
    if (expectedBytes && *expectedBytes > options.maxResponseBytes)
        return fail(HttpTransportStatus::ResponseTooLarge, L"Response size exceeded configured ceiling");
    for (;;) {
        if (options.isCancelled && options.isCancelled()) return fail(HttpTransportStatus::Cancelled, L"Request cancelled");
        DWORD bytes = 0, error = ERROR_SUCCESS;
        if (!read(buffer.data(), static_cast<DWORD>(buffer.size()), bytes, error))
            return fail(error == ERROR_WINHTTP_TIMEOUT ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError,
                        L"HTTPS response interrupted: error " + std::to_wstring(error));
        if (!bytes) break; // Only a successful zero-byte read is clean EOF.
        if (bytes > buffer.size()) return fail(HttpTransportStatus::NetworkError, L"Invalid response chunk size");
        if (bytes > options.maxResponseBytes - total)
            return fail(HttpTransportStatus::ResponseTooLarge, L"Response size exceeded configured ceiling");
        if (sink) {
            if (!sink(buffer.data(), bytes)) return fail(HttpTransportStatus::FileError, L"Failed to write downloaded response");
        } else response.body.append(buffer.data(), bytes);
        total += bytes;
    }
    if (expectedBytes && total != *expectedBytes) return fail(HttpTransportStatus::NetworkError, L"HTTPS response ended before its declared size");
    response.success = true; response.status = HttpTransportStatus::Success; return true;
}
} // namespace Aperture::HttpDetail
