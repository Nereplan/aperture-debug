#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(UnwritableRoot, ReadOnlyFailureAndNoExternalFallback) {
    StorageStatus status = CheckDataDirectoryWritability();
    TEST_ASSERT(status.is_ok());
    TEST_ASSERT_EQ(status.code, StorageErrorCode::Success);

    StorageStatus invalidStatus = AtomicWriteTextFile(L"", "test");
    TEST_ASSERT(!invalidStatus.is_ok());
    TEST_ASSERT_EQ(invalidStatus.code, StorageErrorCode::InvalidPath);

    std::wstring nonExistentDrive;
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'A'; --letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            nonExistentDrive = std::wstring(1, letter) + L":\\NonExistentDriveDirectory12345\\file.txt";
            break;
        }
    }
    TEST_ASSERT(!nonExistentDrive.empty());
    StorageStatus status2 = AtomicWriteTextFile(nonExistentDrive, "test");
    TEST_ASSERT(!status2.is_ok());
    TEST_ASSERT(status2.code == StorageErrorCode::DirectoryCreationFailed || status2.code == StorageErrorCode::FileOpenFailed);

    ScopedTempDir tempDir;
    std::wstring roFile = tempDir.SubPath(L"locked_file.json");
    tempDir.WriteFile(L"locked_file.json", "initial");
    TEST_ASSERT(SetFileAttributesW(roFile.c_str(), FILE_ATTRIBUTE_READONLY));

    StorageStatus roStatus = AtomicWriteTextFile(roFile, "new_content");
    TEST_ASSERT(!roStatus.is_ok());
    TEST_ASSERT_EQ(roStatus.code, StorageErrorCode::AtomicRenameFailed);
    std::string preserved;
    TEST_ASSERT(ReadTextFile(roFile, preserved).is_ok());
    TEST_ASSERT_EQ(preserved, std::string("initial"));
    SetFileAttributesW(roFile.c_str(), FILE_ATTRIBUTE_NORMAL);

    StorageStatus err = StorageStatus::Error(StorageErrorCode::DirectoryNotWritable, ERROR_ACCESS_DENIED, L"C:\\Aperture\\data", "Access denied to data dir");
    TEST_ASSERT(!err.is_ok());
    TEST_ASSERT(!static_cast<bool>(err));
    TEST_ASSERT_EQ(err.code, StorageErrorCode::DirectoryNotWritable);
    TEST_ASSERT_EQ(err.win32Error, (DWORD)ERROR_ACCESS_DENIED);
    TEST_ASSERT_EQ(err.path, L"C:\\Aperture\\data");
    TEST_ASSERT_EQ(err.message, "Access denied to data dir");
}

} // namespace Aperture::Test
