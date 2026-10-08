#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/MediaImportProbeCache.h"

namespace Aperture::Test {

namespace ImportFingerprintTests {
inline bool SetWriteTime(const std::wstring& path, uint64_t value) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    const BOOL ok = SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
    return ok != FALSE;
}
inline MediaProbeFunction Probe(size_t& calls) {
    return [&calls](const std::wstring&, double& duration, uint32_t& width, uint32_t& height) {
        ++calls; duration = 100; width = 1920; height = 1080; return true;
    };
}
}

TEST_CASE(ImportFingerprints, PersistedFingerprintSkipsRepeatProbeAndNoOpMerge) {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"movie.mkv", "media"));
    const auto path = temp.SubPath(L"movie.mkv");
    MediaLibrary library; TEST_ASSERT(library.LoadFromRoot(temp.GetPath()).is_ok());
    size_t probes = 0;
    const auto probe = ImportFingerprintTests::Probe(probes);
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, probe);
    TEST_ASSERT_EQ(probes, size_t(1));
    TEST_ASSERT(library.mediaItems[0].metadataFileWriteTime != 0);
    const auto stamp = library.mediaItems[0].metadataFileWriteTime;
    TEST_ASSERT(library.Save().is_ok());
    MediaLibrary restored; TEST_ASSERT(restored.LoadFromRoot(temp.GetPath()).is_ok());
    TEST_ASSERT_EQ(restored.mediaItems[0].metadataFileWriteTime, stamp);
    MediaImportProbeCache cache(MediaImportProbeCache::Capture(restored));
    MediaLibrary staged;
    const auto imported = staged.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, &cache, probe);
    TEST_ASSERT_EQ(imported.reusedMetadata, size_t(1));
    TEST_ASSERT_EQ(probes, size_t(1));
    const auto baseline = *cache.Find(path);
    restored.mediaItems[0].lastPosSec = 55;
    restored.mediaItems[0].overrides.emplace(); restored.mediaItems[0].overrides->title = L"Current user title";
    const auto revisions = restored.GetRevisions(); const auto text = restored.GetSearchContentVersion(); const auto version = restored.GetVersion();
    const auto result = restored.MergeImportedMedia(std::move(staged.mediaItems), {baseline});
    TEST_ASSERT_EQ(result.updatedCount, size_t(0));
    TEST_ASSERT(!result.viewChanged);
    TEST_ASSERT_EQ(restored.GetVersion(), version);
    TEST_ASSERT_EQ(restored.GetRevisions().catalog, revisions.catalog);
    TEST_ASSERT_EQ(restored.GetSearchContentVersion(), text);
    TEST_ASSERT_EQ(restored.mediaItems[0].lastPosSec, 55.0);
    TEST_ASSERT_EQ(restored.mediaItems[0].GetDisplayTitle(), std::wstring(L"Current user title"));
}

TEST_CASE(ImportFingerprints, SameSizeModificationAndReplacementRequireProbe) {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"movie.mkv", "first"));
    const auto path = temp.SubPath(L"movie.mkv");
    MediaLibrary library;
    size_t probes = 0; const auto probe = ImportFingerprintTests::Probe(probes);
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, probe);
    const auto stamp = library.mediaItems[0].metadataFileWriteTime;
    TEST_ASSERT(temp.WriteFile(L"movie.mkv", "later"));
    TEST_ASSERT(ImportFingerprintTests::SetWriteTime(path, stamp + 20000000));
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, probe);
    TEST_ASSERT_EQ(probes, size_t(2));
    const auto replacementTime = library.mediaItems[0].metadataFileWriteTime;
    const auto oldIndex = library.mediaItems[0].fileIndex;
    TEST_ASSERT(temp.WriteFile(L"replacement.mkv", "other"));
    TEST_ASSERT(ImportFingerprintTests::SetWriteTime(temp.SubPath(L"replacement.mkv"), replacementTime));
    TEST_ASSERT(MoveFileExW(temp.SubPath(L"replacement.mkv").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE);
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, probe);
    TEST_ASSERT_EQ(probes, size_t(3));
    TEST_ASSERT(library.mediaItems[0].fileIndex != oldIndex);
    TEST_ASSERT_EQ(library.mediaItems[0].metadataFileWriteTime, replacementTime);
}

TEST_CASE(ImportFingerprints, OpenWriterAndChangingProbeCannotCreateTrustedFingerprint) {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"movie.mkv", "media"));
    const auto path = temp.SubPath(L"movie.mkv");
    MediaLibrary library;
    size_t probes = 0; const auto probe = ImportFingerprintTests::Probe(probes);
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, probe);
    MediaImportProbeCache cache(MediaImportProbeCache::Capture(library));
    {
        HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        TEST_ASSERT(writer != INVALID_HANDLE_VALUE);
        struct Closer { HANDLE handle; ~Closer() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } closer{writer};
        MediaLibrary staged;
        const auto result = staged.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, &cache, probe);
        TEST_ASSERT_EQ(result.reusedMetadata, size_t(0));
        TEST_ASSERT_EQ(probes, size_t(2));
        TEST_ASSERT_EQ(staged.mediaItems[0].metadataFileWriteTime, uint64_t(0));
    }
    const auto before = ReadMediaFileFingerprint(path);
    bool changed = false;
    MediaProbeFunction changing = [&](const std::wstring&, double& duration, uint32_t& width, uint32_t& height) {
        changed = temp.WriteFile(L"movie.mkv", "later") && ImportFingerprintTests::SetWriteTime(path, before.writeTime + 20000000);
        duration = 120; width = 1920; height = 1080; return true;
    };
    MediaLibrary staged;
    staged.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, changing);
    TEST_ASSERT(changed);
    TEST_ASSERT_EQ(staged.mediaItems[0].metadataFileWriteTime, uint64_t(0));
}

TEST_CASE(ImportFingerprints, FailedImportPreservesUsefulMetadataAndRejectsStaleMerge) {
    ScopedTempDir temp;
    TEST_ASSERT(temp.WriteFile(L"movie.mkv", "media"));
    const auto path = temp.SubPath(L"movie.mkv");
    MediaLibrary library;
    size_t probes = 0;
    library.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, nullptr, ImportFingerprintTests::Probe(probes));
    MediaImportProbeCache cache(MediaImportProbeCache::Capture(library));
    const auto baseline = *cache.Find(path);
    library.mediaItems[0].lastPosSec = 60;
    TEST_ASSERT(ImportFingerprintTests::SetWriteTime(path, baseline.fileWriteTime + 20000000));
    MediaLibrary staged;
    MediaProbeFunction failed = [](const std::wstring&, double&, uint32_t&, uint32_t&) { return false; };
    staged.ImportPaths({path}, {}, false, ImportConflictPolicy::KeepExisting, &cache, failed);
    const auto copy = staged.mediaItems;
    auto result = library.MergeImportedMedia(std::move(staged.mediaItems), {baseline});
    TEST_ASSERT_EQ(result.updatedCount, size_t(1));
    TEST_ASSERT_EQ(library.mediaItems[0].durationSec, 100.0);
    TEST_ASSERT_EQ(library.mediaItems[0].lastPosSec, 60.0);
    TEST_ASSERT_EQ(library.mediaItems[0].videoWidth, uint32_t(1920));
    TEST_ASSERT(library.mediaItems[0].metadataProbeFailed);
    TEST_ASSERT_EQ(library.mediaItems[0].metadataFileWriteTime, uint64_t(0));
    result = library.MergeImportedMedia(copy, {baseline});
    TEST_ASSERT_EQ(result.updatedCount, size_t(0));
    TEST_ASSERT_EQ(result.discardedCount, size_t(1));
}

} // namespace Aperture::Test
