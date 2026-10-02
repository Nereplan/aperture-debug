#pragma once
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/HttpBodyReader.h"

namespace Aperture::Test {
TEST_CASE(HttpTransportFixes, InterruptedReadsNeverReturnPartialSuccess) {
    for (const DWORD failure : {ERROR_WINHTTP_TIMEOUT, ERROR_WINHTTP_CONNECTION_ERROR}) {
        HttpResponse response; HttpRequestOptions options; int calls = 0;
        TEST_ASSERT(!HttpDetail::ReadBody(response, options, [&](char* buffer, DWORD, DWORD& bytes, DWORD& error) {
            if (!calls++) { memcpy(buffer, "partial", 7); bytes = 7; return true; }
            error = failure; return false;
        }));
        TEST_ASSERT(!response.success); TEST_ASSERT(response.body.empty());
        TEST_ASSERT_EQ(response.status, failure == ERROR_WINHTTP_TIMEOUT ? HttpTransportStatus::Timeout : HttpTransportStatus::NetworkError);
    }
}
TEST_CASE(HttpTransportFixes, CleanEofAndDeclaredLengthAreDistinguished) {
    for (const uint64_t expected : {3ull, 4ull}) {
        HttpResponse response; HttpRequestOptions options; int calls = 0;
        const bool success = HttpDetail::ReadBody(response, options, [&](char* buffer, DWORD capacity, DWORD& bytes, DWORD&) {
            TEST_ASSERT_EQ(capacity, DWORD(64 * 1024));
            if (!calls++) { memcpy(buffer, "abc", 3); bytes = 3; } else bytes = 0;
            return true;
        }, {}, expected);
        TEST_ASSERT_EQ(success, expected == 3);
        TEST_ASSERT_EQ(response.success, expected == 3);
        TEST_ASSERT_EQ(response.body, expected == 3 ? std::string("abc") : std::string());
    }
}
TEST_CASE(HttpTransportFixes, StreamingBodyUsesBoundedBufferAndNoAccumulation) {
    HttpResponse response; HttpRequestOptions options; options.maxResponseBytes = 150 * 1024 * 1024;
    uint64_t received = 0; int chunks = 0;
    TEST_ASSERT(HttpDetail::ReadBody(response, options, [&](char* buffer, DWORD capacity, DWORD& bytes, DWORD&) {
        TEST_ASSERT_EQ(capacity, DWORD(64 * 1024));
        bytes = chunks++ < 2400 ? capacity : 0;
        memset(buffer, 'x', bytes); return true;
    }, [&](const char*, DWORD bytes) { received += bytes; return true; }, options.maxResponseBytes));
    TEST_ASSERT_EQ(received, uint64_t(150 * 1024 * 1024));
    TEST_ASSERT(response.body.empty()); TEST_ASSERT_EQ(response.body.capacity(), std::string().capacity());
}
TEST_CASE(HttpTransportFixes, BoundsCancellationAndSinkFailuresAreReported) {
    HttpRequestOptions options; options.maxResponseBytes = 2;
    const HttpDetail::ReadBodyChunk chunk = [](char* buffer, DWORD, DWORD& bytes, DWORD&) {
        memcpy(buffer, "abc", 3); bytes = 3; return true;
    };
    HttpResponse response;
    TEST_ASSERT(!HttpDetail::ReadBody(response, options, chunk));
    TEST_ASSERT_EQ(response.status, HttpTransportStatus::ResponseTooLarge);
    options.maxResponseBytes = 3;
    TEST_ASSERT(!HttpDetail::ReadBody(response, options, chunk, [](const char*, DWORD) { return false; }));
    TEST_ASSERT_EQ(response.status, HttpTransportStatus::FileError);
    options.isCancelled = [] { return true; };
    TEST_ASSERT(!HttpDetail::ReadBody(response, options, chunk));
    TEST_ASSERT_EQ(response.status, HttpTransportStatus::Cancelled);
}
TEST_CASE(HttpTransportFixes, DestinationPolicyAppliesToNativeRequestsAndInjectedFinalUrls) {
    HttpRequestOptions options; options.allowedHosts = {L"example.com"};
    WinHttpTransport native;
    TEST_ASSERT_EQ(native.Get(L"https://rejected.example/path", options).status, HttpTransportStatus::DestinationRejected);
    TEST_ASSERT_EQ(native.Get(L"http://example.com/path", options).status, HttpTransportStatus::HttpsRequired);
    TEST_ASSERT_EQ(native.Get(L"https://user:pass@example.com/path", options).status, HttpTransportStatus::InvalidUrl);
    MockHttpTransport mock;
    HttpResponse redirected; redirected.success = true; redirected.statusCode = 200;
    redirected.body = "body"; redirected.finalUrl = L"https://rejected.example/download";
    mock.SetResponse(L"https://example.com/download", redirected);
    TEST_ASSERT_EQ(mock.Get(L"https://example.com/download", options).status, HttpTransportStatus::DestinationRejected);
}
TEST_CASE(HttpTransportFixes, StagedDownloadsAreCheckedAndPartialFilesAreRemoved) {
    ScopedTempDir stage; MockHttpTransport transport; HttpResponse response;
    response.success = true; response.statusCode = 200; response.body = "complete body";
    const auto url = L"https://example.com/download"; transport.SetResponse(url, response);
    auto downloaded = transport.GetToFile(url, stage.SubPath(L"download.bin"));
    TEST_ASSERT(downloaded.success); TEST_ASSERT(downloaded.body.empty());
    std::string content; TEST_ASSERT(ReadTextFile(stage.SubPath(L"download.bin"), content).is_ok());
    TEST_ASSERT_EQ(content, std::string("complete body"));
    TEST_ASSERT_EQ(transport.GetToFile(url, stage.SubPath(L"download.bin")).status, HttpTransportStatus::FileError);
    response.success = false; response.status = HttpTransportStatus::Timeout; transport.SetResponse(url, response);
    TEST_ASSERT(!transport.GetToFile(url, stage.SubPath(L"failed.bin")).success);
    TEST_ASSERT(!FileExists(stage.SubPath(L"failed.bin")));
}
} // namespace Aperture::Test
