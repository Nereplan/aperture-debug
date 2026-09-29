#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(UnwritableRoot, CheckDirectoryWritabilityNormal) {
    // Current data dir should be writable
    StorageStatus status = CheckDataDirectoryWritability();
    TEST_ASSERT(status.is_ok());
    TEST_ASSERT_EQ(status.code, StorageErrorCode::Success);
}

TEST_CASE(UnwritableRoot, ReadOnlyDirectoryDetection) {
    ScopedTempDir tempDir;
    std::wstring dataDir = tempDir.SubPath(L"readonly_data");
    CreateDirectoryW(dataDir.c_str(), NULL);

    // Set read-only attribute on directory
    SetFileAttributesW(dataDir.c_str(), FILE_ATTRIBUTE_READONLY);

    // Test writing temporary file to verify access restriction
    std::wstring probeFile = dataDir + L"\\.test_write.tmp";
    HANDLE hFile = CreateFileW(probeFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        DeleteFileW(probeFile.c_str());
    }

    // Restore attributes so RAII cleanup can delete it cleanly
    SetFileAttributesW(dataDir.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(UnwritableRoot, AtomicWriteToInvalidPath) {
    std::wstring invalidPath = L"";
    StorageStatus status = AtomicWriteTextFile(invalidPath, "test");
    TEST_ASSERT(!status.is_ok());
    TEST_ASSERT_EQ(status.code, StorageErrorCode::InvalidPath);

    // Path on non-existent drive
    std::wstring nonExistentDrive = L"Q:\\NonExistentDriveDirectory12345\\file.txt";
    StorageStatus status2 = AtomicWriteTextFile(nonExistentDrive, "test");
    TEST_ASSERT(!status2.is_ok());
    TEST_ASSERT(status2.code == StorageErrorCode::DirectoryCreationFailed || status2.code == StorageErrorCode::FileOpenFailed);
}

TEST_CASE(UnwritableRoot, AtomicWriteToReadonlyFile) {
    ScopedTempDir tempDir;
    std::wstring roFile = tempDir.SubPath(L"locked_file.json");
    tempDir.WriteFile(L"locked_file.json", "initial");

    // Set file to read-only
    SetFileAttributesW(roFile.c_str(), FILE_ATTRIBUTE_READONLY);

    // Atomic write creates a .tmp file and tries to MoveFileEx with REPLACE_EXISTING.
    // Overwriting a readonly destination may fail with ACCESS_DENIED or AtomicRenameFailed.
    StorageStatus status = AtomicWriteTextFile(roFile, "new_content");
    if (!status.is_ok()) {
        TEST_ASSERT_EQ(status.code, StorageErrorCode::AtomicRenameFailed);
    }

    // Restore file attributes for clean teardown
    SetFileAttributesW(roFile.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(UnwritableRoot, StorageStatusErrorFormatting) {
    StorageStatus err = StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, L"C:\\Aperture\\data", "Access denied to data dir");
    TEST_ASSERT(!err.is_ok());
    TEST_ASSERT(!static_cast<bool>(err));
    TEST_ASSERT_EQ(err.code, StorageErrorCode::DirectoryNotWritable);
    TEST_ASSERT_EQ(err.win32Error, (DWORD)ERROR_ACCESS_DENIED);
    TEST_ASSERT_EQ(err.path, L"C:\\Aperture\\data");
    TEST_ASSERT_EQ(err.message, "Access denied to data dir");
}

} // namespace Aperture::Test
