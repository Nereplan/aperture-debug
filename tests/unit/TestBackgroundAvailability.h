#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/BackgroundPathAvailability.h"
#include "../../src/MediaLibrary.h"
#include <future>

namespace Aperture::Test {
namespace BackgroundAvailabilityFixture {
template<class Predicate> bool PumpUntil(BackgroundPathAvailability& cache, Predicate predicate) {
    const auto until = GetTickCount64() + 5000;
    while (!predicate() && GetTickCount64() < until) { cache.Pump(); Sleep(5); }
    cache.Pump(); return predicate();
}
struct ReleaseOnExit {
    std::promise<void>& promise;
    void Release() { try { promise.set_value(); } catch (...) {} }
    ~ReleaseOnExit() { Release(); }
};
}

TEST_CASE(BackgroundAvailability, LookupsDeduplicateAndRefreshWithoutProbingInline) {
    using namespace BackgroundAvailabilityFixture;
    std::atomic<unsigned> probes{0};
    std::atomic<bool> present{false};
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> ranOnCaller{false};
    BackgroundPathAvailability cache([&](const auto&, const auto&) {
        ++probes; ranOnCaller.store(ranOnCaller.load() || std::this_thread::get_id() == caller);
        return PathAvailabilityObservation{present.load() ? PathAvailability::Present : PathAvailability::Missing, 0, true};
    });
    TEST_ASSERT(!cache.Lookup(L"C:\\Media\\movie.mkv").known);
    TEST_ASSERT(!cache.Lookup(L"c:/media/MOVIE.mkv").known);
    TEST_ASSERT_EQ(probes.load(), 0u);
    TEST_ASSERT_EQ(cache.CacheSize(), size_t(1));
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
    auto observed = cache.Lookup(L"C:\\Media\\movie.mkv");
    TEST_ASSERT(observed.known && observed.value == PathAvailability::Missing);
    TEST_ASSERT_EQ(probes.load(), 1u);
    present.store(true);
    const auto stale = cache.Lookup(L"C:\\Media\\movie.mkv", observed.checkedAtTick + BackgroundPathAvailability::kTtlMs);
    TEST_ASSERT(stale.known && stale.value == PathAvailability::Missing);
    TEST_ASSERT_EQ(probes.load(), 1u);
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
    TEST_ASSERT(cache.Lookup(L"C:\\Media\\movie.mkv").value == PathAvailability::Present);
    TEST_ASSERT_EQ(probes.load(), 2u);
    TEST_ASSERT(!ranOnCaller.load());
}

TEST_CASE(BackgroundAvailability, ABlockedCheckDoesNotBlockOtherLookupsOrTheOtherWorker) {
    using namespace BackgroundAvailabilityFixture;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
    BackgroundPathAvailability cache([&](const std::wstring& path, const auto&) {
        if (path.find(L"slow") != std::wstring::npos) { entered.set_value(); released.wait(); }
        return PathAvailabilityObservation{PathAvailability::Present, 0, true};
    });
    ReleaseOnExit releaseGuard{release};
    cache.Lookup(L"C:\\slow.mkv"); cache.Pump();
    TEST_ASSERT(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    TEST_ASSERT(!cache.Lookup(L"C:\\fast.mkv").known);
    TEST_ASSERT(PumpUntil(cache, [&] { return cache.Lookup(L"C:\\fast.mkv").known; }));
    TEST_ASSERT(!cache.Lookup(L"C:\\slow.mkv").known);
    releaseGuard.Release();
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
}

TEST_CASE(BackgroundAvailability, ResetRejectsStaleResultsForTheSamePath) {
    using namespace BackgroundAvailabilityFixture;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
    std::atomic<unsigned> probes{0};
    BackgroundPathAvailability cache([&](const auto&, const auto&) {
        if (++probes == 1) {
            entered.set_value(); released.wait(); // Deliberately ignore cancellation.
            return PathAvailabilityObservation{PathAvailability::Present, 0, true};
        }
        return PathAvailabilityObservation{PathAvailability::Missing, 0, true};
    });
    ReleaseOnExit releaseGuard{release};
    cache.Lookup(L"C:\\same.mkv"); cache.Pump();
    TEST_ASSERT(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    cache.Clear(); TEST_ASSERT(!cache.Lookup(L"C:\\same.mkv").known);
    cache.Pump(); releaseGuard.Release();
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
    const auto observation = cache.Lookup(L"C:\\same.mkv");
    TEST_ASSERT(observation.known && observation.value == PathAvailability::Missing);
    TEST_ASSERT_EQ(cache.CompletedProbeCount(), 1ull);
}

TEST_CASE(BackgroundAvailability, PendingDemandIsBoundedAndFailuresBackOff) {
    BackgroundPathAvailability cache([](const auto&, const auto&) -> PathAvailabilityObservation {
        throw std::runtime_error("injected failure");
    });
    for (size_t i = 0; i < BackgroundPathAvailability::kExtraPaths + 128; ++i)
        cache.Lookup(L"C:\\media\\item-" + std::to_wstring(i) + L".mkv");
    TEST_ASSERT_EQ(cache.CacheSize(), BackgroundPathAvailability::kExtraPaths);
    cache.Clear();
    TEST_ASSERT_EQ(cache.CacheSize(), size_t(0));
    TEST_ASSERT(!cache.Lookup(L"C:\\failure.mkv").known);
    TEST_ASSERT(BackgroundAvailabilityFixture::PumpUntil(cache, [&] { return !cache.HasPending(); }));
    TEST_ASSERT(!cache.Lookup(L"C:\\failure.mkv").known);
    TEST_ASSERT(!cache.HasPending());
}

TEST_CASE(BackgroundAvailability, SmartPlaylistsShareObservationsAndPreserveIndependentViews) {
    using namespace BackgroundAvailabilityFixture;
    MediaLibrary library;
    MediaItem item; item.id = "episode"; item.filePath = L"C:\\Media\\episode.mkv"; item.title = L"Episode";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    Playlist missing; missing.id = "missing"; missing.type = PlaylistType::Smart;
    missing.smartCriteria.rules = {{SmartRuleField::Status, SmartRuleOperator::Equals, "missing"}};
    Playlist title; title.id = "title"; title.type = PlaylistType::Smart;
    title.smartCriteria.rules = {{SmartRuleField::Title, SmartRuleOperator::Contains, "episode"}};
    library.playlists = {missing, title};
    BackgroundPathAvailability cache([](const auto&, const auto&) {
        return PathAvailabilityObservation{PathAvailability::Missing, 0, true};
    });
    library.SetAvailabilityProvider([&](const auto& path) { return cache.Lookup(path); });
    TEST_ASSERT(library.GetPlaylistPathsRef(library.playlists[0]).empty());
    const auto titleGeneration = library.ResolvePlaylistPaths(library.playlists[1]).generation;
    TEST_ASSERT_EQ(library.GetSmartPlaylistDiskProbeCount(), 0ull);
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
    library.InvalidateAvailabilityViews();
    TEST_ASSERT_EQ(library.GetPlaylistPathsRef(library.playlists[0]).size(), size_t(1));
    TEST_ASSERT_EQ(library.ResolvePlaylistPaths(library.playlists[1]).generation, titleGeneration);
    TEST_ASSERT_EQ(library.GetSmartPlaylistDiskProbeCount(), 0ull);
    TEST_ASSERT_EQ(cache.CompletedProbeCount(), 1ull);
    TEST_ASSERT_EQ(library.GetPlaylistStats("missing").totalCount, 1);
}

TEST_CASE(BackgroundAvailability, CopiedLibraryDoesNotCallTheOriginalUiProvider) {
    ScopedTempDir temp;
    MediaLibrary library;
    MediaItem item; item.id = "missing"; item.filePath = temp.SubPath(L"absent.mkv");
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    unsigned callbacks = 0;
    library.SetAvailabilityProvider([&](const auto&) { ++callbacks; return PathAvailabilityObservation{}; });
    SmartPlaylistCriteria criteria;
    criteria.rules = {{SmartRuleField::Status, SmartRuleOperator::Equals, "missing"}};
    TEST_ASSERT(library.EvaluateSmartPlaylist(criteria).empty());
    TEST_ASSERT_EQ(callbacks, 1u);
    MediaLibrary copy = library;
    TEST_ASSERT_EQ(copy.EvaluateSmartPlaylist(criteria).size(), size_t(1));
    TEST_ASSERT_EQ(callbacks, 1u);
}

TEST_CASE(BackgroundAvailability, SmartMembershipUsesTheObservationsOriginalExpiry) {
    MediaLibrary library;
    MediaItem item; item.id = "missing"; item.filePath = L"C:\\Media\\absent.mkv";
    library.mediaItems.push_back(item); library.RebuildPathIndex();
    Playlist playlist; playlist.id = "missing"; playlist.type = PlaylistType::Smart;
    playlist.smartCriteria.rules = {{SmartRuleField::Status, SmartRuleOperator::Equals, "missing"}};
    library.playlists.push_back(playlist);
    const auto checkedAt = GetTickCount64() - 1500;
    library.SetAvailabilityProvider([&](const auto&) {
        return PathAvailabilityObservation{PathAvailability::Missing, checkedAt, true};
    });
    const auto generation = library.ResolvePlaylistPaths(library.playlists.front()).generation;
    Sleep(600);
    TEST_ASSERT(library.ResolvePlaylistPaths(library.playlists.front()).generation != generation);
}

TEST_CASE(BackgroundAvailability, RealObservationsDistinguishDirectoriesAndMissingFiles) {
    using namespace BackgroundAvailabilityFixture;
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"video.mkv", "file"));
    BackgroundPathAvailability cache;
    cache.Lookup(temp.GetPath());
    cache.Lookup(temp.SubPath(L"video.mkv"));
    cache.Lookup(temp.SubPath(L"missing.mkv"));
    TEST_ASSERT(PumpUntil(cache, [&] { return !cache.HasPending(); }));
    TEST_ASSERT(cache.Lookup(temp.GetPath()).isDirectory);
    const auto file = cache.Lookup(temp.SubPath(L"video.mkv"));
    TEST_ASSERT(file.known && file.value == PathAvailability::Present && !file.isDirectory);
    TEST_ASSERT(cache.Lookup(temp.SubPath(L"missing.mkv")).value == PathAvailability::Missing);
}

} // namespace Aperture::Test
