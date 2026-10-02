#pragma once

#include "AsyncOwnedJob.h"
#include "OnlineSubtitles.h"

namespace Aperture {

// UI-thread controller. Its workers operate on copied request data only. A
// context change invalidates both jobs; one bounded search cache survives menu
// close/reopen and download completion for the same media/provider/language.
class OnlineSubtitleJobs {
public:
    enum class SearchState { Idle, Loading, Ready };
    bool SyncContext(const std::wstring& media, const Settings& settings, uint64_t mediaGeneration = 0, bool privateMode = false) {
        if (media_ == media && endpoint_ == settings.onlineSubtitleEndpoint && language_ == settings.subLang &&
            enabled_ == settings.onlineSubtitlesEnabled && mediaGeneration_ == mediaGeneration && privateMode_ == privateMode) return false;
        Cancel(); media_ = media; endpoint_ = settings.onlineSubtitleEndpoint;
        language_ = settings.subLang; enabled_ = settings.onlineSubtitlesEnabled;
        mediaGeneration_ = mediaGeneration; privateMode_ = privateMode;
        state_ = SearchState::Idle; searchResult_ = {}; return true;
    }
    void EnsureSearch(const Settings& settings, IHttpTransport* testTransport = nullptr) {
        if (state_ != SearchState::Idle || !enabled_ || media_.empty()) return;
        state_ = SearchState::Loading;
        search_.Start([media = media_, settings, testTransport](const auto& cancellation) {
            OnlineSubtitleService service;
            return service.Search(media, L"", settings, testTransport, cancellation);
        });
    }
    void RetrySearch(const Settings& settings) {
        search_.Cancel(); state_ = SearchState::Idle; searchResult_ = {}; EnsureSearch(settings);
    }
    bool StartDownload(const OnlineSubtitleSearchResult& item, const std::wstring& expectedMedia, const Settings& settings,
                       const std::wstring& root = GetExeDir(), IHttpTransport* testTransport = nullptr) {
        if (media_ != expectedMedia || !enabled_ || !settings.onlineSubtitlesEnabled || downloading_ ||
            endpoint_ != settings.onlineSubtitleEndpoint || language_ != settings.subLang) return false;
        downloading_ = true;
        download_.Start([item, media = media_, settings, root, testTransport](const auto& cancellation) {
            OnlineSubtitleService service;
            return service.DownloadAndLoad(item, media, settings, testTransport, root, cancellation);
        });
        return true;
    }
    bool PumpSearch() {
        auto completion = search_.TakeResult();
        if (!completion) return false;
        if (completion->result) searchResult_ = std::move(*completion->result);
        else { searchResult_ = {}; searchResult_.status = OnlineSubtitleStatus::NetworkError;
               searchResult_.errorMessage = Utf8ToUtf16(completion->error); }
        state_ = SearchState::Ready; return true;
    }
    std::optional<OnlineSubtitleResult> TakeDownload() {
        auto completion = download_.TakeResult();
        if (!completion) return {};
        downloading_ = false;
        if (completion->result) return std::move(completion->result);
        OnlineSubtitleResult error; error.status = OnlineSubtitleStatus::NetworkError;
        error.errorMessage = Utf8ToUtf16(completion->error); return error;
    }
    void Cancel() { search_.Cancel(); download_.Cancel(); downloading_ = false; }
    void StopAndJoin() { search_.StopAndJoin(); download_.StopAndJoin(); }
    bool IsRunning() const { return search_.IsRunning() || download_.IsRunning(); }
    bool IsDownloading() const { return downloading_; }
    SearchState GetSearchState() const { return state_; }
    const OnlineSubtitleResult& GetSearchResult() const { return searchResult_; }
    const std::wstring& GetMedia() const { return media_; }
private:
    std::wstring media_;
    std::string endpoint_, language_;
    uint64_t mediaGeneration_ = 0;
    bool enabled_ = false, downloading_ = false, privateMode_ = false;
    SearchState state_ = SearchState::Idle;
    OnlineSubtitleResult searchResult_;
    AsyncOwnedJob<OnlineSubtitleResult> search_, download_;
};

} // namespace Aperture
