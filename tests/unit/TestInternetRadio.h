#pragma once

#include "../support/TestFramework.h"
#include "../../src/InternetRadio.h"
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

} // namespace Aperture::Test
