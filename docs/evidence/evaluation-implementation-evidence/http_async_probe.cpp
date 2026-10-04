#include "HttpTransport.h"
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

int wmain() {
    using namespace Aperture;
    WinHttpTransport transport;
    std::cout << "case,expected_status,actual_status,http_status,body_bytes,cancel_to_completion_ms,passed\n";
    int failures = 0;
    for (const auto& test : { std::pair<const char*, const wchar_t*>{"async_get", L"https://httpbingo.org/get"},
                              {"async_redirect", L"https://httpbingo.org/redirect/1"} }) {
        HttpRequestOptions options; options.timeoutMs = 15000; options.maxResponseBytes = 128 * 1024;
        options.allowedHosts = { L"httpbingo.org" }; options.cancellation = std::make_shared<OperationCancellation>();
        const auto response = transport.Get(test.second, options);
        const bool passed = response.success && response.status == HttpTransportStatus::Success && !response.body.empty();
        if (!passed) ++failures;
        std::cout << test.first << ',' << static_cast<int>(HttpTransportStatus::Success) << ',' << static_cast<int>(response.status)
                  << ',' << response.statusCode << ',' << response.body.size() << ",0," << passed << '\n';
    }
    HttpRequestOptions options; options.timeoutMs = 15000; options.cancellation = std::make_shared<OperationCancellation>();
    const std::wstring delayed = L"https://httpbingo.org/delay/10?aperture_cancel_probe=" + std::to_wstring(GetTickCount64());
    auto pending = std::async(std::launch::async, [&] { return transport.Get(delayed, options); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bool pendingAtCancel = pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    const auto start = std::chrono::steady_clock::now(); options.cancellation->Cancel();
    const auto response = pending.get();
    const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const bool passed = pendingAtCancel && response.status == HttpTransportStatus::Cancelled && !response.success && milliseconds < 1500;
    if (!passed) ++failures;
    std::cout << "cancel_in_flight," << static_cast<int>(HttpTransportStatus::Cancelled) << ',' << static_cast<int>(response.status)
              << ',' << response.statusCode << ',' << response.body.size() << ',' << milliseconds << ',' << passed << '\n';
    wchar_t temporaryRoot[MAX_PATH]{}; GetTempPathW(MAX_PATH, temporaryRoot);
    const std::wstring file = std::wstring(temporaryRoot) + L"Aperture_AsyncHttp_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64()) + L".tmp";
    HttpRequestOptions fileOptions; fileOptions.timeoutMs = 15000; fileOptions.maxResponseBytes = 128 * 1024;
    fileOptions.cancellation = std::make_shared<OperationCancellation>();
    const auto fileResponse = transport.GetToFile(L"https://httpbingo.org/bytes/65536", file, fileOptions);
    const bool filePassed = fileResponse.success && fileResponse.body.empty() && GetFileSizeInBytes(file) == 65536;
    if (!filePassed) ++failures;
    std::cout << "async_file," << static_cast<int>(HttpTransportStatus::Success) << ',' << static_cast<int>(fileResponse.status)
              << ',' << fileResponse.statusCode << ',' << fileResponse.body.size() << ",0," << filePassed << '\n';
    DeleteFileW(file.c_str());
    fileOptions.cancellation = std::make_shared<OperationCancellation>();
    const auto drip = L"https://httpbingo.org/drip?duration=10&numbytes=1024&delay=0&aperture=" + std::to_wstring(GetTickCount64());
    auto pendingFile = std::async(std::launch::async, [&] { return transport.GetToFile(drip, file, fileOptions); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const bool filePending = pendingFile.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    const auto fileStart = std::chrono::steady_clock::now(); fileOptions.cancellation->Cancel();
    const auto cancelledFile = pendingFile.get();
    const auto fileMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fileStart).count();
    const bool cancelledFilePassed = filePending && cancelledFile.status == HttpTransportStatus::Cancelled &&
        !cancelledFile.success && !FileExists(file) && fileMilliseconds < 1500;
    if (!cancelledFilePassed) ++failures;
    std::cout << "cancel_stream_file," << static_cast<int>(HttpTransportStatus::Cancelled) << ',' << static_cast<int>(cancelledFile.status)
              << ',' << cancelledFile.statusCode << ',' << cancelledFile.body.size() << ',' << fileMilliseconds << ',' << cancelledFilePassed << '\n';
    DeleteFileW(file.c_str());
    return failures ? 1 : 0;
}
