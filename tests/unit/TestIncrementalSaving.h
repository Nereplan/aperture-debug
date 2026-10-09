#pragma once

#include "TestLibrarySaving.h"
#include "../../src/LibraryIncrementalStore.h"

namespace Aperture::Test {
namespace IncrementalSavingFixture {
struct Store {
    ScopedTempDir temp;
    MediaLibrary library;
    MediaLibrarySaveState state;
    std::shared_ptr<const MediaLibrarySaveSnapshot> snapshot;
    Store() {
        TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
        LibrarySavingFixture::AddRecords(library, temp.GetPath(), 300);
        library.mediaItems[7].extraFields = JsonValue::parse("{\"extension\":{\"preserve\":[1,2,3]}}");
        state.incrementalLibrary = true;
        TEST_ASSERT(Save().is_ok());
    }
    MediaLibrarySaveResult Save() {
        snapshot = library.CaptureSaveSnapshot(snapshot);
        return WriteLibrarySaveSnapshot(*snapshot, state);
    }
    std::wstring Checkpoint() const { return temp.SubPath(L"data\\library.json"); }
    std::wstring Changes() const { return temp.SubPath(L"data\\library.changes"); }
    std::string Read(const std::wstring& path) const {
        std::string text; TEST_ASSERT(ReadTextFile(path, text).is_ok()); return text;
    }
};
}

TEST_CASE(IncrementalSaving, ProgressPersistsWithoutRewritingTheCheckpoint) {
    using namespace LibraryIncrementalStore;
    IncrementalSavingFixture::Store store;
    const auto checkpoint = store.Read(store.Checkpoint());
    const auto bytes = store.state.writtenLibraryBytes;
    store.library.mediaItems[7].lastPosSec = 27;
    TEST_ASSERT(store.Save().librarySaved);
    TEST_ASSERT_EQ(store.state.incrementalWriteCount, 1ull);
    TEST_ASSERT_EQ(store.state.libraryCheckpointCount, 1ull);
    TEST_ASSERT_EQ(store.state.serializedMediaRecordCount, 301ull);
    TEST_ASSERT_EQ(store.Read(store.Checkpoint()), checkpoint);
    TEST_ASSERT(store.state.writtenLibraryBytes - bytes < checkpoint.size() / 4);
    JsonValue payload;
    TEST_ASSERT_EQ(Decode(store.Read(store.Changes()), payload), DWORD(ERROR_SUCCESS));
    TEST_ASSERT_EQ(payload.get("records").as_array().size(), size_t(1));
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[7].lastPosSec, 27.0);
    TEST_ASSERT_EQ(reloaded.mediaItems[7].filePath, store.library.mediaItems[7].filePath);
    TEST_ASSERT(reloaded.mediaItems[7].extraFields.get("extension") == store.library.mediaItems[7].extraFields.get("extension"));
}

TEST_CASE(IncrementalSaving, CumulativeChangesSupportRevertsAppendsAndTailRemoval) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[3].lastPosSec = 10; TEST_ASSERT(store.Save().is_ok());
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    store.library.mediaItems[3].lastPosSec = 0; TEST_ASSERT(store.Save().is_ok());
    MediaItem item = store.library.mediaItems[5];
    item.filePath = store.temp.SubPath(L"media\\appended.mkv");
    item.lastPosSec = 35; // A duplicate ID still addresses a distinct record slot.
    store.library.mediaItems.push_back(item); TEST_ASSERT(store.Save().is_ok());
    MediaLibrary appended; TEST_ASSERT(appended.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(appended.mediaItems.size(), size_t(301));
    TEST_ASSERT_EQ(appended.mediaItems[3].lastPosSec, 0.0);
    TEST_ASSERT_EQ(appended.mediaItems[7].lastPosSec, 20.0);
    TEST_ASSERT_EQ(appended.mediaItems.back().lastPosSec, 35.0);
    store.library.mediaItems.resize(298); TEST_ASSERT(store.Save().is_ok());
    MediaLibrary shrunk; TEST_ASSERT(shrunk.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(shrunk.mediaItems.size(), size_t(298));
    TEST_ASSERT_EQ(shrunk.mediaItems[7].lastPosSec, 20.0);
    TEST_ASSERT_EQ(store.state.libraryCheckpointCount, 1ull);
}

TEST_CASE(IncrementalSaving, CheckpointCompactsChangesAndNeverReplaysOldChanges) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    const auto oldChanges = store.Read(store.Changes()), oldCheckpoint = store.Read(store.Checkpoint());
    // Reverting record values still produces a distinct checkpoint identity.
    store.library.mediaItems[7].lastPosSec = 0;
    store.state.forceLibraryCheckpoint = true;
    TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT(!FileExists(store.Changes()));
    TEST_ASSERT(store.Read(store.Checkpoint()) != oldCheckpoint);
    TEST_ASSERT(AtomicWriteTextFile(store.Changes(), oldChanges).is_ok());
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[7].lastPosSec, 0.0);
    store.state.forceLibraryCheckpoint = false;
    std::reverse(store.library.mediaItems.begin(), store.library.mediaItems.end());
    TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT_EQ(store.state.libraryCheckpointCount, 3ull);
    TEST_ASSERT(!FileExists(store.Changes()));
    MediaLibrary reordered; TEST_ASSERT(reordered.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reordered.mediaItems.front().id, store.library.mediaItems.front().id);
}

TEST_CASE(IncrementalSaving, FailedDeltaAndCheckpointDoNotAcknowledgeNewState) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 10; TEST_ASSERT(store.Save().is_ok());
    const auto savedGeneration = store.state.libraryGeneration;
    const auto records = store.state.lastMediaRecords;
    TEST_ASSERT(SetFileAttributesW(store.Changes().c_str(), FILE_ATTRIBUTE_READONLY));
    store.library.mediaItems[7].lastPosSec = 25;
    const auto failed = store.Save();
    TEST_ASSERT(SetFileAttributesW(store.Changes().c_str(), FILE_ATTRIBUTE_NORMAL));
    TEST_ASSERT(!failed.libraryStatus.is_ok());
    TEST_ASSERT_EQ(store.state.libraryGeneration, savedGeneration);
    TEST_ASSERT(store.state.lastMediaRecords == records);
    TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT(SetFileAttributesW(store.Checkpoint().c_str(), FILE_ATTRIBUTE_READONLY));
    store.state.forceLibraryCheckpoint = true;
    const auto checkpointFailure = store.Save();
    TEST_ASSERT(SetFileAttributesW(store.Checkpoint().c_str(), FILE_ATTRIBUTE_NORMAL));
    TEST_ASSERT(!checkpointFailure.libraryStatus.is_ok());
    TEST_ASSERT(store.state.hasLibraryChanges);
    TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT(!store.state.hasLibraryChanges);
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[7].lastPosSec, 25.0);
}

TEST_CASE(IncrementalSaving, CorruptChangesRecoverOnlyAMatchingBackup) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 10; TEST_ASSERT(store.Save().is_ok());
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    auto corrupt = store.Read(store.Changes()); corrupt.back() = '?';
    TEST_ASSERT(AtomicWriteTextFile(store.Changes(), corrupt).is_ok());
    MediaLibrary recovered; TEST_ASSERT(recovered.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(recovered.mediaItems[7].lastPosSec, 10.0);
    TEST_ASSERT(recovered.GetLastLoadResult().libraryRecoveredFromBackup);
}

TEST_CASE(IncrementalSaving, CheckpointRecoveryFindsThePairedChangesBackup) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    store.state.forceLibraryCheckpoint = true; TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT(!FileExists(store.Changes()));
    TEST_ASSERT(AtomicWriteTextFile(store.Checkpoint(), "broken checkpoint").is_ok());
    MediaLibrary recovered; TEST_ASSERT(recovered.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(recovered.mediaItems[7].lastPosSec, 20.0);
    TEST_ASSERT(recovered.GetLastLoadResult().libraryRecoveredFromBackup);
}

TEST_CASE(IncrementalSaving, MissingChangesForceAFullCheckpointFromTheLatestSnapshot) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT(DeleteFileW(store.Changes().c_str()));
    TEST_ASSERT(store.Save().is_ok());
    TEST_ASSERT_EQ(store.state.libraryCheckpointCount, 2ull);
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[7].lastPosSec, 20.0);
}

TEST_CASE(IncrementalSaving, FutureAndOrphanedChangesAreProtectedFromSaving) {
    IncrementalSavingFixture::Store store;
    store.library.mediaItems[7].lastPosSec = 20; TEST_ASSERT(store.Save().is_ok());
    const auto valid = store.Read(store.Changes());
    const std::string future = "ApertureLibraryChanges:99\nfuture format";
    TEST_ASSERT(AtomicWriteTextFile(store.Changes(), future).is_ok());
    MediaLibrary protectedStore; TEST_ASSERT(!protectedStore.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT(!protectedStore.Save().libraryStatus.is_ok());
    TEST_ASSERT_EQ(store.Read(store.Changes()), future);
    TEST_ASSERT(AtomicWriteTextFile(store.Changes(), valid).is_ok());
    TEST_ASSERT(DeleteFileW(store.Checkpoint().c_str()));
    MediaLibrary orphaned; TEST_ASSERT(!orphaned.LoadFromRoot(store.temp.GetPath()).is_ok());
    TEST_ASSERT(!orphaned.Save().libraryStatus.is_ok());
    TEST_ASSERT(!FileExists(store.Checkpoint()));
}

TEST_CASE(IncrementalSaving, ValidationRejectsChecksumDamageDuplicateSlotsAndUnfilledAppends) {
    using namespace LibraryIncrementalStore;
    TEST_ASSERT_EQ(Digest("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const std::string base = "[{}]";
    JsonValue payload = JsonValue::parse("{\"baseBytes\":4,\"itemCount\":1,\"records\":[[0,{}],[0,{}]]}");
    payload["baseSha256"] = Digest(base);
    TEST_ASSERT(Validate(payload, Digest(base), base.size(), 1) == Match::Invalid);
    payload["records"] = JsonValue::parse("[[3,{}]]"); payload["itemCount"] = 4;
    TEST_ASSERT(Validate(payload, Digest(base), base.size(), 1) == Match::Invalid);
    payload["records"] = JsonValue::parse("[[1,{}],[2,{}],[3,{}]]");
    TEST_ASSERT(Validate(payload, Digest(base), base.size(), 1) == Match::Applicable);
    auto encoded = Encode(payload.stringify(false)); JsonValue decoded;
    TEST_ASSERT_EQ(Decode(encoded, decoded), DWORD(ERROR_SUCCESS));
    encoded.back() = '?';
    TEST_ASSERT_EQ(Decode(encoded, decoded), DWORD(ERROR_INVALID_DATA));
}

TEST_CASE(IncrementalSaving, IdleWorkerCheckpointBarrierPublishesTheLastAcceptedSnapshot) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath(), 300);
    LibrarySaveWorker worker(WriteLibrarySaveSnapshot, true);
    auto snapshot = library.CaptureSaveSnapshot();
    auto serial = worker.Submit(snapshot); worker.RequestFlush(); TEST_ASSERT(worker.WaitUntil(serial));
    library.mediaItems[7].lastPosSec = 25; snapshot = library.CaptureSaveSnapshot(snapshot);
    serial = worker.Submit(snapshot); worker.RequestFlush(); TEST_ASSERT(worker.WaitUntil(serial));
    TEST_ASSERT(worker.WasSuccessful(serial) && worker.IsIdle());
    TEST_ASSERT(FileExists(temp.SubPath(L"data\\library.changes")));
    const auto checkpointSerial = worker.RequestCheckpoint();
    TEST_ASSERT(checkpointSerial > serial && worker.WaitUntil(checkpointSerial) && worker.WasSuccessful(checkpointSerial));
    TEST_ASSERT(!FileExists(temp.SubPath(L"data\\library.changes")));
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[7].lastPosSec, 25.0);
}

} // namespace Aperture::Test
