#pragma once

#include "StressHarness.h"
#include "OnlineSubtitles.h"
#include "HttpTransport.h"
#include "Settings.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

namespace Aperture::Test::Stress {

TEST_CASE(Stress, OnlineSubtitlesConcurrentSearchAndDownload) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 12119 + 12;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.OnlineSubtitlesConcurrentSearchAndDownload", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_Subs_");
        std::wstring rootDir = tempDir.GetPath();

        MockHttpTransport mockTransport;
        std::string jsonResponse = R"({
            "data": [
                {
                    "id": "sub_1",
                    "attributes": {
                        "subtitle_id": "101",
                        "language": "en",
                        "release": "Test.Release.1080p",
                        "files": [
                            { "file_id": 1001, "file_name": "sub1.srt" }
                        ]
                    }
                }
            ]
        })";

        mockTransport.SetHandler(L"https://api.opensubtitles.com/api/v1/subtitles",
            [&jsonResponse](const std::wstring&, const HttpRequestOptions&) -> HttpResponse {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                HttpResponse resp;
                resp.success = true;
                resp.statusCode = 200;
                resp.body = jsonResponse;
                return resp;
            });

        std::string srtContent = "1\n00:00:01,000 --> 00:00:04,000\nHello World\n\n";
        mockTransport.SetHandler(L"https://api.opensubtitles.com/api/v1/download",
            [&srtContent](const std::wstring&, const HttpRequestOptions&) -> HttpResponse {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                HttpResponse resp;
                resp.success = true;
                resp.statusCode = 200;
                resp.body = srtContent;
                return resp;
            });

        Settings settings;
        settings.onlineSubtitlesEnabled = true;
        settings.audioLang = "eng";

        std::atomic<bool> stopWorkers{false};
        std::atomic<uint32_t> completedOps{0};

        constexpr size_t kThreads = 3;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([t, &settings, &mockTransport, &rootDir, &stopWorkers, &completedOps]() {
                OnlineSubtitleService service;
                while (!stopWorkers.load(std::memory_order_relaxed)) {
                    std::wstring mediaPath = L"C:\\media\\movie_" + std::to_wstring(t) + L".mp4";
                    OnlineSubtitleResult searchResult = service.Search(mediaPath, L"", settings, &mockTransport);
                    if (searchResult.success && !searchResult.searchResults.empty()) {
                        OnlineSubtitleSearchResult item = searchResult.searchResults.front();
                        item.downloadUrl = "https://api.opensubtitles.com/api/v1/download";
                        service.DownloadAndLoad(item, mediaPath, settings, &mockTransport, rootDir);
                    }
                    completedOps++;
                    std::this_thread::yield();
                }
            });
        }

        uint32_t delayMs = sched.NextDelayMs(5, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        stopWorkers.store(true, std::memory_order_release);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        TEST_ASSERT(completedOps.load() > 0);
    }
}

TEST_CASE(Stress, OnlineSubtitlesAtomicSaveStress) {
    const auto& config = GetGlobalStressConfig();
    const uint32_t iterations = config.iterations;

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        const uint64_t iterSeed = config.seed + iter * 13121 + 13;
        DeterministicSchedule sched(iterSeed);
        TestWatchdog watchdog(config.defaultTimeoutMs, "Stress.OnlineSubtitlesAtomicSaveStress", iterSeed, iter);

        ScopedTempDir tempDir(L"Aperture_Stress_SubSave_");
        std::wstring rootDir = tempDir.GetPath();

        std::string sampleSrt = "1\n00:00:01,000 --> 00:00:03,000\nStress Subtitle\n\n";
        std::atomic<bool> stopSaves{false};
        std::atomic<uint32_t> savesCompleted{0};

        constexpr size_t kThreads = 4;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([t, &sampleSrt, &rootDir, &stopSaves, &savesCompleted]() {
                uint32_t saveIdx = 0;
                while (!stopSaves.load(std::memory_order_relaxed)) {
                    std::wstring title = L"Title_" + std::to_wstring(t) + L"_" + std::to_wstring(saveIdx++);
                    std::wstring savedPath;
                    StorageStatus status = OnlineSubtitleService::SaveSubtitleAtomically(
                        sampleSrt, title, L"en", "hash_" + std::to_string(t), "srt", savedPath, rootDir);
                    if (status.is_ok()) {
                        savesCompleted++;
                    }
                    std::this_thread::yield();
                }
            });
        }

        uint32_t delayMs = sched.NextDelayMs(5, 25);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        stopSaves.store(true, std::memory_order_release);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        TEST_ASSERT(savesCompleted.load() > 0);
    }
}

} // namespace Aperture::Test::Stress
