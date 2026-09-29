#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/LibrarySnapshot.h"

#include <atomic>
#include <sstream>
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

TEST_CASE(LibrarySnapshot, AsyncBuildTransfersWithoutDeepCopy) {
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
    TEST_ASSERT_EQ(received->library.mediaItems.size(), (size_t)2);
    TEST_ASSERT_EQ(received->library.mediaItems.front().id, std::string("newest"));
    TEST_ASSERT(received->library.mediaItems.front().extraFields.has_key("futureField"));
    TEST_ASSERT(received->library.playlists.front().extraFields.has_key("futurePlaylistField"));
    TEST_ASSERT(received->library.history.front().extraFields.has_key("futureHistoryField"));
    TEST_ASSERT_EQ(received->library.watchedFolders.size(), (size_t)1);
    TEST_ASSERT_EQ(received->visibleIndices.size(), (size_t)2);

    MediaLibrary adopted = std::move(received->library);
    TEST_ASSERT_EQ(adopted.mediaItems.data(), workerBuffer);
    LibraryQueryEngine uiEngine;
    LibraryQueryOptions queryOptions;
    queryOptions.groupMode = LibraryGroupMode::Flat;
    TEST_ASSERT(uiEngine.PrimeCache(adopted, queryOptions, std::move(received->initialProjection)));
    TEST_ASSERT(uiEngine.IsCacheValid(adopted, queryOptions));
    TEST_ASSERT_EQ(uiEngine.Query(adopted, queryOptions).items.size(), (size_t)2);
}

TEST_CASE(LibrarySnapshot, RejectsCorruptPrimaryWithoutOverwritingIt) {
    ScopedTempDir stage(L"Aperture_Snapshot_Corrupt_");
    WriteSnapshotCompanionStores(stage);
    const std::string corrupt = "CORRUPT_LIBRARY{{{";
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", corrupt));

    auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 1));
    TEST_ASSERT(snapshot != nullptr);
    TEST_ASSERT(!snapshot->publishable);
    TEST_ASSERT(!snapshot->loadStatus.is_ok());

    std::string after;
    TEST_ASSERT(ReadTextFile(stage.SubPath(L"data\\library.json"), after).is_ok());
    TEST_ASSERT_EQ(after, corrupt);
}

TEST_CASE(LibrarySnapshot, RecoversFromNewestValidBackupReadOnly) {
    ScopedTempDir stage(L"Aperture_Snapshot_Recovery_");
    WriteSnapshotCompanionStores(stage);
    const std::wstring primary = stage.SubPath(L"data\\library.json");
    const std::string valid = "[{\"id\":\"recover-me\",\"filePath\":\"media/recovered.mp4\",\"title\":\"Recovered\",\"durationSec\":4,\"videoWidth\":1,\"videoHeight\":1}]";
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", valid));
    TEST_ASSERT(CreateRotatingBackup(primary, 3).is_ok());
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", "broken{{"));

    auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 2));
    TEST_ASSERT(snapshot != nullptr);
    TEST_ASSERT(snapshot->publishable);
    TEST_ASSERT(snapshot->library.GetLastLoadResult().libraryRecoveredFromBackup);
    TEST_ASSERT_EQ(snapshot->library.mediaItems.size(), (size_t)1);
    TEST_ASSERT_EQ(snapshot->library.mediaItems.front().id, std::string("recover-me"));

    std::string primaryAfter;
    TEST_ASSERT(ReadTextFile(primary, primaryAfter).is_ok());
    TEST_ASSERT_EQ(primaryAfter, std::string("broken{{"));
}

TEST_CASE(LibrarySnapshot, MissingStoresProduceValidEmptySnapshot) {
    ScopedTempDir stage(L"Aperture_Snapshot_Empty_");
    stage.CreateSubDir(L"data");
    auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 3));
    TEST_ASSERT(snapshot != nullptr);
    TEST_ASSERT(snapshot->publishable);
    TEST_ASSERT(snapshot->library.mediaItems.empty());
    TEST_ASSERT_EQ(snapshot->library.playlists.size(), (size_t)1);
    TEST_ASSERT_EQ(snapshot->library.playlists.front().id, std::string("pl_favorites"));
    TEST_ASSERT(snapshot->initialProjection.items.empty());
}

TEST_CASE(LibrarySnapshot, MailboxRejectsStaleAndCanceledGenerations) {
    LibrarySnapshotMailbox mailbox;
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
}

TEST_CASE(LibrarySnapshot, PreCanceledBuildDoesNotPublish) {
    ScopedTempDir stage(L"Aperture_Snapshot_Cancel_");
    WriteSnapshotCompanionStores(stage);
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", "[]"));
    std::atomic<bool> canceled{ true };
    auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 4, &canceled));
    TEST_ASSERT(snapshot != nullptr);
    TEST_ASSERT(snapshot->canceled);
    TEST_ASSERT(!snapshot->publishable);
}

TEST_CASE(LibrarySnapshot, ShutdownCancellationFencesInFlightProducer) {
    ScopedTempDir stage(L"Aperture_Snapshot_Shutdown_");
    WriteSnapshotCompanionStores(stage);
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", "[]"));

    LibrarySnapshotMailbox mailbox;
    const uint64_t generation = mailbox.BeginGeneration();
    std::atomic<bool> canceled{ false };
    std::atomic<bool> workerAtGate{ false };
    std::atomic<bool> releaseWorker{ false };
    std::atomic<bool> published{ false };

    auto options = SnapshotOptions(stage, generation, &canceled);
    options.milestone = [&](const wchar_t* phase) {
        if (wcscmp(phase, L"library-worker-start") != 0) return;
        workerAtGate.store(true, std::memory_order_release);
        while (!releaseWorker.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    std::thread producer([&]() {
        auto snapshot = BuildLibraryStartupSnapshot(options);
        if (snapshot && !snapshot->canceled) {
            published.store(mailbox.Publish(std::move(snapshot)), std::memory_order_release);
        }
    });
    while (!workerAtGate.load(std::memory_order_acquire)) std::this_thread::yield();
    canceled.store(true, std::memory_order_release);
    mailbox.Cancel();
    releaseWorker.store(true, std::memory_order_release);
    producer.join();

    TEST_ASSERT(!published.load(std::memory_order_acquire));
    TEST_ASSERT(!mailbox.HasPending());
    TEST_ASSERT(mailbox.Take(generation) == nullptr);
}

TEST_CASE(LibrarySnapshot, BuildsTwentyFiveThousandItemProjection) {
    ScopedTempDir stage(L"Aperture_Snapshot_Large_");
    WriteSnapshotCompanionStores(stage);
    std::ostringstream json;
    json << '[';
    constexpr size_t kCount = 25000;
    for (size_t i = 0; i < kCount; ++i) {
        if (i) json << ',';
        json << "{\"id\":\"item_" << i
             << "\",\"filePath\":\"media/item_" << i
             << ".mp4\",\"title\":\"Item " << i
             << "\",\"durationSec\":60,\"videoWidth\":1920,\"videoHeight\":1080,\"lastWatchedTimestamp\":"
             << i << '}';
    }
    json << ']';
    TEST_ASSERT(stage.WriteFile(L"data\\library.json", json.str()));

    auto snapshot = BuildLibraryStartupSnapshot(SnapshotOptions(stage, 5));
    TEST_ASSERT(snapshot != nullptr);
    TEST_ASSERT(snapshot->publishable);
    TEST_ASSERT_EQ(snapshot->library.mediaItems.size(), kCount);
    TEST_ASSERT_EQ(snapshot->initialProjection.items.size(), kCount);
    TEST_ASSERT_EQ(snapshot->visibleIndices.size(), kCount);
    TEST_ASSERT_EQ(snapshot->library.mediaItems.front().id, std::string("item_24999"));
}

} // namespace Aperture::Test
