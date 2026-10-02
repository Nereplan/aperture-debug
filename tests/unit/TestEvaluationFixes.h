#pragma once
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/BackupFiles.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Settings.h"
#include "../../src/PlaybackSession.h"
#include "../../src/MediaFoundation.h"
#include "../../src/MpvPlayer.h"
#include "../../src/FolderWatcher.h"
#include <thread>
#include <condition_variable>

namespace Aperture::Test {
namespace {
// The unit executable itself lives in an isolated temporary directory. Keep
// tests independent of stores/backups produced by earlier component suites.
struct EvaluationStoreGuard {
    struct File { std::wstring path; std::string text; bool existed; };
    std::vector<File> files;
    explicit EvaluationStoreGuard(const wchar_t* name) {
        std::vector<std::wstring> paths{GetDataDir() + L"\\" + name};
        for (int i = 1; i <= 5; ++i) paths.push_back(GetDataDir() + L"\\backups\\" + name + L".bak." + std::to_wstring(i));
        for (const auto& path : paths) {
            std::string text;
            const bool existed = FileExists(path) && ReadTextFile(path, text).is_ok();
            files.push_back({path, std::move(text), existed});
            DeleteFileW(path.c_str());
        }
    }
    ~EvaluationStoreGuard() {
        for (const auto& file : files) {
            if (file.existed) AtomicWriteTextFile(file.path, file.text);
            else DeleteFileW(file.path.c_str());
        }
    }
};
}

TEST_CASE(EvaluationFixes, OfflineRescanPreservesMetadataAndMembership) {
    ScopedTempDir stage;
    MediaLibrary library;
    MediaItem item; item.id = "retained"; item.filePath = stage.SubPath(L"offline\\movie.mkv"); item.lastPosSec = 42;
    item.preferences.emplace(); item.preferences->playbackSpeed = 1.5;
    library.mediaItems.push_back(item);
    Playlist playlist; playlist.id = "list"; playlist.itemPaths.push_back(item.filePath);
    library.playlists.push_back(playlist); library.RebuildPathIndex();
    const auto result = library.ReconcileRescan(stage.SubPath(L"offline"), false);
    TEST_ASSERT(!result.status.is_ok());
    TEST_ASSERT_EQ(result.removedCount, 0ull);
    TEST_ASSERT_EQ(library.mediaItems.size(), 1ull);
    TEST_ASSERT_EQ(library.playlists.front().itemPaths.size(), 1ull);
    TEST_ASSERT_EQ(library.FindByPath(item.filePath)->lastPosSec, 42.0);
    TEST_ASSERT_EQ(*library.FindByPath(item.filePath)->preferences->playbackSpeed, 1.5);
}

TEST_CASE(EvaluationFixes, CompleteRescanHonorsDeferredSave) {
    EvaluationStoreGuard guard(L"library.json");
    const auto path = GetDataDir() + L"\\library.json";
    TEST_ASSERT(AtomicWriteTextFile(path, "keep this store").is_ok());
    ScopedTempDir stage; stage.CreateSubDir(L"available");
    MediaLibrary library;
    MediaItem item; item.id = "removed"; item.filePath = stage.SubPath(L"available\\gone.mkv");
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    const auto result = library.ReconcileRescan(stage.SubPath(L"available"), false);
    TEST_ASSERT(result.status.is_ok());
    TEST_ASSERT_EQ(result.removedCount, 1ull);
    std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok());
    TEST_ASSERT_EQ(text, std::string("keep this store"));
}

TEST_CASE(EvaluationFixes, InvalidStoreShapeAndFutureSchemaAreProtected) {
    ScopedTempDir stage; stage.CreateSubDir(L"data");
    for (const std::string text : {std::string("{\"schemaVersion\":2,\"items\":{}}"),
                                  std::string("{\"schemaVersion\":999,\"items\":[]}"), std::string("{}")}) {
        TEST_ASSERT(stage.WriteFile(L"data\\library.json", text));
        MediaLibrary library;
        TEST_ASSERT(!library.LoadFromRoot(stage.GetPath()).is_ok());
        TEST_ASSERT(!library.Save().is_ok());
        std::string after; TEST_ASSERT(ReadTextFile(stage.SubPath(L"data\\library.json"), after).is_ok());
        TEST_ASSERT_EQ(after, text);
    }
}

TEST_CASE(EvaluationFixes, StructurallyInvalidLibraryRecoversFromValidBackup) {
    ScopedTempDir stage; stage.CreateSubDir(L"data\\backups");
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", "{\"schemaVersion\":2,\"items\":{}}"));
    TEST_ASSERT(stage.WriteFile(L"data\\backups\\library.json.bak.1", "{}"));
    TEST_ASSERT(stage.WriteFile(L"data\\backups\\library.json.bak.2", "[{\"id\":\"kept\",\"filePath\":\"movie.mkv\"}]"));
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(stage.GetPath()).is_ok());
    TEST_ASSERT(library.GetLastLoadResult().libraryRecoveredFromBackup);
    TEST_ASSERT_EQ(library.mediaItems.front().id, std::string("kept"));
}

TEST_CASE(EvaluationFixes, MalformedNumericRulesFailWithoutThrowing) {
    MediaItem item;
    for (const auto field : {SmartRuleField::Duration, SmartRuleField::Progress, SmartRuleField::FileSize,
                            SmartRuleField::DateAdded, SmartRuleField::LastWatched, SmartRuleField::VideoHeight}) {
        for (const char* value : {"", "badm", "bad days", "nan", "inf", "-1", "1e999", "18446744073709551616"}) {
            SmartPlaylistRule rule; rule.field = field; rule.op = SmartRuleOperator::GreaterThan; rule.value = value;
            TEST_ASSERT(!rule.Validate());
            TEST_ASSERT(!rule.Matches(item));
        }
    }
    TEST_ASSERT(!SmartPlaylistCriteria::FromRuleString("limit:-1").Validate());
    TEST_ASSERT(!SmartPlaylistCriteria::FromRuleString("sort:bad").Validate());
    auto valid = SmartPlaylistCriteria::FromRuleString("duration > 10m; progress < 90%; resolution >= 720p; limit:50");
    TEST_ASSERT(valid.Validate());
    item.durationSec = 1000; item.lastPosSec = 100; item.videoHeight = 1080;
    TEST_ASSERT(valid.Matches(item));
}

TEST_CASE(EvaluationFixes, MissingSmartRuleChecksActualAvailability) {
    ScopedTempDir stage;
    MediaLibrary library; MediaItem item; item.id = "missing"; item.filePath = stage.SubPath(L"gone.mkv");
    library.mediaItems.push_back(item);
    const auto criteria = SmartPlaylistCriteria::FromRuleString("status = missing");
    TEST_ASSERT_EQ(library.EvaluateSmartPlaylist(criteria).size(), 1ull);
    TEST_ASSERT(stage.WriteFile(L"gone.mkv", "present"));
    TEST_ASSERT(library.EvaluateSmartPlaylist(criteria).empty());
}

TEST_CASE(EvaluationFixes, UnresolvedDurationHasBoundedBackoff) {
    ScopedTempDir stage; TEST_ASSERT(stage.WriteFile(L"unknown.mkv", "fixture"));
    MediaLibrary library; MediaItem item; item.id = "unknown"; item.filePath = stage.SubPath(L"unknown.mkv");
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    MetadataBackfillOptions options; options.nowTimestamp = 1000;
    options.probeFunction = [](const std::wstring&, double& d, uint32_t& w, uint32_t& h) { d = 0; w = 1920; h = 1080; return true; };
    const auto first = library.BackfillMissingMetadata(options, {}, false);
    const auto second = library.BackfillMissingMetadata(options, {}, false);
    TEST_ASSERT_EQ(first.failedCount, 1ull);
    TEST_ASSERT_EQ(second.processedCount, 0ull);
    TEST_ASSERT_EQ(library.mediaItems.front().probeAttemptCount, 1u);
    TEST_ASSERT_EQ(library.mediaItems.front().videoHeight, 1080u);
    TEST_ASSERT(!MediaLibrary::CanAttemptMetadataBackfill(library.mediaItems.front(), options, 1029));
    TEST_ASSERT(MediaLibrary::CanAttemptMetadataBackfill(library.mediaItems.front(), options, 1030));
}

TEST_CASE(EvaluationFixes, FailedSettingsLoadPreservesOriginalUntilExplicitReset) {
    EvaluationStoreGuard guard(L"settings.json");
    const auto path = GetDataDir() + L"\\settings.json";
    TEST_ASSERT(AtomicWriteTextFile(path, "{broken").is_ok());
    Settings settings; TEST_ASSERT(!settings.Load().is_ok());
    TEST_ASSERT(!settings.Save().is_ok());
    std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok()); TEST_ASSERT_EQ(text, std::string("{broken"));
    settings.ResetToDefaults(); TEST_ASSERT(settings.Save().is_ok());
}

TEST_CASE(EvaluationFixes, VolumeAndNegativePlacementRoundTrip) {
    EvaluationStoreGuard guard(L"settings.json");
    Settings settings; settings.volume = 0.25; settings.windowX = -1400; settings.windowY = -600; settings.hasWindowPlacement = true;
    TEST_ASSERT(settings.Save().is_ok());
    Settings loaded; TEST_ASSERT(loaded.Load().is_ok());
    TEST_ASSERT_EQ(loaded.volume, 0.25);
    TEST_ASSERT_EQ(loaded.windowX, -1400);
    TEST_ASSERT_EQ(loaded.windowY, -600);
    TEST_ASSERT(loaded.hasWindowPlacement);
}

TEST_CASE(EvaluationFixes, FailedSessionLoadIsWriteProtected) {
    ScopedTempDir stage; const auto path = stage.SubPath(L"session.json");
    TEST_ASSERT(stage.WriteFile(L"session.json", "{broken"));
    PlaybackSession session; TEST_ASSERT(!session.Load(path, stage.GetPath()).is_ok());
    session.OpenDirect(stage.SubPath(L"movie.mkv"));
    TEST_ASSERT(!session.Save(path, stage.GetPath()).is_ok());
    std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok()); TEST_ASSERT_EQ(text, std::string("{broken"));
    session.Reset(); TEST_ASSERT(session.Save(path, stage.GetPath()).is_ok());
}

TEST_CASE(EvaluationFixes, BackupRetainsNewestEvenAboveBudget) {
    ScopedTempDir stage;
    TEST_ASSERT(stage.WriteFile(L"large.json", std::string(33 * 1024 * 1024, 'x')));
    const auto status = CreateRotatingBackup(stage.SubPath(L"large.json"));
    TEST_ASSERT(status.is_ok());
    TEST_ASSERT(FileExists(status.path));
}

TEST_CASE(EvaluationFixes, PrivateAndStreamPersistencePolicy) {
    PlaybackSession session;
    const auto url = L"https://example.com/movie.mkv?token=do-not-persist";
    TEST_ASSERT(!session.CanPersistLocalTarget(url));
    TEST_ASSERT(!session.CanPersistRecentStreams(false));
    TEST_ASSERT(session.CanPersistRecentStreams(true));
    session.SetPrivateMode(true);
    TEST_ASSERT(!session.CanPersistLocalTarget(L"C:\\movie.mkv"));
    TEST_ASSERT(!session.CanPersistRecentStreams(true));
    ScopedTempDir stage; const auto path = stage.SubPath(L"session.json");
    TEST_ASSERT(session.OpenDirect(url));
    TEST_ASSERT(session.Save(path, stage.GetPath()).is_ok());
    TEST_ASSERT(!FileExists(path));
    session.SetPrivateMode(false);
    TEST_ASSERT(session.OpenQueue({url, stage.SubPath(L"local.mkv")}, 0));
    TEST_ASSERT(session.Save(path, stage.GetPath()).is_ok());
    std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok());
    TEST_ASSERT(text.find("do-not-persist") == std::string::npos);
    JsonValue root; TEST_ASSERT(JsonValue::parse(text, root));
    TEST_ASSERT(root.get("currentFilePath").as_string().empty());
    TEST_ASSERT_EQ(root.get("queue").as_array().size(), 1ull);
}

TEST_CASE(EvaluationFixes, RecoverableSubtitleFailureDoesNotReportFatalPlayback) {
    MpvPlayer player;
    int fatalErrors = 0, warnings = 0;
    player.SetOnError([&](const std::wstring&) { ++fatalErrors; });
    player.SetOnWarning([&](const std::wstring&) { ++warnings; });
    TEST_ASSERT(!player.LoadExternalSubtitle(L""));
    TEST_ASSERT_EQ(fatalErrors, 0);
    TEST_ASSERT_EQ(warnings, 1);
    player.ClearCallbacks();
    TEST_ASSERT(!player.LoadExternalSubtitle(L""));
    TEST_ASSERT_EQ(warnings, 1);
}

TEST_CASE(EvaluationFixes, ConcurrentMediaFoundationTransitionsArePaired) {
    const auto before = MediaFoundation::GetLifecycleStats();
    TEST_ASSERT_EQ(before.references, 0);
    std::mutex mutex; std::condition_variable ready;
    int arrived = 0, acquired = 0;
    bool release = false;
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) workers.emplace_back([&] {
        const bool held = MediaFoundation::Acquire();
        { std::unique_lock<std::mutex> lock(mutex);
          ++arrived; if (held) ++acquired; ready.notify_all();
          ready.wait(lock, [&] { return release; }); }
        if (held) MediaFoundation::Release();
    });
    { std::unique_lock<std::mutex> lock(mutex);
      ready.wait(lock, [&] { return arrived == 16; });
      release = true; ready.notify_all(); }
    for (auto& worker : workers) worker.join();
    const auto after = MediaFoundation::GetLifecycleStats();
    TEST_ASSERT_EQ(after.references, 0);
    TEST_ASSERT_EQ(after.startups - before.startups, acquired ? 1ull : 0ull);
    TEST_ASSERT_EQ(after.shutdowns - before.shutdowns, acquired ? 1ull : 0ull);
    TEST_ASSERT(!MediaFoundation::IsInitialized());
}

TEST_CASE(EvaluationFixes, RemovedFolderWatchesReclaimAfterTheirCompletion) {
    ScopedTempDir stage;
    FolderWatcher watcher; TEST_ASSERT(watcher.Start());
    for (int i = 0; i < 128; ++i) {
        TEST_ASSERT(watcher.AddWatch(stage.GetPath()));
        TEST_ASSERT(watcher.RemoveWatch(stage.GetPath()));
    }
    const auto deadline = GetTickCount64() + 3000;
    while (watcher.GetRetiredWatchCount() && GetTickCount64() < deadline) Sleep(5);
    TEST_ASSERT_EQ(watcher.GetRetiredWatchCount(), 0ull);
    TEST_ASSERT(watcher.AddWatch(stage.GetPath()));
    watcher.Stop();
    TEST_ASSERT_EQ(watcher.GetRetiredWatchCount(), 0ull);
    TEST_ASSERT(watcher.Start());
    watcher.ClearWatches(); watcher.Stop();
    TEST_ASSERT_EQ(watcher.GetRetiredWatchCount(), 0ull);
}

TEST_CASE(EvaluationFixes, PreviewCacheInvalidatesAcrossD2DDevices) {
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    TEST_ASSERT(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)));
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi; TEST_ASSERT(SUCCEEDED(d3d.As(&dxgi)));
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory;
    TEST_ASSERT(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))));
    Microsoft::WRL::ComPtr<ID2D1Device> firstDevice, secondDevice;
    TEST_ASSERT(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &firstDevice)));
    TEST_ASSERT(SUCCEEDED(factory->CreateDevice(dxgi.Get(), &secondDevice)));
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> first, second;
    TEST_ASSERT(SUCCEEDED(firstDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &first)));
    TEST_ASSERT(SUCCEEDED(secondDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &second)));
    MpvPreviewPlayer preview;
    PreviewThumbnailFrame frame; frame.filePath = L"C:\\fixture.mkv"; frame.timestampSec = 0;
    frame.width = 2; frame.height = 2; frame.bgraPixels.assign(16, 127);
    preview.GetMemoryCache().Put(frame);
    TEST_ASSERT(preview.RequestSeekbarThumbnail(frame.filePath, 0, 60, 1, first.Get()));
    TEST_ASSERT(preview.HasSeekbarThumbnail());
    preview.BindDeviceContext(second.Get());
    TEST_ASSERT(!preview.HasSeekbarThumbnail());
    TEST_ASSERT(preview.RequestSeekbarThumbnail(frame.filePath, 0, 60, 1, second.Get()));
    TEST_ASSERT(preview.HasSeekbarThumbnail());
}

TEST_CASE(EvaluationFixes, UnreadableOutputIsRejectedBeforeReplacement) {
    ScopedTempDir stage; const auto path = stage.SubPath(L"store.json");
    TEST_ASSERT(stage.WriteFile(L"store.json", "original"));
    TEST_ASSERT(!AtomicWriteTextFile(path, std::string(kMaxPortableStoreBytes + 1, 'x')).is_ok());
    std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok()); TEST_ASSERT_EQ(text, std::string("original"));
}
} // namespace Aperture::Test
