#pragma once

#include "AsyncOwnedJob.h"
#include "HttpTransport.h"
#include "Json.h"
#include "StorageStatus.h"
#include <string>
#include <vector>
#include <cstdint>

namespace Aperture {

struct RadioStation {
    std::wstring name, url;
    int bitrateKbps = 0;
    std::wstring codec, country;
};

// Parse the Radio-Browser /json/stations response: an array of station
// objects. Names are trimmed to 64 characters, URLs must start with http:// or
// https:// (entries are skipped otherwise and deduplicated by URL), and empty
// names fall back to "Unnamed station".
std::vector<RadioStation> ParseRadioBrowserJson(const std::string& json, size_t maxStations = 60);

// Percent-encode a wide string's UTF-8 bytes for use as one URL path segment
// (unreserved characters pass through, everything else becomes %XX).
std::string PercentEncodeUrlPathUtf8(const std::wstring& text);

// Parse a batch station import source: M3U/M3U8 playlists (#EXTINF names),
// PLS playlists (FileN=/TitleN=), or a plain URL-per-line text file. Only
// http(s) URLs are kept; entries are deduplicated by URL. Blank lines and
// '#' comments are ignored.
std::vector<RadioStation> ParseRadioPlaylistText(const std::string& utf8Text, size_t maxStations = 200);

struct RadioDirectoryResult {
    bool success = false;
    bool fromCache = false; // served from the 24 h cache (fresh or stale fallback)
    bool stale = false;     // network refresh failed and a stale cache was used
    std::vector<RadioStation> stations;
    std::wstring errorMessage;
};

struct RadioSearchResult {
    bool success = false;
    std::wstring query; // trimmed query this result belongs to
    std::vector<RadioStation> stations;
    std::wstring errorMessage;
};

// Fetch orchestration for the internet radio directory. The response is cached
// atomically under <root>\cache\radio\stations.json with a 24 h freshness
// stamp; stale entries still serve as a fallback when a refresh fails.
class InternetRadioService {
public:
    static constexpr wchar_t kDirectoryUrl[] =
        L"https://de1.api.radio-browser.info/json/stations?limit=60&order=clickcount&reverse=true&hidebroken=true";
    static constexpr int kFetchTimeoutMs = 10000;                // 10 s network timeout
    static constexpr size_t kMaxResponseBytes = 1024 * 1024;     // 1 MB; the station list is ~100 KB
    static constexpr int64_t kCacheFreshnessSec = 24LL * 60LL * 60LL; // 24 h freshness stamp
    static std::wstring GetCachePath(const std::wstring& root = GetExeDir());
    RadioDirectoryResult FetchDirectory( const std::wstring& root = GetExeDir(),
        IHttpTransport* customTransport = nullptr, std::shared_ptr<OperationCancellation> cancellation = {});
    // Exposed for tests: the byname search URL for one query (percent-encoded).
    static std::wstring BuildSearchUrl(const std::wstring& query);
    // Name search against radio-browser's /byname endpoint. Results are not
    // cached on disk: queries are cheap, short-lived, and user-typed.
    RadioSearchResult SearchStations( const std::wstring& query,
        IHttpTransport* customTransport = nullptr, std::shared_ptr<OperationCancellation> cancellation = {});
};

// UI-thread controller following the OnlineSubtitleJobs member pattern: one
// owned worker with a replaceable request and one consumed completion, plus a
// second independent job for name searches (a new Start replaces the pending
// request, so rapid typing only pays for the newest query).
class RadioDirectoryJobs {
public:
    enum class FetchState { Idle, Loading, Ready };
    void EnsureFetch(const std::wstring& root = GetExeDir(), IHttpTransport* testTransport = nullptr) {
        if (state_ != FetchState::Idle) return;
        state_ = FetchState::Loading;
        fetch_.Start([root, testTransport](const auto& cancellation) {
            InternetRadioService service;
            return service.FetchDirectory(root, testTransport, cancellation);
        });
    }
    // User-triggered refresh: re-runs even after a completed fetch (the 24 h
    // cache still short-circuits the network inside FetchDirectory when fresh,
    // so this is cheap when nothing changed).
    void ForceFetch(const std::wstring& root = GetExeDir(), IHttpTransport* testTransport = nullptr) {
        if (state_ == FetchState::Loading) return;
        state_ = FetchState::Loading;
        fetch_.Start([root, testTransport](const auto& cancellation) {
            InternetRadioService service;
            return service.FetchDirectory(root, testTransport, cancellation);
        });
    }
    bool Pump() {
        auto completion = fetch_.TakeResult();
        if (!completion) return false;
        if (completion->result) result_ = std::move(*completion->result);
        else { result_ = {}; result_.errorMessage = Utf8ToUtf16(completion->error); }
        state_ = FetchState::Ready; return true;
    }
    // Name search: the caller debounces; each EnsureSearch replaces any
    // pending/running search. PumpSearch returns true once per completion.
    void EnsureSearch(const std::wstring& query, IHttpTransport* testTransport = nullptr) {
        search_.Start([query, testTransport](const auto& cancellation) {
            InternetRadioService service;
            return service.SearchStations(query, testTransport, cancellation);
        });
    }
    void CancelSearch() { search_.Cancel(); }
    bool PumpSearch(RadioSearchResult& outResult) {
        auto completion = search_.TakeResult();
        if (!completion) return false;
        if (completion->result) outResult = std::move(*completion->result);
        else outResult = RadioSearchResult{ false, L"", {}, Utf8ToUtf16(completion->error) };
        return true;
    }
    bool IsSearchRunning() const { return search_.IsRunning(); }
    void Cancel() { fetch_.Cancel(); search_.Cancel(); }
    void StopAndJoin() { fetch_.StopAndJoin(); search_.StopAndJoin(); }
    bool IsRunning() const { return fetch_.IsRunning(); }
    FetchState GetFetchState() const { return state_; }
    const RadioDirectoryResult& GetResult() const { return result_; }
private:
    FetchState state_ = FetchState::Idle;
    RadioDirectoryResult result_;
    AsyncOwnedJob<RadioDirectoryResult> fetch_;
    AsyncOwnedJob<RadioSearchResult> search_;
};

// Persisted favorite stations at <dataDir>\radio.json:
// { "schemaVersion": 1, "stations": [ { "name", "url", "bitrateKbps",
// "codec", "country" } ] }. Deduplicated by URL; unknown JSON fields are
// ignored on load and a missing file yields an empty list.
class RadioFavorites {
public:
    static constexpr int kCurrentSchemaVersion = 1;
    static constexpr size_t kMaxStations = 100;
    static constexpr const wchar_t* kDefaultFileName = L"radio.json";
    // Returns false when the station was already saved. Entries are kept in
    // insertion order (most recently added last).
    bool Add(const RadioStation& station);
    bool Remove(const std::wstring& url);
    bool Contains(const std::wstring& url) const;
    const std::vector<RadioStation>& GetStations() const { return stations_; }
    StorageStatus Load(const std::wstring& dataDir, const std::wstring& fileName = kDefaultFileName);
    StorageStatus Save(const std::wstring& dataDir, const std::wstring& fileName = kDefaultFileName);

private:
    std::vector<RadioStation> stations_;
};

} // namespace Aperture
