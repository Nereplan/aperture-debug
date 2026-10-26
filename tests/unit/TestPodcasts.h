#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Podcasts.h"
#include "../../src/Json.h"
#include <string>

namespace Aperture::Test {

TEST_CASE(Podcasts, ParseRssFeedHappyPath) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\">\n"
        "  <channel>\n"
        "    <title>Example Podcast</title>\n"
        "    <item>\n"
        "      <title>Episode One</title>\n"
        "      <enclosure url=\"https://cdn.example.com/ep1.mp3\" type=\"audio/mpeg\" length=\"1024\"/>\n"
        "      <pubDate>Wed, 02 Oct 2024 14:30:00 +0200</pubDate>\n"
        "      <itunes:duration>1:02:03</itunes:duration>\n"
        "    </item>\n"
        "    <item>\n"
        "      <title>Episode Two</title>\n"
        "      <enclosure url=\"https://cdn.example.com/ep2.mp3\" type=\"audio/mpeg\" length=\"2048\"/>\n"
        "      <pubDate>Tue, 24 Sep 2024 10:00:00 GMT</pubDate>\n"
        "      <itunes:duration>185</itunes:duration>\n"
        "    </item>\n"
        "  </channel>\n"
        "</rss>";
    const PodcastFeed feed = ParseRssFeed(xml, L"https://example.com/feed.xml");
    TEST_ASSERT_EQ(feed.title, std::wstring(L"Example Podcast"));
    TEST_ASSERT_EQ(feed.url, std::wstring(L"https://example.com/feed.xml"));
    TEST_ASSERT_EQ(feed.episodes.size(), size_t(2));
    TEST_ASSERT_EQ(feed.episodes[0].title, std::wstring(L"Episode One"));
    TEST_ASSERT_EQ(feed.episodes[0].url, std::wstring(L"https://cdn.example.com/ep1.mp3"));
    TEST_ASSERT_EQ(feed.episodes[0].durationSec, 3723.0); // "1:02:03"
    // 2024-10-02 14:30:00 +0200 == 2024-10-02 12:30:00 UTC.
    TEST_ASSERT_EQ(feed.episodes[0].publishedSec, static_cast<int64_t>(1727872200));
    TEST_ASSERT_EQ(feed.episodes[1].title, std::wstring(L"Episode Two"));
    TEST_ASSERT_EQ(feed.episodes[1].url, std::wstring(L"https://cdn.example.com/ep2.mp3"));
    TEST_ASSERT_EQ(feed.episodes[1].durationSec, 185.0); // plain seconds
    // 2024-09-24 10:00:00 GMT.
    TEST_ASSERT_EQ(feed.episodes[1].publishedSec, static_cast<int64_t>(1727172000));
}

TEST_CASE(Podcasts, ParseRssFeedSkipsItemsWithoutHttpEnclosure) {
    const std::string xml =
        "<rss><channel><title>Feed &amp; Stuff</title>"
        "<item><title>No Enclosure</title><pubDate>not a date</pubDate></item>"
        "<item><title>Ftp Only</title><enclosure url=\"ftp://cdn.example.com/ep.mp3\"/></item>"
        "<item><title>Keep &lt;This&gt;</title><enclosure url='http://cdn.example.com/ep3.mp3'/></item>"
        "</channel></rss>";
    const PodcastFeed feed = ParseRssFeed(xml, L"https://example.com/feed.xml");
    TEST_ASSERT_EQ(feed.title, std::wstring(L"Feed & Stuff"));
    TEST_ASSERT_EQ(feed.episodes.size(), size_t(1));
    TEST_ASSERT_EQ(feed.episodes[0].title, std::wstring(L"Keep <This>"));
    TEST_ASSERT_EQ(feed.episodes[0].url, std::wstring(L"http://cdn.example.com/ep3.mp3"));
    // Items without a parseable pubDate or itunes:duration fall back to zero.
    TEST_ASSERT_EQ(feed.episodes[0].publishedSec, int64_t(0));
    TEST_ASSERT_EQ(feed.episodes[0].durationSec, 0.0);
}

TEST_CASE(Podcasts, ParseRssFeedRejectsGarbage) {
    TEST_ASSERT(ParseRssFeed("", L"https://example.com/feed.xml").episodes.empty());
    TEST_ASSERT(ParseRssFeed("not xml at all", L"https://example.com/feed.xml").title.empty());
    TEST_ASSERT(ParseRssFeed("<html><body>no channel here</body></html>", L"https://example.com/feed.xml").title.empty());
    TEST_ASSERT(ParseRssFeed("<rss><channel><title>Empty</title></channel></rss>", L"https://example.com/feed.xml").episodes.empty());
}

TEST_CASE(Podcasts, SubscriptionsAddDedupeRemoveAndRoundTrip) {
    ScopedTempDir root;
    PodcastSubscriptions store;
    TEST_ASSERT(store.Add(L"Example Podcast", L"https://example.com/feed.xml"));
    TEST_ASSERT(!store.Add(L"Different Title", L"https://example.com/feed.xml")); // dedupe by URL keeps the first title
    TEST_ASSERT(store.Add(L"Second", L"https://example.org/two.xml"));
    TEST_ASSERT_EQ(store.GetFeeds().size(), size_t(2));
    TEST_ASSERT_EQ(store.GetFeeds()[0].title, std::wstring(L"Example Podcast"));
    TEST_ASSERT(store.Remove(L"https://example.com/feed.xml"));
    TEST_ASSERT(!store.Remove(L"https://missing.example/feed.xml"));
    TEST_ASSERT_EQ(store.GetFeeds().size(), size_t(1));

    TEST_ASSERT(store.Save(root.GetPath()).is_ok());
    const auto file = root.SubPath(PodcastSubscriptions::kDefaultSubscriptionsFileName);
    std::string saved;
    TEST_ASSERT(ReadTextFile(file, saved).is_ok());
    JsonValue json;
    TEST_ASSERT(JsonValue::parse(saved, json));
    TEST_ASSERT_EQ(json.get("schemaVersion").as_int64(0), int64_t(1));
    TEST_ASSERT_EQ(json.get("feeds").as_array().size(), size_t(1));
    TEST_ASSERT_EQ(json.get("feeds")[0].get("url").as_string(), "https://example.org/two.xml");
    TEST_ASSERT_EQ(json.get("feeds")[0].get("title").as_string(), "Second");

    PodcastSubscriptions restarted;
    TEST_ASSERT(restarted.Load(root.GetPath()).is_ok());
    TEST_ASSERT_EQ(restarted.GetFeeds().size(), size_t(1));
    TEST_ASSERT_EQ(restarted.GetFeeds()[0].title, std::wstring(L"Second"));
    TEST_ASSERT_EQ(restarted.GetFeeds()[0].url, std::wstring(L"https://example.org/two.xml"));
    TEST_ASSERT(restarted.Save(root.GetPath()).is_ok());
    std::string roundtrip;
    TEST_ASSERT(ReadTextFile(file, roundtrip).is_ok());
    TEST_ASSERT_EQ(saved, roundtrip);
}

TEST_CASE(Podcasts, SubscriptionsTolerateUnknownFieldsAndMissingFile) {
    ScopedTempDir root;
    // Unknown/malformed entries are skipped; a URL-only entry falls back to its
    // URL as the title (same rule as Add).
    const auto file = root.SubPath(PodcastSubscriptions::kDefaultSubscriptionsFileName);
    TEST_ASSERT(AtomicWriteTextFile(file,
        "{\"schemaVersion\":1,\"feeds\":[{\"url\":\"https://example.org/keep.xml\",\"title\":\"Keep\",\"extra\":123},"
        "{\"noUrl\":true},{\"url\":\"\"},{\"url\":\"https://example.org/untitled.xml\"}]}").is_ok());
    PodcastSubscriptions store;
    TEST_ASSERT(store.Load(root.GetPath()).is_ok());
    TEST_ASSERT_EQ(store.GetFeeds().size(), size_t(2));
    TEST_ASSERT_EQ(store.GetFeeds()[0].url, std::wstring(L"https://example.org/keep.xml"));
    TEST_ASSERT_EQ(store.GetFeeds()[0].title, std::wstring(L"Keep"));
    TEST_ASSERT_EQ(store.GetFeeds()[1].title, std::wstring(L"https://example.org/untitled.xml"));

    // A missing subscriptions file is the pristine state, not an error.
    PodcastSubscriptions pristine;
    TEST_ASSERT(pristine.Load(root.GetPath(), L"missing_podcasts.json").is_ok());
    TEST_ASSERT(pristine.GetFeeds().empty());
}

TEST_CASE(Podcasts, NormalizeUrlAndCdataRssParsing) {
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"  HTTPS://Example.com/feed.xml  "), std::wstring(L"https://Example.com/feed.xml"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"http://feeds.example.com/rss"), std::wstring(L"http://feeds.example.com/rss"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"feed://feeds.example.com/show.xml"), std::wstring(L"https://feeds.example.com/show.xml"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"podcast://feeds.example.com/show.xml"), std::wstring(L"https://feeds.example.com/show.xml"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"itpc://feeds.example.com/show.xml"), std::wstring(L"https://feeds.example.com/show.xml"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"feed:http://feeds.example.com/show.xml"), std::wstring(L"http://feeds.example.com/show.xml"));
    TEST_ASSERT_EQ(NormalizePodcastFeedUrl(L"feeds.example.com/podcast.rss"), std::wstring(L"https://feeds.example.com/podcast.rss"));
    TEST_ASSERT(NormalizePodcastFeedUrl(L"").empty());
    TEST_ASSERT(NormalizePodcastFeedUrl(L"ftp://feeds.example.com/podcast.rss").empty());

    const std::string cdataXml =
        "<rss version=\"2.0\" xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\" xmlns:media=\"http://search.yahoo.com/mrss/\">"
        "<channel>"
        "<title><![CDATA[The Daily &amp; Special Show]]></title>"
        "<item>"
        "<title><![CDATA[Episode #1: Launch]]></title>"
        "<media:content url=\"HTTPS://cdn.example.com/ep1.mp3\" type=\"audio/mpeg\"/>"
        "</item>"
        "<item>"
        "<itunes:title>Fallback Title</itunes:title>"
        "<link rel=\"enclosure\" href=\"http://cdn.example.com/ep2.m4a\"/>"
        "</item>"
        "</channel></rss>";
    const PodcastFeed feed = ParseRssFeed(cdataXml, L"https://feeds.example.com/show.xml");
    TEST_ASSERT_EQ(feed.title, std::wstring(L"The Daily & Special Show"));
    TEST_ASSERT_EQ(feed.episodes.size(), size_t(2));
    TEST_ASSERT_EQ(feed.episodes[0].title, std::wstring(L"Episode #1: Launch"));
    TEST_ASSERT_EQ(feed.episodes[0].url, std::wstring(L"https://cdn.example.com/ep1.mp3"));
    TEST_ASSERT_EQ(feed.episodes[1].title, std::wstring(L"Fallback Title"));
    TEST_ASSERT_EQ(feed.episodes[1].url, std::wstring(L"http://cdn.example.com/ep2.m4a"));
}

} // namespace Aperture::Test
