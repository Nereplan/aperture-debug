#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/OnlineSubtitles.h"
#include "../../src/Settings.h"
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(OnlineSubtitles, HttpsSearchValidationAndContainment) {
    TEST_ASSERT_EQ(OnlineSubtitleService::DeriveSearchQuery(L"C:\\media\\movies\\Inception.2010.1080p.BluRay.x264.DTS-HD.mp4"),
                   std::wstring(L"Inception 2010"));
    TEST_ASSERT_EQ(OnlineSubtitleService::DeriveSearchQuery(L"C:\\TV\\Breaking.Bad.S01E01.Pilot.720p.HDTV.x264.mkv"),
                   std::wstring(L"Breaking Bad S01E01 Pilot"));
    TEST_ASSERT_EQ(OnlineSubtitleService::DeriveSearchQuery(L"C:\\media\\vid12345.mp4", L"Interstellar (2014)"),
                   std::wstring(L"Interstellar 2014"));

    TEST_ASSERT(OnlineSubtitleService::IsValidEndpointTemplate("https://api.opensubtitles.com/api/v1/subtitles?query={query}&languages={lang}"));
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("http://insecure.api.com/subtitles?q={q}"));
    TEST_ASSERT(!OnlineSubtitleService::IsValidEndpointTemplate("file:///C:/subtitles.json"));

    std::wstring builtUrl = OnlineSubtitleService::BuildSearchUrl(
        "https://api.example.com/search?q={query}&lang={lang}", L"The Dark Knight", "eng");
    TEST_ASSERT_EQ(builtUrl, std::wstring(L"https://api.example.com/search?q=The%20Dark%20Knight&lang=eng"));

    MockHttpTransport transport;
    HttpRequestOptions opts;
    opts.maxResponseBytes = 1024;
    HttpResponse insecureResp = transport.Get(L"http://api.example.com/search?q=Test", opts);
    TEST_ASSERT(!insecureResp.success);
    TEST_ASSERT_EQ(insecureResp.status, OnlineSubtitleStatus::HttpsRequired);

    HttpResponse largeRes;
    largeRes.success = true;
    largeRes.statusCode = 200;
    largeRes.body = std::string(2048, 'X');
    transport.SetResponse(L"https://api.example.com/large", largeRes);
    HttpResponse boundedResp = transport.Get(L"https://api.example.com/large", opts);
    TEST_ASSERT(!boundedResp.success);
    TEST_ASSERT_EQ(boundedResp.status, OnlineSubtitleStatus::ResponseTooLarge);

    std::string arrayJson = R"([
        {"id":"1001","title":"Inception.2010.en.srt","language":"English","url":"https://download.example.com/1001.srt","format":"srt","score":95},
        {"id":"1002","title":"Inception.2010.insecure.srt","language":"English","url":"http://insecure.download.com/1002.srt","format":"srt"}
    ])";
    auto parsed = OnlineSubtitleService::ParseSearchResultsJson(arrayJson, L"English");
    TEST_ASSERT_EQ(parsed.size(), size_t(1));
    TEST_ASSERT_EQ(parsed[0].id, std::string("1001"));

    std::string validSrt = "1\r\n00:00:01,000 --> 00:00:04,000\r\nHello, world!\r\n";
    std::string normText;
    SubtitleEncoding enc = SubtitleEncoding::Unknown;
    TEST_ASSERT(OnlineSubtitleService::ValidateSubtitleContent(validSrt, normText, enc));
    TEST_ASSERT(!OnlineSubtitleService::ValidateSubtitleContent("<html><body>404</body></html>", normText, enc));

    ScopedTempDir tempDir(L"OnlineSubtitles_Workflow");
    std::wstring root = tempDir.GetPath();
    std::wstring savedPath;
    TEST_ASSERT(OnlineSubtitleService::SaveSubtitleAtomically(validSrt, L"Test_Movie_2024", L"eng", "sub_12345", "srt", savedPath, root).is_ok());
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(GetCacheDir(CacheClass::Subtitles, root), savedPath));

    Settings settings;
    OnlineSubtitleService service;
    OnlineSubtitleResult disabledRes = service.Search(L"C:\\media\\movie.mp4", L"", settings, &transport);
    TEST_ASSERT(!disabledRes.success);
    TEST_ASSERT_EQ(disabledRes.status, OnlineSubtitleStatus::Disabled);

    settings.onlineSubtitlesEnabled = true;
    settings.onlineSubtitleEndpoint = "https://api.testprovider.com/subtitles?q={query}&lang={lang}";
    std::wstring searchUrl = OnlineSubtitleService::BuildSearchUrl(settings.onlineSubtitleEndpoint, L"movie", settings.subLang);
    HttpResponse searchHttpRes;
    searchHttpRes.success = true;
    searchHttpRes.statusCode = 200;
    searchHttpRes.body = R"({"results":[{"id":"777","title":"Movie.2024.en.srt","language":"English","download_url":"https://cdn.testprovider.com/sub/777.srt","format":"srt"}]})";
    transport.SetResponse(searchUrl, searchHttpRes);

    HttpResponse fileHttpRes;
    fileHttpRes.success = true;
    fileHttpRes.statusCode = 200;
    fileHttpRes.body = validSrt;
    transport.SetResponse(L"https://cdn.testprovider.com/sub/777.srt", fileHttpRes);

    OnlineSubtitleResult searchRes = service.Search(L"C:\\media\\movie.mp4", L"", settings, &transport);
    TEST_ASSERT(searchRes.success);
    TEST_ASSERT_EQ(searchRes.searchResults.size(), size_t(1));

    OnlineSubtitleResult dlRes = service.DownloadAndLoad(searchRes.searchResults[0], L"C:\\media\\movie.mp4", settings, &transport, root);
    TEST_ASSERT(dlRes.success);
    TEST_ASSERT(FileExists(dlRes.savedFilePath));
    TEST_ASSERT(SubtitleDiscovery::IsPathContained(GetCacheDir(CacheClass::Subtitles, root), dlRes.savedFilePath));
}

} // namespace Aperture::Test
