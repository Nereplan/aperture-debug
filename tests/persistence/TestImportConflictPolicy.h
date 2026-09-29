#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(ImportConflictPolicy, KeepExistingDefaultPolicy) {
    ScopedTempDir tempDir(L"Aperture_Conflict_Keep_");
    std::wstring videoPath = tempDir.SubPath(L"video.mp4");
    tempDir.WriteFile(L"video.mp4", "mock video file stream content 12345");

    MediaLibrary library;
    // Initial import
    MediaImportResult r1 = library.ImportPaths({ videoPath }, {}, false, ImportConflictPolicy::KeepExisting);
    TEST_ASSERT_EQ(r1.added, 1);
    TEST_ASSERT_EQ(r1.conflictCount, 0);

    MediaItem* item = library.FindByPath(videoPath);
    TEST_ASSERT(item != nullptr);
    std::string origId = item->id;

    // Second import with KeepExisting
    MediaImportResult r2 = library.ImportPaths({ videoPath }, {}, false, ImportConflictPolicy::KeepExisting);
    TEST_ASSERT_EQ(r2.conflictCount, 1);
    TEST_ASSERT_EQ(r2.conflictResolvedKeep, 1);
    TEST_ASSERT_EQ(r2.conflicts.size(), 1);
    TEST_ASSERT_EQ(r2.conflicts[0].existingId, origId);
    TEST_ASSERT_EQ(r2.conflicts[0].resolution, "kept_existing");
    TEST_ASSERT_EQ(library.mediaItems.size(), 1);
}

TEST_CASE(ImportConflictPolicy, OverwritePolicy) {
    ScopedTempDir tempDir(L"Aperture_Conflict_Ovr_");
    std::wstring videoPath = tempDir.SubPath(L"clip.mp4");
    tempDir.WriteFile(L"clip.mp4", "mock video 1");

    MediaLibrary library;
    library.ImportPaths({ videoPath }, {}, false);
    MediaItem* item = library.FindByPath(videoPath);
    TEST_ASSERT(item != nullptr);
    item->title = L"Custom User Title";

    // Re-import with Overwrite policy
    MediaImportResult r = library.ImportPaths({ videoPath }, {}, false, ImportConflictPolicy::Overwrite);
    TEST_ASSERT_EQ(r.conflictCount, 1);
    TEST_ASSERT_EQ(r.conflictResolvedOverwrite, 1);
    TEST_ASSERT_EQ(r.conflicts.size(), 1);
    TEST_ASSERT_EQ(r.conflicts[0].resolution, "overwritten");
    TEST_ASSERT_EQ(r.updated, 1);

    // Title should be restored to file name
    MediaItem* updatedItem = library.FindByPath(videoPath);
    TEST_ASSERT_EQ(updatedItem->title, L"clip.mp4");
}

TEST_CASE(ImportConflictPolicy, SkipPolicy) {
    ScopedTempDir tempDir(L"Aperture_Conflict_Skip_");
    std::wstring videoPath = tempDir.SubPath(L"test.mp4");
    tempDir.WriteFile(L"test.mp4", "mock video content");

    MediaLibrary library;
    library.ImportPaths({ videoPath }, {}, false);

    MediaImportResult r = library.ImportPaths({ videoPath }, {}, false, ImportConflictPolicy::Skip);
    TEST_ASSERT_EQ(r.conflictCount, 1);
    TEST_ASSERT_EQ(r.conflictResolvedSkip, 1);
    TEST_ASSERT_EQ(r.unchanged, 1);
    TEST_ASSERT_EQ(r.conflicts[0].resolution, "skipped");
}

TEST_CASE(ImportConflictPolicy, CreateCopyPolicy) {
    ScopedTempDir tempDir(L"Aperture_Conflict_Copy_");
    std::wstring videoPath = tempDir.SubPath(L"movie.mkv");
    tempDir.WriteFile(L"movie.mkv", "mock mkv stream");

    MediaLibrary library;
    library.ImportPaths({ videoPath }, {}, false);
    TEST_ASSERT_EQ(library.mediaItems.size(), 1);

    MediaImportResult r = library.ImportPaths({ videoPath }, {}, false, ImportConflictPolicy::CreateCopy);
    TEST_ASSERT_EQ(r.conflictCount, 1);
    TEST_ASSERT_EQ(r.conflictResolvedCopy, 1);
    TEST_ASSERT_EQ(r.added, 1);
    TEST_ASSERT_EQ(r.conflicts[0].resolution, "created_copy");

    // Library now contains 2 entries for the video
    TEST_ASSERT_EQ(library.mediaItems.size(), 2);
    TEST_ASSERT_NEQ(library.mediaItems[0].id, library.mediaItems[1].id);
    TEST_ASSERT(library.mediaItems[0].title.find(L"(Copy)") != std::wstring::npos ||
                library.mediaItems[1].title.find(L"(Copy)") != std::wstring::npos);
}

TEST_CASE(ImportConflictPolicy, ReportsCompletedOutcome) {
    ScopedTempDir tempDir(L"Aperture_Import_Completed_");
    const std::wstring videoPath = tempDir.SubPath(L"complete.mp4");
    tempDir.WriteFile(L"complete.mp4", "mock video");

    MediaLibrary library;
    const MediaImportResult result = library.ImportPaths({ videoPath }, {}, false);
    TEST_ASSERT(result.outcome == LongOperationOutcome::Completed);
    TEST_ASSERT(result.status.is_ok());
}

TEST_CASE(ImportConflictPolicy, ReportsCanceledOutcomeWithoutFailure) {
    ScopedTempDir tempDir(L"Aperture_Import_Canceled_");
    const std::wstring videoPath = tempDir.SubPath(L"cancel.mp4");
    tempDir.WriteFile(L"cancel.mp4", "mock video");

    MediaLibrary library;
    const MediaImportResult result = library.ImportPaths(
        { videoPath },
        [](size_t, size_t, const std::wstring&) { return false; },
        false);
    TEST_ASSERT(result.outcome == LongOperationOutcome::Canceled);
    TEST_ASSERT(result.status.is_ok());
    TEST_ASSERT_EQ(result.acceptedFiles, 0);
}

} // namespace Aperture::Test
