#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/OnlineSubtitles.h"
#include "../../src/Settings.h"
#include <string>
#include <vector>
#include "TestAsyncNetworkJobs.h"

namespace Aperture::Test {

TEST_CASE(OnlineSubtitles, QueryDerivation) {
    // Standard scene release filename cleaning
    std::wstring q1 = OnlineSubtitleService::DeriveSearchQuery(L"C:\\media\\movies\\Inception.2010.1080p.BluRay.x264.DTS-HD.mp4");
    TEST_ASSERT_EQ(q1, std::wstring(L"Inception 2010"));

    // Underscores and dots
    std::wstring q2 = OnlineSubtitleService::DeriveSearchQuery(L"D:\\Videos\\The_Matrix_1999_720p_WEBRip_AAC.mkv");
    TEST_ASSERT_EQ(q2, std::wstring(L"The Matrix 1999"));

    // TV Show with season and episode preserved
    std::wstring q3 = OnlineSubtitleService::DeriveSearchQuery(L"C:\\TV\\Breaking.Bad.S01E01.Pilot.720p.HDTV.x264.mkv");
    TEST_ASSERT_EQ(q3, std::wstring(L"Breaking Bad S01E01 Pilot"));

    // Bracketed noise
    std::wstring q4 = OnlineSubtitleService::DeriveSearchQuery(L"[AnimeGroup] Frieren - Beyond Journey's End - 01 [1080p HEVC].mkv");
    TEST_ASSERT(q4.find(L"Frieren") != std::wstring::npos);

    // Title override takes precedence
    std::wstring q5 = OnlineSubtitleService::DeriveSearchQuery(L"C:\\media\\vid12345.mp4", L"Interstellar (2014)");
    TEST_ASSERT_EQ(q5, std::wstring(L"Interstellar 2014"));
}

TEST_CASE(OnlineSubtitles, EndpointAndUrlBuilding) {
    // Valid HTTPS endpoints
    TEST_ASSERT(OnlineSubtitleService::IsValidEndpointTemplate("https://api.opensubtitles.com/api/v1/subtitles?query={query}&languages={lang}"));
    TEST_ASSERT(OnlineSubtitleService::IsValidEndpointTemplate("https://custom.provider.org/subtitles?q={q}"));
    TEST_ASSERT(OnlineSubtitleService::IsValidEndpointTemplate("")); // Empty defaults to built-in HTTPS

    // Disallowed non-HTTPS
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("http://insecure.api.com/subtitles?q={q}"));
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("ftp://ftp.subtitles.com/"));
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("file:///C:/subtitles.json"));
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("https://api.com/path with spaces"));

    // URL building with proper UTF-8 percent-encoding
    std::wstring url = OnlineSubtitleService::BuildSearchUrl(
        "https://api.example.com/search?q={query}&lang={lang}",
        L"The Dark Knight",
        "eng");
    TEST_ASSERT_EQ(url, std::wstring(L"https://api.example.com/search?q=The%20Dark%20Knight&lang=eng"));
}

TEST_CASE(OnlineSubtitles, TransportMockingAndSizeLimits) {
    MockHttpTransport transport;

    // Set canned HTTPS response
    HttpResponse validRes;
    validRes.success = true;
    validRes.statusCode = 200;
    validRes.body = "{\"results\": []}";
    transport.SetResponse(L"https://api.example.com/search?q=Test&lang=eng", validRes);

    HttpRequestOptions opts;
    opts.maxResponseBytes = 1024;

    HttpResponse resp = transport.Get(L"https://api.example.com/search?q=Test&lang=eng", opts);
    TEST_ASSERT(resp.success);
    TEST_ASSERT_EQ(resp.statusCode, 200);

    // Enforce HTTPS-only check: HTTP must be rejected
    HttpResponse insecureResp = transport.Get(L"http://api.example.com/search?q=Test", opts);
    TEST_ASSERT(!insecureResp.success);
    TEST_ASSERT_EQ(insecureResp.status, OnlineSubtitleStatus::HttpsRequired);

    // Response size bounding
    HttpResponse largeRes;
    largeRes.success = true;
    largeRes.statusCode = 200;
    largeRes.body = std::string(2048, 'X');
    transport.SetResponse(L"https://api.example.com/large", largeRes);

    HttpResponse boundedResp = transport.Get(L"https://api.example.com/large", opts);
    TEST_ASSERT(!boundedResp.success);
    TEST_ASSERT_EQ(boundedResp.status, OnlineSubtitleStatus::ResponseTooLarge);
}

TEST_CASE(OnlineSubtitles, JsonSearchResponseParsing) {
    // Array format
    std::string arrayJson = R"([
        {
            "id": "1001",
            "title": "Inception.2010.en.srt",
            "language": "English",
            "url": "https://download.example.com/1001.srt",
            "format": "srt",
            "score": 95
        },
        {
            "id": "1002",
            "title": "Inception.2010.insecure.srt",
            "language": "English",
            "url": "http://insecure.download.com/1002.srt",
            "format": "srt"
        }
    ])";

    auto results = OnlineSubtitleService::ParseSearchResultsJson(arrayJson, L"English");
    // Insecure HTTP download URL must be filtered out
    TEST_ASSERT_EQ(results.size(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(results[0].id, std::string("1001"));
    TEST_ASSERT_EQ(results[0].title, std::wstring(L"Inception.2010.en.srt"));
    TEST_ASSERT_EQ(results[0].language, std::wstring(L"English"));
    TEST_ASSERT_EQ(results[0].downloadUrl, std::string("https://download.example.com/1001.srt"));
    TEST_ASSERT_EQ(results[0].score, 95);

    // Object with results list format
    std::string objJson = R"({
        "results": [
            {
                "file_id": "2001",
                "name": "Interstellar.2014.es.vtt",
                "lang": "Spanish",
                "download_url": "https://download.example.com/2001.vtt",
                "format": "vtt"
            }
        ]
    })";

    auto resultsObj = OnlineSubtitleService::ParseSearchResultsJson(objJson, L"Spanish");
    TEST_ASSERT_EQ(resultsObj.size(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(resultsObj[0].id, std::string("2001"));
    TEST_ASSERT_EQ(resultsObj[0].title, std::wstring(L"Interstellar.2014.es.vtt"));
    TEST_ASSERT_EQ(resultsObj[0].downloadUrl, std::string("https://download.example.com/2001.vtt"));
    TEST_ASSERT_EQ(resultsObj[0].format, std::string("vtt"));
}

TEST_CASE(OnlineSubtitles, SubtitleContentValidation) {
    // Valid SRT content
    std::string validSrt =
        "1\r\n"
        "00:00:01,000 --> 00:00:04,000\r\n"
        "Hello, world!\r\n\r\n"
        "2\r\n"
        "00:00:05,000 --> 00:00:08,000\r\n"
        "This is a valid subtitle.\r\n";

    std::string normText;
    SubtitleEncoding enc = SubtitleEncoding::Unknown;
    TEST_ASSERT(OnlineSubtitleService::ValidateSubtitleContent(validSrt, normText, enc));
    TEST_ASSERT(!normText.empty());

    // Valid WebVTT content
    std::string validVtt =
        "WEBVTT\r\n\r\n"
        "00:01.000 --> 00:04.000\r\n"
        "First VTT cue\r\n\r\n"
        "00:05.000 --> 00:08.000\r\n"
        "Second VTT cue\r\n";

    TEST_ASSERT(OnlineSubtitleService::ValidateSubtitleContent(validVtt, normText, enc));

    // Invalid content (HTML 404 page or arbitrary text with no cues)
    std::string invalidHtml = "<html><body><h1>404 Not Found</h1></body></html>";
    TEST_ASSERT(!OnlineSubtitleService::ValidateSubtitleContent(invalidHtml, normText, enc));

    // Empty content
    TEST_ASSERT(!OnlineSubtitleService::ValidateSubtitleContent("", normText, enc));
}

TEST_CASE(OnlineSubtitles, AtomicStorageAndContainment) {
    ScopedTempDir tempDir(L"OnlineSubtitles_AtomicStorage");
    std::wstring root = tempDir.GetPath();

    std::string validSrt =
        "1\r\n"
        "00:00:01,000 --> 00:00:03,000\r\n"
        "Test subtitle line.\r\n";

    std::wstring savedPath;
    StorageStatus status = OnlineSubtitleService::SaveSubtitleAtomically(
        validSrt,
        L"Test_Movie_2024",
        L"eng",
        "sub_12345",
        "srt",
        savedPath,
        root);

    TEST_ASSERT(status.is_ok());
    TEST_ASSERT(!savedPath.empty());
    TEST_ASSERT(FileExists(savedPath));

    // Must be strictly contained within root cache directory
    std::wstring cacheDir = GetCacheDir(CacheClass::Subtitles, root);
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(cacheDir, savedPath));

    // Temporary files must have been cleaned up
    std::wstring tmpPattern = savedPath + L".tmp.*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(tmpPattern.c_str(), &fd);
    TEST_ASSERT(hFind == INVALID_HANDLE_VALUE);
}

TEST_CASE(OnlineSubtitles, OptInServiceWorkflow) {
    ScopedTempDir tempDir(L"OnlineSubtitles_Workflow");
    std::wstring root = tempDir.GetPath();

    Settings settings;
    // Disabled by default
    TEST_ASSERT(!settings.onlineSubtitlesEnabled);

    MockHttpTransport transport;
    OnlineSubtitleService service;

    // Search when disabled must fail with Disabled status
    OnlineSubtitleResult disabledRes = service.Search(L"C:\\media\\movie.mp4", L"", settings, &transport);
    TEST_ASSERT(!disabledRes.success);
    TEST_ASSERT_EQ(disabledRes.status, OnlineSubtitleStatus::Disabled);

    // Opt-in enable in settings
    settings.onlineSubtitlesEnabled = true;
    settings.onlineSubtitleEndpoint = "https://api.testprovider.com/subtitles?q={query}&lang={lang}";

    // Set mock search response
    std::wstring searchUrl = OnlineSubtitleService::BuildSearchUrl(settings.onlineSubtitleEndpoint, L"movie", settings.subLang);
    HttpResponse searchHttpRes;
    searchHttpRes.success = true;
    searchHttpRes.statusCode = 200;
    searchHttpRes.body = R"({
        "results": [
            {
                "id": "777",
                "title": "Movie.2024.en.srt",
                "language": "English",
                "download_url": "https://cdn.testprovider.com/sub/777.srt",
                "format": "srt"
            }
        ]
    })";
    transport.SetResponse(searchUrl, searchHttpRes);

    // Set mock file download response
    HttpResponse fileHttpRes;
    fileHttpRes.success = true;
    fileHttpRes.statusCode = 200;
    fileHttpRes.body =
        "1\r\n"
        "00:00:01,000 --> 00:00:05,000\r\n"
        "Retrieved online subtitle!\r\n";
    transport.SetResponse(L"https://cdn.testprovider.com/sub/777.srt", fileHttpRes);

    // Execute Search
    OnlineSubtitleResult searchRes = service.Search(L"C:\\media\\movie.mp4", L"", settings, &transport);
    TEST_ASSERT(searchRes.success);
    TEST_ASSERT_EQ(searchRes.searchResults.size(), static_cast<size_t>(1));

    // Execute Download and Validation
    OnlineSubtitleResult dlRes = service.DownloadAndLoad(
        searchRes.searchResults[0],
        L"C:\\media\\movie.mp4",
        settings,
        &transport,
        root);

    TEST_ASSERT(dlRes.success);
    TEST_ASSERT(!dlRes.savedFilePath.empty());
    TEST_ASSERT(FileExists(dlRes.savedFilePath));
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(GetCacheDir(CacheClass::Subtitles, root), dlRes.savedFilePath));
}

} // namespace Aperture::Test
