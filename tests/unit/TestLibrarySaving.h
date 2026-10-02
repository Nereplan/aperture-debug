#pragma once

#include "../support/TestFramework.h"
#include "../../src/MediaLibrary.h"
#include "../../src/LibrarySaveWorker.h"
#include <future>

namespace Aperture::Test {
namespace LibrarySavingFixture {
inline void AddRecords(MediaLibrary& library, const std::wstring& root, size_t count = 3) {
    for (size_t i = 0; i < count; ++i) {
        MediaItem item;
        item.id = "save-" + std::to_string(i);
        item.filePath = root + L"\\media\\movie-" + std::to_wstring(i) + L".mkv";
        item.title = L"Movie " + std::to_wstring(i); item.durationSec = 120;
        library.mediaItems.push_back(std::move(item));
    }
}
}

TEST_CASE(LibrarySaving, BackgroundWriterCoalescesPendingAndFlushesLatest) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath());
    auto first = library.CaptureSaveSnapshot();
    library.mediaItems[0].lastPosSec = 10;
    auto second = library.CaptureSaveSnapshot(first);
    library.mediaItems[0].lastPosSec = 20;
    auto latest = library.CaptureSaveSnapshot(second);
    std::promise<void> started, release;
    auto released = release.get_future().share();
    std::vector<double> positions;
    LibrarySaveWorker worker([&](const MediaLibrarySaveSnapshot& snapshot, MediaLibrarySaveState& state) {
        positions.push_back((*snapshot.mediaItems)[0]->lastPosSec);
        if (positions.size() == 1) { started.set_value(); released.wait(); }
        return WriteLibrarySaveSnapshot(snapshot, state);
    });
    worker.Submit(first); worker.RequestFlush();
    const bool didStart = started.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!didStart) release.set_value();
    TEST_ASSERT(didStart);
    const auto replacedSerial = worker.Submit(second);
    const auto finalSerial = worker.Submit(latest);
    TEST_ASSERT(finalSerial > replacedSerial);
    TEST_ASSERT_EQ(worker.Submit(latest), finalSerial);
    TEST_ASSERT(!worker.IsComplete(finalSerial));
    worker.RequestFlush(); release.set_value(); worker.StopAndFlush();
    TEST_ASSERT(worker.IsComplete(finalSerial)); TEST_ASSERT(worker.IsIdle());
    TEST_ASSERT_EQ(positions.size(), size_t(2)); TEST_ASSERT_EQ(positions.back(), 20.0);
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[0].lastPosSec, 20.0);
}

TEST_CASE(LibrarySaving, FailedBackgroundGenerationCanRetryAndReportsFailure) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    const auto snapshot = library.CaptureSaveSnapshot();
    unsigned calls = 0;
    LibrarySaveWorker worker([&](const MediaLibrarySaveSnapshot& value, MediaLibrarySaveState& state) {
        if (++calls == 1) throw std::runtime_error("injected writer failure");
        return WriteLibrarySaveSnapshot(value, state);
    });
    const auto failed = worker.Submit(snapshot); worker.RequestFlush(); TEST_ASSERT(worker.WaitUntil(failed));
    const auto results = worker.TakeResults(); TEST_ASSERT_EQ(results.size(), size_t(1)); TEST_ASSERT(!results[0].is_ok());
    const auto retry = worker.Submit(snapshot); TEST_ASSERT(retry > failed);
    worker.RequestFlush(); TEST_ASSERT(worker.WaitUntil(retry));
    const auto retried = worker.TakeResults(); TEST_ASSERT_EQ(retried.size(), size_t(1)); TEST_ASSERT(retried[0].is_ok());
}

TEST_CASE(LibrarySaving, AllocationFailurePublishesCompletionAndCanRetry) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath(), 1);
    const auto snapshot = library.CaptureSaveSnapshot();
    unsigned calls = 0;
    LibrarySaveWorker worker([&](const MediaLibrarySaveSnapshot& value, MediaLibrarySaveState& state) {
        if (++calls == 1) throw std::bad_alloc();
        return WriteLibrarySaveSnapshot(value, state);
    });
    const auto failed = worker.Submit(snapshot); worker.RequestFlush();
    TEST_ASSERT(worker.WaitUntil(failed));
    TEST_ASSERT(worker.IsComplete(failed)); TEST_ASSERT(!worker.WasSuccessful(failed));
    const auto failures = worker.TakeResults();
    TEST_ASSERT_EQ(failures.size(), size_t(1));
    TEST_ASSERT(!failures.front().is_ok() && !failures.front().libraryStatus.is_ok());
    TEST_ASSERT_EQ(failures.front().libraryStatus.win32Error, DWORD(ERROR_NOT_ENOUGH_MEMORY));
    const auto retry = worker.Submit(snapshot); worker.RequestFlush();
    TEST_ASSERT(retry > failed && worker.WaitUntil(retry) && worker.WasSuccessful(retry));
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems.size(), size_t(1));
    TEST_ASSERT_EQ(reloaded.mediaItems.front().id, library.mediaItems.front().id);
}

TEST_CASE(LibrarySaving, CopiedModelDoesNotInvokeOriginalOwnerSaveHandler) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    unsigned dispatched = 0;
    library.SetSaveRequestHandler([&] { ++dispatched; return MediaLibrarySaveResult{}; });
    TEST_ASSERT(library.Save().is_ok()); TEST_ASSERT_EQ(dispatched, 1u);
    MediaLibrary copy = library;
    TEST_ASSERT(copy.Save().is_ok()); TEST_ASSERT_EQ(dispatched, 1u);
    bool alternateSaved = false;
    std::thread alternate([&] { alternateSaved = library.Save().is_ok(); }); alternate.join();
    TEST_ASSERT(alternateSaved);
    TEST_ASSERT_EQ(dispatched, 1u);
    library.SetSaveRequestHandler({}); TEST_ASSERT(library.Save().is_ok()); TEST_ASSERT_EQ(dispatched, 1u);
}

TEST_CASE(LibrarySaving, UnavailableRootIsNotCreatedByWritabilityProbe) {
    ScopedTempDir temp;
    const auto missing = temp.SubPath(L"disconnected-root");
    TEST_ASSERT(!CheckPortableRootAvailability(missing).is_ok());
    TEST_ASSERT(!CheckDataDirectoryWritability(missing).is_ok());
    TEST_ASSERT(!DirectoryExists(missing));
    TEST_ASSERT(temp.WriteFile(L"root-file", "file"));
    TEST_ASSERT(!CheckPortableRootAvailability(temp.SubPath(L"root-file")).is_ok());
}

TEST_CASE(LibrarySaving, IncompleteSnapshotCannotWriteOrResetWriterState) {
    MediaLibrarySaveState state; state.seriesId = 42;
    TEST_ASSERT(!WriteLibrarySaveSnapshot({}, state).is_ok());
    TEST_ASSERT_EQ(state.seriesId, 42ull);
    TEST_ASSERT_EQ(state.serializedStoreCount, 0ull);
}

TEST_CASE(LibrarySaving, DirectEditsProduceImmutablePerStoreGenerations) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath());
    const auto before = library.CaptureSaveSnapshot();
    TEST_ASSERT(library.CaptureSaveSnapshot(before) == before);
    library.mediaItems[0].lastPosSec = 42;
    const auto progress = library.CaptureSaveSnapshot(before);
    TEST_ASSERT_EQ(progress->libraryGeneration, before->libraryGeneration + 1);
    TEST_ASSERT_EQ(progress->playlistsGeneration, before->playlistsGeneration);
    TEST_ASSERT_EQ(progress->historyGeneration, before->historyGeneration);
    TEST_ASSERT_EQ(progress->watchedFoldersGeneration, before->watchedFoldersGeneration);
    TEST_ASSERT_EQ((*before->mediaItems)[0]->lastPosSec, 0.0);
    TEST_ASSERT_EQ((*progress->mediaItems)[0]->lastPosSec, 42.0);
    TEST_ASSERT((*before->mediaItems)[1] == (*progress->mediaItems)[1]);
    library.history.push_back({library.mediaItems[0].filePath, L"History", 25, 120, 100});
    const auto history = library.CaptureSaveSnapshot(progress);
    TEST_ASSERT_EQ(history->libraryGeneration, progress->libraryGeneration);
    TEST_ASSERT_EQ(history->historyGeneration, progress->historyGeneration + 1);
    TEST_ASSERT(history->mediaItems == progress->mediaItems);
}

TEST_CASE(LibrarySaving, ReorderingAndRemovalReuseUnchangedRecords) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath());
    const auto before = library.CaptureSaveSnapshot();
    std::swap(library.mediaItems[0], library.mediaItems[2]);
    const auto reordered = library.CaptureSaveSnapshot(before);
    TEST_ASSERT((*reordered->mediaItems)[0] == (*before->mediaItems)[2]);
    TEST_ASSERT((*reordered->mediaItems)[2] == (*before->mediaItems)[0]);
    library.mediaItems.erase(library.mediaItems.begin());
    const auto removed = library.CaptureSaveSnapshot(reordered);
    TEST_ASSERT_EQ(removed->mediaItems->size(), size_t(2));
    TEST_ASSERT((*removed->mediaItems)[0] == (*reordered->mediaItems)[1]);
    library.mediaItems.clear();
    TEST_ASSERT(library.CaptureSaveSnapshot(removed)->mediaItems->empty());
    TEST_ASSERT_EQ(before->mediaItems->size(), size_t(3));
}

TEST_CASE(LibrarySaving, UnchangedStoresSkipSerializationAndDeletedStoreIsRecreated) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath());
    auto snapshot = library.CaptureSaveSnapshot();
    MediaLibrarySaveState state;
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, state).is_ok());
    TEST_ASSERT_EQ(state.serializedStoreCount, 4ull);
    const auto unchanged = WriteLibrarySaveSnapshot(*snapshot, state);
    TEST_ASSERT(unchanged.is_ok()); TEST_ASSERT(!unchanged.librarySaved);
    TEST_ASSERT_EQ(state.serializedStoreCount, 4ull);
    library.mediaItems[0].lastPosSec = 42;
    snapshot = library.CaptureSaveSnapshot(snapshot);
    const auto progress = WriteLibrarySaveSnapshot(*snapshot, state);
    TEST_ASSERT(progress.is_ok()); TEST_ASSERT(progress.librarySaved);
    TEST_ASSERT(!progress.playlistsSaved && !progress.historySaved && !progress.watchedFoldersSaved);
    TEST_ASSERT_EQ(state.serializedStoreCount, 5ull);
    TEST_ASSERT(DeleteFileW(temp.SubPath(L"data\\history.json").c_str()));
    const auto recreated = WriteLibrarySaveSnapshot(*snapshot, state);
    TEST_ASSERT(recreated.historySaved && !recreated.librarySaved);
    TEST_ASSERT_EQ(state.serializedStoreCount, 6ull);
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(reloaded.mediaItems[0].lastPosSec, 42.0);
    TEST_ASSERT_EQ(reloaded.mediaItems[0].filePath, library.mediaItems[0].filePath);
}

TEST_CASE(LibrarySaving, UnknownFieldsAndNestedDirectChangesSurviveRoundtrip) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath(), 1);
    auto& item = library.mediaItems.front();
    item.preferences.emplace(); item.preferences->audioTrackId = 2;
    item.bookmarks.push_back({"bookmark", 15, L"Scene", 100});
    item.overrides.emplace(); item.overrides->title = L"Custom title";
    item.extraFields = JsonValue::parse("{\"plugin\":{\"values\":[1,2,3],\"name\":\"initial\"}}");
    TEST_ASSERT(library.Save().is_ok());
    item.preferences->audioTrackId = 3;
    item.bookmarks.front().label = L"Updated scene";
    item.overrides->year = 2000;
    item.extraFields["plugin"]["name"] = "updated";
    TEST_ASSERT(library.Save().librarySaved);
    MediaLibrary reloaded; TEST_ASSERT(reloaded.LoadFromRoot(temp.GetPath()).is_ok());
    const auto& loaded = reloaded.mediaItems.front();
    TEST_ASSERT_EQ(*loaded.preferences->audioTrackId, 3);
    TEST_ASSERT_EQ(loaded.bookmarks.front().label, std::wstring(L"Updated scene"));
    TEST_ASSERT_EQ(*loaded.overrides->year, 2000);
    TEST_ASSERT_EQ(loaded.extraFields.get("plugin").get("name").as_string(), std::string("updated"));
}

TEST_CASE(LibrarySaving, FailedStoreIsFencedWhileHealthyStoresCanSave) {
    ScopedTempDir temp; temp.CreateSubDir(L"data");
    const auto path = temp.SubPath(L"data\\library.json");
    const std::string future = "{\"schemaVersion\":999,\"items\":[]}";
    TEST_ASSERT(AtomicWriteTextFile(path, future).is_ok());
    MediaLibrary library; TEST_ASSERT(!library.LoadFromRoot(temp.GetPath()).is_ok());
    const auto saved = library.Save();
    TEST_ASSERT(!saved.is_ok() && !saved.libraryStatus.is_ok());
    TEST_ASSERT(!saved.librarySaved && saved.playlistsSaved && saved.historySaved && saved.watchedFoldersSaved);
    std::string after; TEST_ASSERT(ReadTextFile(path, after).is_ok()); TEST_ASSERT_EQ(after, future);
}

TEST_CASE(LibrarySaving, FailedWriteIsNotAcknowledgedAndCanRetrySameSnapshot) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath(), 1);
    auto snapshot = library.CaptureSaveSnapshot(); MediaLibrarySaveState state;
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, state).is_ok());
    const auto oldGeneration = state.libraryGeneration;
    library.mediaItems[0].lastPosSec = 42;
    snapshot = library.CaptureSaveSnapshot(snapshot);
    const auto path = temp.SubPath(L"data\\library.json");
    TEST_ASSERT(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY));
    const auto failed = WriteLibrarySaveSnapshot(*snapshot, state);
    TEST_ASSERT(!failed.libraryStatus.is_ok());
    TEST_ASSERT_EQ(state.libraryGeneration, oldGeneration);
    TEST_ASSERT(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL));
    TEST_ASSERT(WriteLibrarySaveSnapshot(*snapshot, state).librarySaved);
    TEST_ASSERT_EQ(state.libraryGeneration, snapshot->libraryGeneration);
}

TEST_CASE(LibrarySaving, MissingPortableRootIsProtectedAndReloadStartsANewSeries) {
    ScopedTempDir temp;
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    LibrarySavingFixture::AddRecords(library, temp.GetPath(), 1);
    const auto before = library.CaptureSaveSnapshot();
    TEST_ASSERT(library.Save().is_ok());
    TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT(library.CaptureSaveSnapshot()->seriesId != before->seriesId);
    TEST_ASSERT(!library.LoadFromRoot(temp.SubPath(L"unavailable")).is_ok());
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(1));
    TEST_ASSERT(!library.Save().is_ok());
    TEST_ASSERT(GetFileAttributesW(temp.SubPath(L"unavailable").c_str()) == INVALID_FILE_ATTRIBUTES);
}

} // namespace Aperture::Test
