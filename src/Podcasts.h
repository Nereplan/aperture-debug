#pragma once

#include "AsyncOwnedJob.h"
#include "HttpTransport.h"
#include "Json.h"
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace Aperture {

struct PodcastEpisode {
    std::wstring title, url;
    int64_t publishedSec = 0;
    double durationSec = 0.0;
};

struct PodcastFeed {
    std::wstring url, title;
    std::vector<PodcastEpisode> episodes;
};

// Parse an RSS 2.0 podcast feed. The channel <title> comes from the header
// before the first <item>; each item contributes <title>, <enclosure url="...">
// (items without an http(s):// enclosure are skipped), <pubDate> (RFC822;
// unknown or malformed dates become 0) and <itunes:duration> ("H:MM:SS",
// "MM:SS", or plain seconds; 0 when absent or invalid). Namespaced tags are
// matched by their literal spelling (the parser sees "<itunes:duration>").
PodcastFeed ParseRssFeed(const std::string& utf8Xml, const std::wstring& feedUrl, size_t maxEpisodes = 100);

// Normalizes user-entered podcast URLs (trims whitespace/quotes, converts
// feed:// / podcast:// / itpc:// / pcast:// and scheme-less host/path strings
// to https://, lowercases http(s)://). Returns empty on invalid input.
std::wstring NormalizePodcastFeedUrl(const std::wstring& rawUrl);

// Podcast subscriptions persisted at <dataDir>\podcasts.json:
// { "schemaVersion": 1, "feeds": [ { "url": ..., "title": ... } ] }.
// Unknown JSON fields are ignored on load; a missing file yields an empty list.
class PodcastSubscriptions {
public:
    static constexpr int kCurrentSubscriptionsSchemaVersion = 1;
    static constexpr size_t kMaxFeeds = 50;
    static constexpr const wchar_t* kDefaultSubscriptionsFileName = L"podcasts.json";
    // Dedupe by URL; returns false when the feed is already subscribed.
    bool Add(const std::wstring& title, const std::wstring& url);
    bool Remove(const std::wstring& url);
    const std::vector<PodcastFeed>& GetFeeds() const { return feeds_; }
    StorageStatus Load(const std::wstring& dataDir, const std::wstring& fileName = kDefaultSubscriptionsFileName);
    StorageStatus Save(const std::wstring& dataDir, const std::wstring& fileName = kDefaultSubscriptionsFileName);

private:
    std::vector<PodcastFeed> feeds_; // episodes are unused in subscriptions; only url/title persist
};

struct PodcastFeedFailure {
    std::wstring url, reason;
};

struct PodcastRefreshResult {
    bool success = false; // at least one feed was obtained (fresh network or cache)
    std::vector<PodcastFeed> feeds;
    std::vector<PodcastFeedFailure> failures;
};

struct PodcastAddPreviewResult {
    bool success = false; // parsed feed with a title and at least one episode
    PodcastFeed feed;
    std::wstring errorMessage;
};

// Fetch orchestration for podcast feeds. Each feed URL is fetched sequentially
// (http or https), capped at 24 MB and 15 s per feed, with overall cancellation.
// Each response is cached under <root>\cache\podcasts\<sanitized-url>.xml with
// a 6 h freshness wrapper { "cachedAtSec": ..., "body": ... }; stale entries
// still serve as a fallback when a refresh fails.
class PodcastRefreshService {
public:
    static constexpr int kFetchTimeoutMs = 15000;                      // 15 s per-feed network timeout
    static constexpr size_t kMaxResponseBytes = 24 * 1024 * 1024;      // 24 MB ceiling for large multi-year RSS feeds
    static constexpr int64_t kCacheFreshnessSec = 6LL * 60LL * 60LL;   // 6 h freshness stamp
    static std::wstring GetCachePath(const std::wstring& feedUrl, const std::wstring& root = GetExeDir());
    // Cache wrapper { "cachedAtSec": ..., "body": ... } shared with PodcastJobs.
    static bool LoadPodcastCache(const std::wstring& path, std::string& outBody, bool& outFresh);
    static StorageStatus SavePodcastCache(const std::wstring& path, const std::string& body, int64_t nowSec);
    // Cheap UI-side probe (file timestamps only): true when any feed cache is
    // missing or older than kCacheFreshnessSec.
    static bool AnyCacheStale(const std::vector<std::wstring>& feedUrls, const std::wstring& root = GetExeDir());
    static PodcastAddPreviewResult FetchSingleFeed(const std::wstring& feedUrl, const std::wstring& root = GetExeDir(),
        IHttpTransport* customTransport = nullptr, std::shared_ptr<OperationCancellation> cancellation = {});
    static PodcastRefreshResult FetchFeeds(const std::vector<std::wstring>& feedUrls, const std::wstring& root = GetExeDir(),
        IHttpTransport* customTransport = nullptr, std::shared_ptr<OperationCancellation> cancellation = {});
};

// UI-thread controller following the RadioDirectoryJobs member pattern: one
// owned worker with a replaceable request and one consumed completion, plus a
// second independent job for the add-feed preview fetch.
class PodcastJobs {
public:
    enum class FetchState { Idle, Loading, Ready };
    // Starts a refresh unless one is already loading or every feed still has a
    // fresh cache; the caller caps invocations at once per surface-open.
    void EnsureRefresh(const std::vector<std::wstring>& feedUrls, const std::wstring& root = GetExeDir(),
        IHttpTransport* testTransport = nullptr) {
        if (state_ == FetchState::Loading) return;
        if (state_ == FetchState::Ready && !PodcastRefreshService::AnyCacheStale(feedUrls, root)) return;
        state_ = FetchState::Loading;
        refresh_.Start([feedUrls, root, testTransport](const auto& cancellation) {
            return PodcastRefreshService::FetchFeeds(feedUrls, root, testTransport, cancellation);
        });
    }
    // Single-feed fetch used by the Add Podcast modal; never skipped (the modal
    // must learn whether the candidate URL parses).
    void EnsureAddPreview(const std::wstring& feedUrl, const std::wstring& root = GetExeDir(),
        IHttpTransport* testTransport = nullptr) {
        addPreview_.Start([feedUrl, root, testTransport](const auto& cancellation) {
            return PodcastRefreshService::FetchSingleFeed(feedUrl, root, testTransport, cancellation);
        });
    }
    // Returns true once when the refresh job completes; state becomes Ready.
    bool Pump() {
        auto completion = refresh_.TakeResult();
        if (!completion) return false;
        if (completion->result) result_ = std::move(*completion->result);
        else { result_ = {}; result_.failures.push_back({ L"", Utf8ToUtf16(completion->error) }); }
        state_ = FetchState::Ready; return true;
    }
    // Returns true once when the add-preview job completes.
    bool PumpAddPreview(PodcastAddPreviewResult& outResult) {
        auto completion = addPreview_.TakeResult();
        if (!completion) return false;
        if (completion->result) outResult = std::move(*completion->result);
        else outResult = PodcastAddPreviewResult{ false, {}, Utf8ToUtf16(completion->error) };
        return true;
    }
    void Cancel() { refresh_.Cancel(); addPreview_.Cancel(); }
    void StopAndJoin() { refresh_.StopAndJoin(); addPreview_.StopAndJoin(); }
    bool IsRunning() const { return refresh_.IsRunning() || addPreview_.IsRunning(); }
    FetchState GetFetchState() const { return state_; }
    const PodcastRefreshResult& GetResult() const { return result_; }
private:
    FetchState state_ = FetchState::Idle;
    PodcastRefreshResult result_;
    AsyncOwnedJob<PodcastRefreshResult> refresh_;
    AsyncOwnedJob<PodcastAddPreviewResult> addPreview_;
};

} // namespace Aperture
