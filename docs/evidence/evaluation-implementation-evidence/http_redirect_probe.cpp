#include "HttpTransport.h"
#include <iostream>

int wmain() {
    Aperture::WinHttpTransport transport;
    struct Case { const char* name; const wchar_t* url; int limit; Aperture::HttpTransportStatus expected; };
    const Case cases[] = {
        {"zero_budget", L"https://httpbingo.org/redirect/1", 0, Aperture::HttpTransportStatus::TooManyRedirects},
        {"one_redirect", L"https://httpbingo.org/redirect/1", 1, Aperture::HttpTransportStatus::Success},
        {"exceeded_budget", L"https://httpbingo.org/redirect/3", 1, Aperture::HttpTransportStatus::TooManyRedirects},
        {"destination_rejected", L"https://httpbingo.org/redirect-to?url=https%3A%2F%2Fexample.com%2F", 3, Aperture::HttpTransportStatus::DestinationRejected},
        {"downgrade_rejected", L"https://httpbingo.org/redirect-to?url=http%3A%2F%2Fexample.com%2F", 3, Aperture::HttpTransportStatus::HttpsRequired}
    };
    std::cout << "case,expected_status,actual_status,http_status,success,body_bytes,final_url,error\n";
    int failures = 0;
    for (const auto& test : cases) {
        Aperture::HttpRequestOptions options; options.timeoutMs = 3500; options.maxRedirects = test.limit;
        options.allowedHosts = {L"httpbingo.org"}; options.maxResponseBytes = 128 * 1024;
        const auto response = transport.Get(test.url, options);
        if (response.status != test.expected) ++failures;
        std::cout << test.name << ',' << static_cast<int>(test.expected) << ',' << static_cast<int>(response.status) << ','
                  << response.statusCode << ',' << response.success << ',' << response.body.size() << ','
                  << Aperture::Utf16ToUtf8(response.finalUrl) << ',' << Aperture::Utf16ToUtf8(response.errorMessage) << '\n';
    }
    return failures ? 1 : 0;
}
