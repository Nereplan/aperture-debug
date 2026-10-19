#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/FolderRediscovery.h"

namespace Aperture::Test {

namespace RediscoveryTests {
inline MediaItem File(const std::string& id, const std::wstring& path) {
    MediaItem item; item.id = id; item.filePath = path; item.title = GetFileNameFromPath(path);
    item.fileSizeBytes = GetFileSizeInBytes(path);
    const auto identity = GetWindowsFileIdentity(path);
    if (identity.valid) { item.fileIndex = identity.fileIndex; item.volumeSerialNumber = identity.volumeSerialNumber; }
    return item;
}
}
static void step_FolderRediscovery_PreparedMovesPreserveConcurrentUserEditsAndReferences() {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(LR"(Old\episode.mkv)", "video"));
    MediaLibrary library;
    auto item = RediscoveryTests::File("one", temp.SubPath(LR"(Old\episode.mkv)"));
    library.mediaItems.push_back(item);
    library.watchedFolders = {temp.SubPath(L"Old")};
    Playlist playlist; playlist.id = "playlist"; playlist.itemPaths = {item.filePath}; library.playlists.push_back(playlist);
    HistoryRecord history; history.filePath = item.filePath; history.lastPosSec = 12; library.history.push_back(history);
    library.RebuildPathIndex();
    size_t saves = 0; library.SetSaveRequestHandler([&] { ++saves; return MediaLibrarySaveResult{}; });
    const auto snapshot = FolderRediscoverySnapshot::Capture(library);
    TEST_ASSERT(MoveFileW(temp.SubPath(L"Old").c_str(), temp.SubPath(L"New").c_str()) != FALSE);
    const auto prepared = PrepareFolderRediscovery(snapshot);
    TEST_ASSERT_EQ(prepared.moves.size(), size_t(1));
    library.mediaItems[0].lastPosSec = 90;
    library.mediaItems[0].bookmarks.push_back({"new-bookmark", 45, L"During discovery"});
    library.mediaItems[0].overrides.emplace(); library.mediaItems[0].overrides->title = L"Custom";
    const auto before = library.GetRevisions();
    const auto result = library.ApplyFolderRediscovery(prepared, false);
    TEST_ASSERT_EQ(result.relocatedItemsCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 90.0);
    TEST_ASSERT_EQ(library.mediaItems[0].bookmarks.size(), size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].GetDisplayTitle(), std::wstring(L"Custom"));
    TEST_ASSERT_EQ(library.watchedFolders[0], temp.SubPath(L"New"));
    TEST_ASSERT_EQ(library.playlists[0].itemPaths[0], temp.SubPath(LR"(New\episode.mkv)"));
    TEST_ASSERT_EQ(library.history[0].filePath, library.playlists[0].itemPaths[0]);
    TEST_ASSERT_EQ(library.GetRevisions().catalog, before.catalog + 1);
    TEST_ASSERT_EQ(saves, size_t(0));
}
static void step_FolderRediscovery_DuplicateMergeUsesLiveStateAndNeverSavesWhenDisabled() {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(LR"(New\movie.mkv)", "video"));
    MediaLibrary library;
    MediaItem old; old.id = "old"; old.filePath = temp.SubPath(LR"(Old\movie.mkv)");
    library.mediaItems = {old, RediscoveryTests::File("new", temp.SubPath(LR"(New\movie.mkv)"))};
    library.RebuildPathIndex();
    const auto prepared = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library));
    TEST_ASSERT_EQ(prepared.moves.size(), size_t(1));
    library.mediaItems[0].lastPosSec = 100; library.mediaItems[0].durationSec = 300; library.mediaItems[0].status = "in_progress";
    library.mediaItems[1].bookmarks.push_back({"keep", 20, L"Current target"});
    size_t saves = 0; library.SetSaveRequestHandler([&] { ++saves; return MediaLibrarySaveResult{}; });
    const auto result = library.ApplyFolderRediscovery(prepared, false);
    TEST_ASSERT_EQ(result.mergedItemsCount, size_t(1));
    TEST_ASSERT_EQ(result.removedGhostCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].id, std::string("new"));
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 100.0);
    TEST_ASSERT_EQ(library.mediaItems[0].durationSec, 300.0);
    TEST_ASSERT_EQ(library.mediaItems[0].bookmarks[0].id, std::string("keep"));
    TEST_ASSERT_EQ(saves, size_t(0));
}
static void step_FolderRediscovery_StaleFileFactsAndAmbiguousCandidatesAreRetained() {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(LR"(ChoiceA\movie.mkv)", "video"));
    TEST_ASSERT(temp.WriteFile(LR"(ChoiceB\movie.mkv)", "video"));
    MediaLibrary library;
    MediaItem old; old.id = "old"; old.filePath = temp.SubPath(LR"(Old\movie.mkv)"); old.fileSizeBytes = 5;
    library.mediaItems.push_back(old); library.RebuildPathIndex();
    auto prepared = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library));
    TEST_ASSERT(prepared.moves.empty());
    TEST_ASSERT(DeleteFileW(temp.SubPath(LR"(ChoiceB\movie.mkv)").c_str()) != FALSE);
    prepared = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library));
    TEST_ASSERT_EQ(prepared.moves.size(), size_t(1));
    library.mediaItems[0].fileSizeBytes = 10;
    const auto result = library.ApplyFolderRediscovery(prepared, false);
    TEST_ASSERT(result.needsRescan);
    TEST_ASSERT_EQ(result.relocatedItemsCount, size_t(0));
    TEST_ASSERT_EQ(library.mediaItems[0].filePath, old.filePath);
}
static void step_FolderRediscovery_CandidateListingsAreReusedAcrossMissingFolders() {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(LR"(NewA\a.mkv)", "one"));
    TEST_ASSERT(temp.WriteFile(LR"(NewB\b.mkv)", "two"));
    MediaLibrary library;
    MediaItem a; a.id = "a"; a.filePath = temp.SubPath(LR"(OldA\a.mkv)"); a.fileSizeBytes = 3;
    MediaItem b = a; b.id = "b"; b.filePath = temp.SubPath(LR"(OldB\b.mkv)");
    library.mediaItems = {a, b};
    const auto prepared = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library));
    TEST_ASSERT_EQ(prepared.moves.size(), size_t(2));
    TEST_ASSERT_EQ(prepared.directoryScans, size_t(2));
    auto cancel = std::make_shared<OperationCancellation>(); cancel->Cancel();
    const auto canceled = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library), cancel);
    TEST_ASSERT(canceled.canceled);
    TEST_ASSERT(canceled.moves.empty());
    TEST_ASSERT_EQ(canceled.directoryScans, size_t(0));
}
static void step_FolderRediscovery_CompetingSourcesCannotClaimTheSameDestination() {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(LR"(New\movie.mkv)", "video"));
    MediaLibrary library;
    MediaItem a; a.id = "a"; a.filePath = temp.SubPath(LR"(OldA\movie.mkv)"); a.fileSizeBytes = 5;
    MediaItem b = a; b.id = "b"; b.filePath = temp.SubPath(LR"(OldB\movie.mkv)");
    library.mediaItems = {a, b};
    const auto prepared = PrepareFolderRediscovery(FolderRediscoverySnapshot::Capture(library));
    TEST_ASSERT(prepared.moves.empty());
    TEST_ASSERT_EQ(library.ApplyFolderRediscovery(prepared, false).removedGhostCount, size_t(0));
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(2));
}


TEST_CASE(FolderRediscovery, MovePreservationAndGhostMerge) {
    step_FolderRediscovery_PreparedMovesPreserveConcurrentUserEditsAndReferences();
    step_FolderRediscovery_DuplicateMergeUsesLiveStateAndNeverSavesWhenDisabled();
    step_FolderRediscovery_StaleFileFactsAndAmbiguousCandidatesAreRetained();
    step_FolderRediscovery_CandidateListingsAreReusedAcrossMissingFolders();
    step_FolderRediscovery_CompetingSourcesCannotClaimTheSameDestination();
}

} // namespace Aperture::Test
