#pragma once

#include "../support/TestFramework.h"
#include "../../src/InternetRadio.h"
#include "../../src/PathFileHelpers.h"
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(InternetRadio, ParseRadioBrowserJson) {
    const std::string json = R"([
        {"name":"  Jazz FM  ","url_resolved":"https://stream.example.com/jazz.mp3","bitrate":128,"codec":"MP3","country":"United Kingdom"},
        {"name":"News Talk","url":"http://stream.example.com/news.aac","bitrate":64,"codec":"AAC"},
        {"name":"ftp station","url":"ftp://files.example.com/stream","bitrate":96,"codec":"MP3"},
        {"name":"Duplicate","url_resolved":"https://stream.example.com/jazz.mp3","bitrate":128,"codec":"MP3"},
        {"name":"","url_resolved":"https://stream.example.com/blank.flac"},
        {"name":")" + std::string(80, 'x') + R"(","url_resolved":"https://stream.example.com/long.mp3","bitrate":32,"codec":"MP3","country":"Testland"}
    ])";
    const auto stations = ParseRadioBrowserJson(json);
    TEST_ASSERT_EQ(stations.size(), size_t(4));
    TEST_ASSERT_EQ(stations[0].name, std::wstring(L"Jazz FM"));
    TEST_ASSERT_EQ(stations[0].url, std::wstring(L"https://stream.example.com/jazz.mp3"));
    TEST_ASSERT_EQ(stations[0].bitrateKbps, 128);
    TEST_ASSERT_EQ(stations[0].codec, std::wstring(L"MP3"));
    TEST_ASSERT_EQ(stations[0].country, std::wstring(L"United Kingdom"));
    // Plain "url" fallback when "url_resolved" is missing.
    TEST_ASSERT_EQ(stations[1].url, std::wstring(L"http://stream.example.com/news.aac"));
    TEST_ASSERT_EQ(stations[1].bitrateKbps, 64);
    TEST_ASSERT_EQ(stations[1].country, std::wstring(L""));
    // Non-HTTP(S) URL skipped, duplicate URL removed.
    TEST_ASSERT_EQ(stations[2].name, std::wstring(L"Unnamed station"));
    TEST_ASSERT_EQ(stations[2].url, std::wstring(L"https://stream.example.com/blank.flac"));
    TEST_ASSERT_EQ(stations[2].bitrateKbps, 0);
    TEST_ASSERT(stations[2].codec.empty());
    // Names are clamped to 64 characters.
    TEST_ASSERT_EQ(stations[3].name.size(), size_t(64));
    TEST_ASSERT_EQ(stations[3].bitrateKbps, 32);
}

TEST_CASE(InternetRadio, ParseRadioBrowserJsonRejectsInvalidInput) {
    TEST_ASSERT(ParseRadioBrowserJson("").empty());
    TEST_ASSERT(ParseRadioBrowserJson("not json at all").empty());
    TEST_ASSERT(ParseRadioBrowserJson("{\"name\":\"object not array\"}").empty());
    TEST_ASSERT(ParseRadioBrowserJson("[]").empty());
}

TEST_CASE(InternetRadio, PercentEncodeUrlPathUtf8) {
    // Unreserved characters pass through; spaces and UTF-8 become %XX octets.
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L"Jazz FM"), std::string("Jazz%20FM"));
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L"Rádio"), std::string("R%C3%A1dio"));
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L"A-1_.~z9"), std::string("A-1_.~z9"));
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L"&?=#"), std::string("%26%3F%3D%23"));
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L""), std::string(""));
    TEST_ASSERT_EQ(PercentEncodeUrlPathUtf8(L"日本"), std::string("%E6%97%A5%E6%9C%AC"));
}

TEST_CASE(InternetRadio, BuildSearchUrlEncodesQuery) {
    const std::wstring url = InternetRadioService::BuildSearchUrl(L"Jazz FM");
    TEST_ASSERT(url.find(L"/json/stations/byname/Jazz%20FM") != std::wstring::npos);
    TEST_ASSERT(url.find(L"limit=60") != std::wstring::npos);
    TEST_ASSERT(url.rfind(L"https://de1.api.radio-browser.info/", 0) == 0);
}

TEST_CASE(InternetRadio, SearchStationsParsesEmptyAndFailedResponses) {
    MockHttpTransport transport;
    InternetRadioService service;
    // A valid empty array means no matches, not a failure.
    transport.SetResponse(InternetRadioService::BuildSearchUrl(L"nothing here"),
                          HttpResponse{ true, 200, "[]", L"", L"", HttpTransportStatus::Success });
    RadioSearchResult result = service.SearchStations(L"nothing here", &transport);
    TEST_ASSERT(result.success);
    TEST_ASSERT(result.stations.empty());
    TEST_ASSERT_EQ(result.query, std::wstring(L"nothing here"));

    const std::string body = R"([{"name":"Jazz FM","url_resolved":"https://stream.example.com/jazz.mp3","bitrate":128,"codec":"MP3"}])";
    transport.SetResponse(InternetRadioService::BuildSearchUrl(L"jazz"),
                          HttpResponse{ true, 200, body, L"", L"", HttpTransportStatus::Success });
    result = service.SearchStations(L"jazz", &transport);
    TEST_ASSERT(result.success);
    TEST_ASSERT_EQ(result.stations.size(), size_t(1));
    TEST_ASSERT_EQ(result.stations[0].name, std::wstring(L"Jazz FM"));

    // An HTTP failure surfaces as an unsuccessful result with a message.
    transport.SetResponse(InternetRadioService::BuildSearchUrl(L"down"),
                          HttpResponse{ false, 500, "", L"", L"server error", HttpTransportStatus::HttpError });
    result = service.SearchStations(L"down", &transport);
    TEST_ASSERT(!result.success);
    TEST_ASSERT(!result.errorMessage.empty());
}

TEST_CASE(InternetRadio, FavoritesAddDedupeRemoveAndRoundTrip) {
    ScopedTempDir root;
    RadioFavorites store;
    RadioStation a;
    a.name = L"  Jazz FM  ";
    a.url = L"https://stream.example.com/jazz.mp3";
    a.bitrateKbps = 128;
    a.codec = L"MP3";
    a.country = L"United Kingdom";
    RadioStation b;
    b.name = L"News Talk";
    b.url = L"http://stream.example.com/news.aac";
    TEST_ASSERT(store.Add(a));
    TEST_ASSERT(!store.Add(a)); // dedupe by URL keeps the first entry
    TEST_ASSERT(store.Add(b));
    TEST_ASSERT_EQ(store.GetStations().size(), size_t(2));
    TEST_ASSERT(store.Contains(L"https://stream.example.com/jazz.mp3"));
    TEST_ASSERT_EQ(store.GetStations()[0].name, std::wstring(L"Jazz FM")); // whitespace trimmed
    TEST_ASSERT(store.Remove(L"https://stream.example.com/jazz.mp3"));
    TEST_ASSERT(!store.Remove(L"https://missing.example/stream"));
    TEST_ASSERT_EQ(store.GetStations().size(), size_t(1));

    TEST_ASSERT(store.Save(root.GetPath()).is_ok());
    const auto file = root.SubPath(RadioFavorites::kDefaultFileName);
    std::string saved;
    TEST_ASSERT(ReadTextFile(file, saved).is_ok());
    JsonValue json;
    TEST_ASSERT(JsonValue::parse(saved, json));
    TEST_ASSERT_EQ(json.get("schemaVersion").as_int64(0), int64_t(1));
    TEST_ASSERT_EQ(json.get("stations").as_array().size(), size_t(1));
    TEST_ASSERT_EQ(json.get("stations")[0].get("url").as_string(), "http://stream.example.com/news.aac");
    TEST_ASSERT_EQ(json.get("stations")[0].get("name").as_string(), "News Talk");

    RadioFavorites restarted;
    TEST_ASSERT(restarted.Load(root.GetPath()).is_ok());
    TEST_ASSERT_EQ(restarted.GetStations().size(), size_t(1));
    TEST_ASSERT_EQ(restarted.GetStations()[0].name, std::wstring(L"News Talk"));
    TEST_ASSERT_EQ(restarted.GetStations()[0].bitrateKbps, 0);
    TEST_ASSERT(restarted.Save(root.GetPath()).is_ok());
    std::string roundtrip;
    TEST_ASSERT(ReadTextFile(file, roundtrip).is_ok());
    TEST_ASSERT_EQ(saved, roundtrip);
}

TEST_CASE(InternetRadio, FavoritesTolerateUnknownFieldsAndMissingFile) {
    ScopedTempDir root;
    const auto file = root.SubPath(RadioFavorites::kDefaultFileName);
    TEST_ASSERT(AtomicWriteTextFile(file,
        "{\"schemaVersion\":1,\"stations\":[{\"url\":\"https://s.example/a.mp3\",\"name\":\"A\",\"bitrateKbps\":96,\"extra\":1},"
        "{\"noUrl\":true},{\"url\":\"\"},{\"url\":\"https://s.example/a.mp3\",\"name\":\"Dup\"}]}").is_ok());
    RadioFavorites store;
    TEST_ASSERT(store.Load(root.GetPath()).is_ok());
    TEST_ASSERT_EQ(store.GetStations().size(), size_t(1));
    TEST_ASSERT_EQ(store.GetStations()[0].name, std::wstring(L"A"));
    TEST_ASSERT_EQ(store.GetStations()[0].bitrateKbps, 96);

    RadioFavorites pristine;
    TEST_ASSERT(pristine.Load(root.GetPath(), L"missing_radio.json").is_ok());
    TEST_ASSERT(pristine.GetStations().empty());
}

TEST_CASE(InternetRadio, ParseRadioPlaylistTextHandlesM3uPlsAndTxt) {
    // M3U: #EXTINF names the URL that follows.
    const std::string m3u =
        "#EXTM3U\n"
        "#EXTINF:-1,Jazz FM\n"
        "https://stream.example.com/jazz.mp3\n"
        "#EXTINF:123,News Talk\n"
        "http://stream.example.com/news.aac\n"
        "https://stream.example.com/jazz.mp3\n"
        "ftp://stream.example.com/invalid\n";
    const auto stations = ParseRadioPlaylistText(m3u);
    TEST_ASSERT_EQ(stations.size(), size_t(2));
    TEST_ASSERT_EQ(stations[0].name, std::wstring(L"Jazz FM"));
    TEST_ASSERT_EQ(stations[0].url, std::wstring(L"https://stream.example.com/jazz.mp3"));
    TEST_ASSERT_EQ(stations[1].name, std::wstring(L"News Talk"));

    // PLS: FileN=/TitleN= pairs, order-independent.
    const std::string pls =
        "[playlist]\n"
        "NumberOfEntries=2\n"
        "File1=https://stream.example.com/one.mp3\n"
        "Title1=One\n"
        "File2=http://stream.example.com/two.mp3\n"
        "Title2=Two\n";
    const auto plsStations = ParseRadioPlaylistText(pls);
    TEST_ASSERT_EQ(plsStations.size(), size_t(2));
    TEST_ASSERT_EQ(plsStations[0].name, std::wstring(L"One"));
    TEST_ASSERT_EQ(plsStations[1].url, std::wstring(L"http://stream.example.com/two.mp3"));

    // Plain URL-per-line text with names falling back to the placeholder.
    const auto txtStations = ParseRadioPlaylistText("https://stream.example.com/x.mp3\n#comment\n");
    TEST_ASSERT_EQ(txtStations.size(), size_t(1));
    TEST_ASSERT_EQ(txtStations[0].name, std::wstring(L"Unnamed station"));

    TEST_ASSERT(ParseRadioPlaylistText("").empty());
    TEST_ASSERT(ParseRadioPlaylistText("no urls here").empty());
}

} // namespace Aperture::Test
