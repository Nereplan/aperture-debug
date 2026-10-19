#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/FolderWatcher.h"
#include <future>

namespace Aperture::Test {
namespace FolderWatchConfigurationFixture {
template<class Predicate> bool Wait(Predicate predicate) {
    const auto until = GetTickCount64() + 5000;
    while (!predicate() && GetTickCount64() < until) Sleep(5);
    return predicate();
}
struct ReleaseOnExit {
    std::promise<void>& promise;
    void Release() { try { promise.set_value(); } catch (...) {} }
    ~ReleaseOnExit() { Release(); }
};
}
static void step_FolderWatchConfiguration_LatestListWinsWhileWorkerOwnsTheWatchMutex() {
    using namespace FolderWatchConfigurationFixture;
    ScopedTempDir temp;
    TEST_ASSERT(temp.CreateSubDir(L"first") && temp.CreateSubDir(L"second") && temp.CreateSubDir(L"third"));
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
    std::atomic<bool> gated{false};
    FolderWatcher watcher;
    ReleaseOnExit releaseGuard{release};
    watcher.SetMaxBatchSize(1);
    watcher.SetCallback([&](const WatchChangeBatch& batch) {
        // Notification-buffer dispatch owns the watch mutex. The initial rescan
        // is ignored so the gate exercises a real armed notification.
        if (!batch.items.empty() && !gated.exchange(true)) { entered.set_value(); released.wait(); }
    });
    const auto first = watcher.SetWatchedFoldersAsync({temp.SubPath(L"first")});
    TEST_ASSERT(first && Wait([&] { return watcher.GetAppliedConfigurationGeneration() == first; }));
    TEST_ASSERT(temp.WriteFile(L"first\\changed.mp4", "notification"));
    const bool reached = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!reached) releaseGuard.Release();
    TEST_ASSERT(reached);
    // Complete both requests before releasing the worker. A UI operation that
    // took watchMutex here would not complete until after release.
    auto requests = std::async(std::launch::async, [&] {
        watcher.SetWatchedFoldersAsync({temp.SubPath(L"second")});
        return watcher.SetWatchedFoldersAsync({temp.SubPath(L"third"), temp.SubPath(L"THIRD")});
    });
    const bool accepted = requests.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    releaseGuard.Release();
    const auto latest = requests.get();
    TEST_ASSERT(accepted && latest > first);
    TEST_ASSERT(Wait([&] { return watcher.GetAppliedConfigurationGeneration() == latest; }));
    const auto roots = watcher.GetWatchedFolders();
    TEST_ASSERT_EQ(roots.size(), size_t(1));
    TEST_ASSERT_EQ(NormalizePathKey(roots.front()), NormalizePathKey(temp.SubPath(L"third")));
    const auto empty = watcher.SetWatchedFoldersAsync({});
    TEST_ASSERT(Wait([&] { return watcher.GetAppliedConfigurationGeneration() == empty; }));
    TEST_ASSERT(watcher.GetWatchedFolders().empty());
    TEST_ASSERT(Wait([&] { return watcher.GetRetiredWatchCount() == 0; }));
    watcher.Stop();
    TEST_ASSERT(watcher.IsWorkerDone() && !watcher.IsRunning());
}
static void step_FolderWatchConfiguration_StopRequestDoesNotWaitForTheWorkerMutex() {
    using namespace FolderWatchConfigurationFixture;
    ScopedTempDir temp;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future(); auto released = release.get_future().share();
    std::atomic<bool> gated{false};
    FolderWatcher watcher;
    ReleaseOnExit releaseGuard{release};
    watcher.SetMaxBatchSize(1);
    watcher.SetCallback([&](const WatchChangeBatch& batch) {
        if (!batch.items.empty() && !gated.exchange(true)) { entered.set_value(); released.wait(); }
    });
    const auto generation = watcher.SetWatchedFoldersAsync({temp.GetPath()});
    TEST_ASSERT(generation && Wait([&] { return watcher.GetAppliedConfigurationGeneration() == generation; }));
    TEST_ASSERT(temp.WriteFile(L"changed.mp4", "notification"));
    const bool reached = enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!reached) releaseGuard.Release();
    TEST_ASSERT(reached);
    auto stop = std::async(std::launch::async, [&] { watcher.RequestStop(); });
    const bool returned = stop.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    releaseGuard.Release(); stop.get();
    TEST_ASSERT(returned && Wait([&] { return watcher.IsWorkerDone(); }));
    watcher.Stop();
    TEST_ASSERT_EQ(watcher.GetRetiredWatchCount(), size_t(0));
    TEST_ASSERT(watcher.Start()); watcher.Stop();
    TEST_ASSERT(watcher.IsWorkerDone());
}
static void step_FolderWatchConfiguration_EmptyProfileDoesNotStartAWorker() {
    FolderWatcher watcher;
    const auto generation = watcher.SetWatchedFoldersAsync({});
    TEST_ASSERT(generation != 0 && watcher.GetAppliedConfigurationGeneration() == generation);
    TEST_ASSERT(!watcher.IsRunning() && watcher.IsWorkerDone());
    watcher.Stop();
}
static void step_FolderWatchConfiguration_MissingRootsRegisterWhenTheyBecomeAvailable() {
    using namespace FolderWatchConfigurationFixture;
    ScopedTempDir temp;
    const auto path = temp.SubPath(L"late-folder");
    std::atomic<bool> rescanned{false};
    FolderWatcher watcher;
    watcher.SetCallback([&](const WatchChangeBatch& batch) {
        if (batch.overflowOrRescan && NormalizePathKey(batch.rootFolder) == NormalizePathKey(path)) rescanned.store(true);
    });
    const auto generation = watcher.SetWatchedFoldersAsync({path});
    TEST_ASSERT(generation && Wait([&] { return watcher.GetAppliedConfigurationGeneration() == generation; }));
    TEST_ASSERT(!rescanned.load());
    TEST_ASSERT(temp.CreateSubDir(L"late-folder"));
    TEST_ASSERT(Wait([&] { return rescanned.load(); }));
    watcher.Stop();
}


TEST_CASE(FolderWatchConfiguration, NonBlockingWorkerLifecycle) {
    step_FolderWatchConfiguration_LatestListWinsWhileWorkerOwnsTheWatchMutex();
    step_FolderWatchConfiguration_StopRequestDoesNotWaitForTheWorkerMutex();
    step_FolderWatchConfiguration_EmptyProfileDoesNotStartAWorker();
    step_FolderWatchConfiguration_MissingRootsRegisterWhenTheyBecomeAvailable();
}

} // namespace Aperture::Test
