#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/OnlineSubtitleJobs.h"
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

namespace Aperture::Test {
namespace AsyncNetworkTest {
template<class Condition> bool WaitUntil(Condition&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do { if (condition()) return true; Sleep(1); } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
inline Settings SubtitleSettings() {
    Settings settings; settings.onlineSubtitlesEnabled = true;
    settings.onlineSubtitleEndpoint = "https://provider.example/search?q={query}&lang={lang}";
    settings.subLang = "eng"; return settings;
}
inline HttpResponse SearchResponse(const char* title = "Subtitle") {
    HttpResponse response; response.success = true; response.statusCode = 200;
    response.body = std::string("[{\"id\":\"one\",\"title\":\"") + title +
        "\",\"language\":\"eng\",\"url\":\"https://provider.example/subtitle.srt\"}]";
    return response;
}
inline HttpResponse SubtitleResponse() {
    HttpResponse response; response.success = true; response.statusCode = 200;
    response.body = "1\n00:00:01,000 --> 00:00:03,000\nSubtitle text\n\n"; return response;
}
}

TEST_CASE(AsyncNetworkJobs, SearchReturnsWhileTransportBlockedAndCachesAcrossDownload) {
    using namespace AsyncNetworkTest;
    ScopedTempDir temp; MockHttpTransport transport;
    std::promise<void> entered, release; auto released = release.get_future().share();
    std::atomic<int> searches{0}, downloads{0};
    transport.SetHandler(L"https://provider.example/search", [&](const auto&, const auto& options) {
        ++searches; TEST_ASSERT(options.cancellation != nullptr); entered.set_value(); released.wait(); return SearchResponse();
    });
    transport.SetHandler(L"https://provider.example/subtitle", [&](const auto&, const auto&) { ++downloads; return SubtitleResponse(); });
    const auto settings = SubtitleSettings(); OnlineSubtitleJobs jobs;
    jobs.SyncContext(L"C:\\Media\\Movie.mp4", settings); jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    TEST_ASSERT_EQ(jobs.GetSearchState(), OnlineSubtitleJobs::SearchState::Loading);
    TEST_ASSERT(!jobs.PumpSearch());
    jobs.EnsureSearch(settings, &transport); // Reopening the menu must not issue another request.
    release.set_value();
    TEST_ASSERT(WaitUntil([&] { return jobs.PumpSearch(); }));
    TEST_ASSERT(jobs.GetSearchResult().success);
    jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT(jobs.StartDownload(jobs.GetSearchResult().searchResults.front(), jobs.GetMedia(), settings, temp.GetPath(), &transport));
    std::optional<OnlineSubtitleResult> download;
    TEST_ASSERT(WaitUntil([&] { download = jobs.TakeDownload(); return download.has_value(); }));
    TEST_ASSERT(download->success && FileExists(download->savedFilePath));
    jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT_EQ(searches.load(), 1); TEST_ASSERT_EQ(downloads.load(), 1);
    TEST_ASSERT_EQ(jobs.GetSearchResult().searchResults.front().id, std::string("one"));
}

TEST_CASE(AsyncNetworkJobs, SupersededMediaRejectsAnIgnoringTransportCompletion) {
    using namespace AsyncNetworkTest;
    MockHttpTransport transport;
    std::promise<void> entered, release; auto released = release.get_future().share();
    std::atomic<int> searches{0};
    transport.SetHandler(L"https://provider.example/search", [&](const auto& url, const auto&) {
        ++searches;
        if (url.find(L"Old") != std::wstring::npos) { entered.set_value(); released.wait(); return SearchResponse("Old result"); }
        return SearchResponse("Current result");
    });
    const auto settings = SubtitleSettings(); OnlineSubtitleJobs jobs;
    jobs.SyncContext(L"C:\\Media\\Old.mp4", settings); jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    jobs.SyncContext(L"C:\\Media\\Current.mp4", settings); jobs.EnsureSearch(settings, &transport);
    release.set_value();
    TEST_ASSERT(WaitUntil([&] { return jobs.PumpSearch(); }));
    TEST_ASSERT_EQ(jobs.GetSearchResult().searchResults.front().title, std::wstring(L"Current result"));
    TEST_ASSERT_EQ(searches.load(), 2);
}

TEST_CASE(AsyncNetworkJobs, SupersededDownloadCannotStoreOrApplyToNewMedia) {
    using namespace AsyncNetworkTest;
    ScopedTempDir temp; MockHttpTransport transport;
    std::promise<void> entered, release; auto released = release.get_future().share();
    transport.SetHandler(L"https://provider.example/subtitle", [&](const auto&, const auto&) {
        entered.set_value(); released.wait(); return SubtitleResponse();
    });
    const auto settings = SubtitleSettings(); OnlineSubtitleJobs jobs;
    jobs.SyncContext(L"C:\\Media\\Old.mp4", settings);
    OnlineSubtitleSearchResult item; item.downloadUrl = "https://provider.example/subtitle.srt"; item.id = "old";
    TEST_ASSERT(jobs.StartDownload(item, jobs.GetMedia(), settings, temp.GetPath(), &transport));
    TEST_ASSERT(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    jobs.SyncContext(L"C:\\Media\\Current.mp4", settings); release.set_value();
    TEST_ASSERT(WaitUntil([&] { return !jobs.IsRunning(); }));
    TEST_ASSERT(!jobs.TakeDownload());
    TEST_ASSERT(!DirectoryExists(temp.SubPath(L"data\\subtitles")));
    TEST_ASSERT(!jobs.StartDownload(item, L"C:\\Media\\Old.mp4", settings, temp.GetPath(), &transport));
}

TEST_CASE(AsyncNetworkJobs, CancellationInterruptAndWorkerExceptionAreOwned) {
    using namespace AsyncNetworkTest;
    struct Signal { std::mutex mutex; std::condition_variable cv; bool interrupted = false; };
    auto signal = std::make_shared<Signal>(); std::promise<void> entered;
    AsyncOwnedJob<int> job;
    job.Start([signal, &entered](const auto& cancellation) {
        cancellation->SetInterrupt([signal] { std::lock_guard<std::mutex> lock(signal->mutex);
            signal->interrupted = true; signal->cv.notify_all(); });
        entered.set_value(); std::unique_lock<std::mutex> lock(signal->mutex);
        signal->cv.wait(lock, [&] { return signal->interrupted; }); return 42;
    });
    TEST_ASSERT(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    job.Cancel(); TEST_ASSERT(WaitUntil([&] { return !job.IsRunning(); })); TEST_ASSERT(!job.TakeResult());
    job.Start([](const auto&) -> int { throw std::runtime_error("Injected network failure"); });
    std::optional<AsyncOwnedJob<int>::Completion> result;
    TEST_ASSERT(WaitUntil([&] { result = job.TakeResult(); return result.has_value(); }));
    TEST_ASSERT(!result->result); TEST_ASSERT_EQ(result->error, std::string("Injected network failure"));
    job.Start([](const auto&) { return 7; });
    TEST_ASSERT(WaitUntil([&] { result = job.TakeResult(); return result.has_value(); }));
    TEST_ASSERT_EQ(*result->result, 7);
}

TEST_CASE(AsyncNetworkJobs, ProviderLanguageAndOptOutInvalidateSearchCache) {
    using namespace AsyncNetworkTest;
    MockHttpTransport transport; std::atomic<int> calls{0};
    transport.SetHandler(L"https://provider.example", [&](const auto&, const auto&) { ++calls; return SearchResponse(); });
    auto settings = SubtitleSettings(); OnlineSubtitleJobs jobs;
    jobs.SyncContext(L"C:\\Media\\Movie.mp4", settings); jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT(WaitUntil([&] { return jobs.PumpSearch(); }));
    settings.subLang = "tur"; TEST_ASSERT(jobs.SyncContext(jobs.GetMedia(), settings)); jobs.EnsureSearch(settings, &transport);
    TEST_ASSERT(WaitUntil([&] { return jobs.PumpSearch(); })); TEST_ASSERT_EQ(calls.load(), 2);
    settings.onlineSubtitlesEnabled = false; TEST_ASSERT(jobs.SyncContext(jobs.GetMedia(), settings));
    jobs.EnsureSearch(settings, &transport); TEST_ASSERT_EQ(jobs.GetSearchState(), OnlineSubtitleJobs::SearchState::Idle);
    TEST_ASSERT_EQ(calls.load(), 2);
}

TEST_CASE(AsyncNetworkJobs, PreCancelledServiceSkipsTransportAndStorage) {
    using namespace AsyncNetworkTest;
    ScopedTempDir temp; MockHttpTransport transport; std::atomic<int> calls{0};
    transport.SetHandler(L"https://provider.example", [&](const auto&, const auto&) { ++calls; return SearchResponse(); });
    auto cancellation = std::make_shared<OperationCancellation>(); cancellation->Cancel();
    OnlineSubtitleService service; const auto settings = SubtitleSettings();
    TEST_ASSERT_EQ(service.Search(L"Movie.mp4", L"", settings, &transport, cancellation).status, OnlineSubtitleStatus::Cancelled);
    OnlineSubtitleSearchResult item; item.downloadUrl = "https://provider.example/subtitle.srt";
    TEST_ASSERT_EQ(service.DownloadAndLoad(item, L"Movie.mp4", settings, &transport, temp.GetPath(), cancellation).status,
                   OnlineSubtitleStatus::Cancelled);
    TEST_ASSERT_EQ(calls.load(), 0);
}

TEST_CASE(AsyncNetworkJobs, SamePathReopenAndPrivacyChangeInvalidateContext) {
    Settings settings; settings.onlineSubtitlesEnabled = true;
    OnlineSubtitleJobs jobs;
    const std::wstring media = L"C:\\Media\\Movie.mp4";
    TEST_ASSERT(jobs.SyncContext(media, settings, 10, false));
    TEST_ASSERT(!jobs.SyncContext(media, settings, 10, false));
    TEST_ASSERT(jobs.SyncContext(media, settings, 11, false));
    TEST_ASSERT(jobs.SyncContext(media, settings, 11, true));
    TEST_ASSERT(jobs.SyncContext(media, settings, 11, false));
}

} // namespace Aperture::Test
