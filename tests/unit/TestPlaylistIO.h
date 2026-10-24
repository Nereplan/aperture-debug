#pragma once

#include "../support/TestFramework.h"
#include "../../src/PlaylistIO.h"
#include <cmath>
#include <random>

namespace Aperture::Test {

TEST_CASE(PlaylistIO, BatchAppendUsesOrdinalIgnoreCaseAndKeepsFirstOrder) {
    std::vector<std::wstring> existing = {
        L"C:\\Media\\Existing.mp4", L"C:\\Media\\Other.mp4", L"C:\\Media\\existing.mp4"
    };
    const std::vector<std::wstring> candidates = {
        L"c:\\media\\EXISTING.MP4", L"C:\\Media\\New.mp4", L"c:\\media\\new.MP4",
        L"C:\\Media\\Last.mp4", L"C:\\Media\\OTHER.mp4", L"C:\\Media\\New.mp4",
        L"C:\\Media\\Last.mp4", L"C:\\Media\\OTHER.mp4", L"C:\\Media\\New.mp4"
    };

    const size_t added = PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates);
    TEST_ASSERT_EQ(added, size_t(2));
    TEST_ASSERT_EQ(existing.size(), size_t(5));
    TEST_ASSERT_EQ(existing[0], std::wstring(L"C:\\Media\\Existing.mp4"));
    TEST_ASSERT_EQ(existing[1], std::wstring(L"C:\\Media\\Other.mp4"));
    TEST_ASSERT_EQ(existing[2], std::wstring(L"C:\\Media\\existing.mp4"));
    TEST_ASSERT_EQ(existing[3], std::wstring(L"C:\\Media\\New.mp4"));
    TEST_ASSERT_EQ(existing[4], std::wstring(L"C:\\Media\\Last.mp4"));
}

TEST_CASE(PlaylistIO, BatchAppendUsesOrdinalCaseRulesForUnicode) {
    std::vector<std::wstring> existing = {L"C:\\Media\\\u00E9.mov"};
    const std::vector<std::wstring> candidates = {
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00D6.mov",
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00F6.MOV",
        L"c:\\media\\\u00C9.MOV", L"C:\\Media\\e\u0301.mov", L"C:\\Media\\\u00D6.mov"
    };

    const size_t added = PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates);
    TEST_ASSERT_EQ(added, size_t(2));
    TEST_ASSERT_EQ(existing.size(), size_t(3));
    TEST_ASSERT_EQ(existing[0], std::wstring(L"C:\\Media\\\u00E9.mov"));
    TEST_ASSERT_EQ(existing[1], std::wstring(L"C:\\Media\\e\u0301.mov"));
    TEST_ASSERT_EQ(existing[2], std::wstring(L"C:\\Media\\\u00D6.mov"));
}

TEST_CASE(PlaylistIO, BatchAppendMatchesSequentialMembershipOracle) {
    std::mt19937 random(20261001);
    for (size_t count : {size_t(0), size_t(1), size_t(8), size_t(9), size_t(256)}) {
        for (int sample = 0; sample < 12; ++sample) {
            auto path = [&]() {
                const auto id = random() % 100;
                return (random() % 2 ? L"C:\\MEDIA\\Video" : L"c:\\media\\video") +
                    std::to_wstring(id) + (random() % 2 ? L".MKV" : L".mkv");
            };
            std::vector<std::wstring> existing;
            const size_t existingCount = random() % 128;
            for (size_t i = 0; i < existingCount; ++i) existing.push_back(path());
            std::vector<std::wstring> candidates;
            for (size_t i = 0; i < count; ++i) candidates.push_back(path());
            auto expected = existing;
            for (const auto& candidate : candidates) {
                bool duplicate = false;
                for (const auto& prior : expected) {
                    if (CompareStringOrdinal(prior.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) expected.push_back(candidate);
            }
            const auto expectedAdded = expected.size() - existing.size();
            TEST_ASSERT_EQ(PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, candidates), expectedAdded);
            TEST_ASSERT_EQ(existing.size(), expected.size());
            for (size_t i = 0; i < existing.size(); ++i) TEST_ASSERT_EQ(existing[i], expected[i]);
        }
    }
    // A repeated candidate after every existing group previously could cause
    // repeated scans of the same duplicate group during the merge.
    std::vector<std::wstring> existing;
    for (int i = 0; i < 100; ++i) existing.push_back(L"A" + std::to_wstring(i));
    const std::vector<std::wstring> repeated(256, L"Z");
    TEST_ASSERT_EQ(PlaylistIO::AppendUniquePathsOrdinalIgnoreCase(existing, repeated), size_t(1));
    TEST_ASSERT_EQ(existing.back(), std::wstring(L"Z"));
}

TEST_CASE(PlaylistIO, ParseAsxCollectsRefEntriesAndDecodesEntities) {
    const std::string content =
        "<ASX version=\"3.0\">\n"
        "  <TITLE>Test &amp; Demo</TITLE>\n"
        "  <ENTRY>\n"
        "    <TITLE>Stream &#65;&#66;</TITLE>\n"
        "    <REF HREF=\"http://example.com/stream&#46;mp3\" />\n"
        "  </ENTRY>\n"
        "  <ENTRY>\n"
        "    <TITLE>Local &lt;Clip&gt;</TITLE>\n"
        "    <REF HREF=\"C:\\Media\\A &amp; B.mp3\" />\n"
        "  </ENTRY>\n"
        "</ASX>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseASXContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::ASX);
    TEST_ASSERT_EQ(result.entries.size(), size_t(2));
    TEST_ASSERT(result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"http://example.com/stream.mp3"));
    TEST_ASSERT_EQ(result.entries[0].title, std::wstring(L"Stream AB"));
    TEST_ASSERT(!result.entries[1].isUrl);
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"C:\\Media\\A & B.mp3"));
    TEST_ASSERT_EQ(result.entries[1].title, std::wstring(L"Local <Clip>"));
}

TEST_CASE(PlaylistIO, ParseWplCollectsMediaSrcEntries) {
    const std::string content =
        "<?wpl version=\"1.0\"?>\n"
        "<smil>\n"
        "  <head><title>Sample</title></head>\n"
        "  <body>\n"
        "    <seq>\n"
        "      <media src=\"C:\\Music\\One.mp3\" />\n"
        "      <media src=\"http://example.com/two.wma\" />\n"
        "    </seq>\n"
        "  </body>\n"
        "</smil>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseWPLContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::WPL);
    TEST_ASSERT_EQ(result.entries.size(), size_t(2));
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"C:\\Music\\One.mp3"));
    TEST_ASSERT(!result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"http://example.com/two.wma"));
    TEST_ASSERT(result.entries[1].isUrl);
}

TEST_CASE(PlaylistIO, ParseXspfDecodesFileUrisAndKeepsHttpLocations) {
    const std::string content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<playlist xmlns=\"http://xspf.org/ns/0/\" version=\"1\">\n"
        "  <title>My Playlist</title>\n"
        "  <trackList>\n"
        "    <track><location>file:///C:/My%20Media/A%20%26%20B.mp3</location>"
        "<title>Local</title><duration>1500</duration></track>\n"
        "    <track><location>http://example.com/stream.mp3</location></track>\n"
        "  </trackList>\n"
        "</playlist>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseXSPFContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::XSPF);
    TEST_ASSERT_EQ(result.playlistTitle, std::wstring(L"My Playlist"));
    TEST_ASSERT_EQ(result.entries.size(), size_t(2));
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"C:\\My Media\\A & B.mp3"));
    TEST_ASSERT(!result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[0].title, std::wstring(L"Local"));
    TEST_ASSERT(std::abs(result.entries[0].durationSec - 1.5) < 1e-9);
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"http://example.com/stream.mp3"));
    TEST_ASSERT(result.entries[1].isUrl);
}

TEST_CASE(PlaylistIO, XspfSerializeParseRoundTripForLocalPathWithSpaceAndAmpersand) {
    std::vector<PlaylistEntry> entries;
    PlaylistEntry entry;
    entry.location = L"C:\\My Media\\A & B.mp3";
    entry.title = L"Cool & \"Loud\" <Mix>";
    entry.durationSec = 12.5;
    entry.isUrl = false;
    entries.push_back(entry);

    PlaylistExportOptions options;
    options.writeExtendedMetadata = true;
    const std::string xml = PlaylistIO::SerializeXSPF(entries, L"Round & Trip", options);
    TEST_ASSERT(xml.find("file:///C:/My%20Media/A%20%26%20B.mp3") != std::string::npos);
    TEST_ASSERT(xml.find("&amp;") != std::string::npos);

    PlaylistParseOptions parseOptions;
    parseOptions.checkFileExistence = false;
    auto result = PlaylistIO::ParseContent(xml, PlaylistFormat::XSPF, L"", parseOptions);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::XSPF);
    TEST_ASSERT_EQ(result.playlistTitle, std::wstring(L"Round & Trip"));
    TEST_ASSERT_EQ(result.entries.size(), size_t(1));
    TEST_ASSERT_EQ(result.entries[0].location, entry.location);
    TEST_ASSERT_EQ(result.entries[0].title, entry.title);
    TEST_ASSERT(std::abs(result.entries[0].durationSec - 12.5) < 1e-9);
}

TEST_CASE(PlaylistIO, ParseITunesXmlCollectsTrackDictionaries) {
    const std::string content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "  <key>Major Version</key><integer>1</integer>\n"
        "  <key>Tracks</key><dict>\n"
        "    <key>100</key><dict>\n"
        "      <key>Track ID</key><integer>100</integer>\n"
        "      <key>Name</key><string>Local &amp; Loud</string>\n"
        "      <key>Total Time</key><integer>150000</integer>\n"
        "      <key>Location</key><string>file:///C:/My%20Music/A%20%26%20B.mp3</string>\n"
        "    </dict>\n"
        "    <key>101</key><dict>\n"
        "      <key>Track ID</key><integer>101</integer>\n"
        "      <key>Name</key><string>Remote Stream</string>\n"
        "      <key>Location</key><string>http://example.com/stream.mp3</string>\n"
        "    </dict>\n"
        "  </dict>\n"
        "</dict>\n"
        "</plist>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseITunesXmlContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::ITunesXml);
    TEST_ASSERT_EQ(result.entries.size(), size_t(2));
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"C:\\My Music\\A & B.mp3"));
    TEST_ASSERT(!result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[0].title, std::wstring(L"Local & Loud"));
    TEST_ASSERT(std::abs(result.entries[0].durationSec - 150.0) < 1e-9);
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"http://example.com/stream.mp3"));
    TEST_ASSERT(result.entries[1].isUrl);
}

TEST_CASE(PlaylistIO, ParseB4SCollectsEntryPlaystringsAndTitles) {
    const std::string content =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<WinampXML>\n"
        "  <playlist num_entries=\"2\">\n"
        "    <entry Playstring=\"file:C:\\Music\\One.mp3\" Title=\"One\">\n"
        "      <Name>One Meta</Name>\n"
        "    </entry>\n"
        "    <entry Playstring=\"file:///C:/My%20Music/Two.mp3\">\n"
        "      <Name>Two</Name>\n"
        "    </entry>\n"
        "  </playlist>\n"
        "</WinampXML>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseB4SContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::B4S);
    TEST_ASSERT_EQ(result.entries.size(), size_t(2));
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"C:\\Music\\One.mp3"));
    TEST_ASSERT(!result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[0].title, std::wstring(L"One"));
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"C:\\My Music\\Two.mp3"));
    TEST_ASSERT(!result.entries[1].isUrl);
    TEST_ASSERT_EQ(result.entries[1].title, std::wstring(L"Two"));
}

TEST_CASE(PlaylistIO, ParseQtlCollectsEmbedSrcAndQtnSrc) {
    const std::string content =
        "<?xml version=\"1.0\"?>\n"
        "<?quicktime type=\"application/x-quicktime-media-link\"?>\n"
        "<embed src=\"C:\\Movies\\Clip.mov\" autoplay=\"true\" />\n"
        "<embed src=\"http://example.com/stream.mov\" />\n"
        "<qtn_src>C:\\Movies\\Other.mov</qtn_src>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto result = PlaylistIO::ParseQTLContent(content, L"", options);
    TEST_ASSERT(result.is_ok());
    TEST_ASSERT_EQ(result.detectedFormat, PlaylistFormat::QTL);
    TEST_ASSERT_EQ(result.entries.size(), size_t(3));
    TEST_ASSERT_EQ(result.entries[0].location, std::wstring(L"C:\\Movies\\Clip.mov"));
    TEST_ASSERT(!result.entries[0].isUrl);
    TEST_ASSERT_EQ(result.entries[1].location, std::wstring(L"http://example.com/stream.mov"));
    TEST_ASSERT(result.entries[1].isUrl);
    TEST_ASSERT_EQ(result.entries[2].location, std::wstring(L"C:\\Movies\\Other.mov"));
}

TEST_CASE(PlaylistIO, XmlExtensionSniffKeepsXspfOnPlainXmlPlaylists) {
    const std::string xspf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<playlist xmlns=\"http://xspf.org/ns/0/\" version=\"1\">\n"
        "  <trackList><track><location>file:///C:/Media/A.mp3</location></track></trackList>\n"
        "</playlist>\n";
    PlaylistParseOptions options;
    options.checkFileExistence = false;
    auto byContent = PlaylistIO::ParseContent(xspf, PlaylistFormat::Auto, L"", options);
    TEST_ASSERT(byContent.is_ok());
    TEST_ASSERT_EQ(byContent.detectedFormat, PlaylistFormat::XSPF);

    // The .xml extension guesses iTunes XML, but the plist sniff fails and the
    // parser falls back to content detection where XSPF wins.
    auto byExtension = PlaylistIO::ParseContent(xspf, PlaylistFormat::ITunesXml, L"", options);
    TEST_ASSERT(byExtension.is_ok());
    TEST_ASSERT_EQ(byExtension.detectedFormat, PlaylistFormat::XSPF);
    TEST_ASSERT_EQ(byExtension.entries.size(), size_t(1));
}

} // namespace Aperture::Test
