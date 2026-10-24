#pragma once

#include "AsyncOwnedJob.h"
#include "HttpTransport.h"
#include "Json.h"
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

struct RadioDirectoryResult {
    bool success = false;
    bool fromCache = false; // served from the 24 h cache (fresh or stale fallback)
    bool stale = false;     // network refresh failed and a stale cache was used
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
};

// UI-thread controller following the OnlineSubtitleJobs member pattern: one
// owned worker with a replaceable request and one consumed completion.
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
    bool Pump() {
        auto completion = fetch_.TakeResult();
        if (!completion) return false;
        if (completion->result) result_ = std::move(*completion->result);
        else { result_ = {}; result_.errorMessage = Utf8ToUtf16(completion->error); }
        state_ = FetchState::Ready; return true;
    }
    void Cancel() { fetch_.Cancel(); }
    void StopAndJoin() { fetch_.StopAndJoin(); }
    bool IsRunning() const { return fetch_.IsRunning(); }
    FetchState GetFetchState() const { return state_; }
    const RadioDirectoryResult& GetResult() const { return result_; }
private:
    FetchState state_ = FetchState::Idle;
    RadioDirectoryResult result_;
    AsyncOwnedJob<RadioDirectoryResult> fetch_;
};

} // namespace Aperture
