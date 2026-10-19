#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/LibrarySnapshot.h"

#include <atomic>
#include <thread>

namespace Aperture::Test {
namespace {

void WriteSnapshotCompanionStores(const ScopedTempDir& stage) {
    stage.CreateSubDir(L"data");
    stage.WriteFile(L"data\\playlists.json",
        "[{\"id\":\"playlist-one\",\"name\":\"One\",\"itemPaths\":[],\"futurePlaylistField\":42}]");
    stage.WriteFile(L"data\\history.json",
        "[{\"filePath\":\"media/history.mp4\",\"title\":\"History\",\"watchedTimestamp\":1,\"futureHistoryField\":true}]");
    stage.WriteFile(L"data\\watched_folders.json", "[\"media\"]");
}

LibrarySnapshotBuildOptions SnapshotOptions(const ScopedTempDir& stage,
                                            uint64_t generation,
                                            const std::atomic<bool>* cancel = nullptr) {
    LibrarySnapshotBuildOptions options;
    options.generation = generation;
    options.groupMode = LibraryGroupMode::Flat;
    options.rootDir = stage.GetPath();
    options.cancelRequested = cancel;
    return options;
}

} // namespace

TEST_CASE(LibrarySnapshot, AsyncMailboxAndBackupRecovery) {
    ScopedTempDir stage(L"Aperture_Snapshot_Move_");
    WriteSnapshotCompanionStores(stage);
    TEST_ASSERT(stage.WriteFile(L"data\\library.json",
        "[{\"id\":\"older\",\"filePath\":\"media/older.mp4\",\"title\":\"Older\",\"durationSec\":10,\"videoWidth\":1920,\"videoHeight\":1080,\"lastWatchedTimestamp\":10},"
        "{\"id\":\"newest\",\"filePath\":\"media/newest.mp4\",\"title\":\"Newest\",\"durationSec\":20,\"videoWidth\":1920,\"videoHeight\":1080,\"lastWatchedTimestamp\":20,\"futureField\":{\"kept\":true}}]"));

    LibrarySnapshotMailbox mailbox;
    const uint64_t generation = mailbox.BeginGeneration();
    const MediaItem* workerBuffer = nullptr;
    std::thread producer([&]() {
        auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, generation));
        TEST_ASSERT(snapshot != nullptr);
        TEST_ASSERT(snapshot->publishable);
        workerBuffer = snapshot->library.mediaItems.data();
        TEST_ASSERT(mailbox.Publish(std::move(snapshot)));
    });
    producer.join();

    auto received = mailbox.Take(generation);
    TEST_ASSERT(received != nullptr);
    TEST_ASSERT_EQ(received->library.mediaItems.data(), workerBuffer);
    TEST_ASSERT_EQ(received->library.mediaItems.size(), size_t(2));
    TEST_ASSERT_EQ(received->library.mediaItems.front().id, std::string("newest"));
    TEST_ASSERT(received->library.mediaItems.front().extraFields.has_key("futureField"));
    TEST_ASSERT(received->library.playlists.front().extraFields.has_key("futurePlaylistField"));
    TEST_ASSERT(received->library.history.front().extraFields.has_key("futureHistoryField"));
    TEST_ASSERT_EQ(received->library.watchedFolders.size(), size_t(1));
    TEST_ASSERT_EQ(received->initialProjection.indices.size(), size_t(2));

    MediaLibrary adopted = std::move(received->library);
    TEST_ASSERT_EQ(adopted.mediaItems.data(), workerBuffer);
    LibraryQueryEngine uiEngine;
    LibraryQueryOptions queryOptions;
    queryOptions.groupMode = LibraryGroupMode::Flat;
    TEST_ASSERT(uiEngine.PrimeViewCache(adopted, queryOptions, std::move(received->initialProjection)));
    TEST_ASSERT_EQ(uiEngine.QueryView(adopted, queryOptions).indices.size(), size_t(2));
    TEST_ASSERT_EQ(uiEngine.GetViewBuildCount(), 0ull);
    queryOptions.pinLastWatched = true;
    TEST_ASSERT_EQ(uiEngine.QueryView(adopted, queryOptions).indices.front(), size_t(0));
    TEST_ASSERT_EQ(uiEngine.GetPlanBuildCount(), 0ull);

    ScopedTempDir corruptStage(L"Aperture_Snapshot_Corrupt_");
    WriteSnapshotCompanionStores(corruptStage);
    const std::string corrupt = "CORRUPT_LIBRARY{{{";
    TEST_ASSERT(corruptStage.WriteFile(L"data\\library.json", corrupt));
    auto corruptSnap = BuildLibraryStartupSnapshot(SnapshotOptions(corruptStage, 1));
    TEST_ASSERT(corruptSnap != nullptr);
    TEST_ASSERT(!corruptSnap->publishable);
    TEST_ASSERT(!corruptSnap->loadStatus.is_ok());
    std::string after;
    TEST_ASSERT(ReadTextFile(corruptStage.SubPath(L"data\\library.json"), after).is_ok());
    TEST_ASSERT_EQ(after, corrupt);

    ScopedTempDir recoveryStage(L"Aperture_Snapshot_Recovery_");
    WriteSnapshotCompanionStores(recoveryStage);
    const std::wstring primary = recoveryStage.SubPath(L"data\\library.json");
    const std::string valid = "[{\"id\":\"recover-me\",\"filePath\":\"media/recovered.mp4\",\"title\":\"Recovered\",\"durationSec\":4,\"videoWidth\":1,\"videoHeight\":1}]";
    TEST_ASSERT(recoveryStage.WriteFile(L"data\\library.json", valid));
    TEST_ASSERT(CreateRotatingBackup(primary, 3).is_ok());
    TEST_ASSERT(recoveryStage.WriteFile(L"data\\library.json", "broken{{"));

    auto recSnap = BuildLibraryStartupSnapshot(SnapshotOptions(recoveryStage, 2));
    TEST_ASSERT(recSnap != nullptr);
    TEST_ASSERT(recSnap->publishable);
    TEST_ASSERT(recSnap->library.GetLastLoadResult().libraryRecoveredFromBackup);
    TEST_ASSERT_EQ(recSnap->library.mediaItems.size(), size_t(1));
    TEST_ASSERT_EQ(recSnap->library.mediaItems.front().id, std::string("recover-me"));
    std::string primaryAfter;
    TEST_ASSERT(ReadTextFile(primary, primaryAfter).is_ok());
    TEST_ASSERT_EQ(primaryAfter, std::string("broken{{"));

    const uint64_t first = mailbox.BeginGeneration();
    const uint64_t second = mailbox.BeginGeneration();
    TEST_ASSERT(first != second);
    auto stale = std::make_unique<LibraryStartupSnapshot>();
    stale->generation = first;
    TEST_ASSERT(!mailbox.Publish(std::move(stale)));

    auto current = std::make_unique<LibraryStartupSnapshot>();
    current->generation = second;
    TEST_ASSERT(mailbox.Publish(std::move(current)));
    TEST_ASSERT(mailbox.HasPending());
    mailbox.Cancel();
    TEST_ASSERT(!mailbox.HasPending());
    TEST_ASSERT(mailbox.Take(second) == nullptr);

    std::atomic<bool> canceled{ true };
    auto canceledSnap = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 4, &canceled));
    TEST_ASSERT(canceledSnap != nullptr);
    TEST_ASSERT(canceledSnap->canceled);
    TEST_ASSERT(!canceledSnap->publishable);

    const uint64_t fenceGen = mailbox.BeginGeneration();
    std::atomic<bool> fenceCancel{ false };
    std::atomic<bool> workerAtGate{ false };
    std::atomic<bool> releaseWorker{ false };
    std::atomic<bool> published{ false };

    auto fenceOpts = SnapshotOptions(stage, fenceGen, &fenceCancel);
    fenceOpts.milestone = [&](const wchar_t* phase) {
        if (wcscmp(phase, L"library-worker-start") != 0) return;
        workerAtGate.store(true, std::memory_order_release);
        while (!releaseWorker.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    std::thread fencedProducer([&]() {
        auto snapshot = BuildLibraryStartupSnapshot(fenceOpts);
        if (snapshot && !snapshot->canceled) {
            published.store(mailbox.Publish(std::move(snapshot)), std::memory_order_release);
        }
    });
    while (!workerAtGate.load(std::memory_order_acquire)) std::this_thread::yield();
    fenceCancel.store(true, std::memory_order_release);
    mailbox.Cancel();
    releaseWorker.store(true, std::memory_order_release);
    fencedProducer.join();

    TEST_ASSERT(!published.load(std::memory_order_acquire));
    TEST_ASSERT(!mailbox.HasPending());
    TEST_ASSERT(mailbox.Take(fenceGen) == nullptr);
}

} // namespace Aperture::Test
