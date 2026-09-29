#pragma once

#include "../support/TestFramework.h"
#include "../../src/Updater.h"
#include <windows.h>
#include <string>

namespace Aperture::Test {

TEST_CASE(UpdaterTempCleanup, ScopedTempDirectoryCleanup) {
    std::wstring createdPath;
    {
        ScopedTempDirectory tempDir(L"Aperture_TestUpdate_");
        createdPath = tempDir.GetPath();

        TEST_ASSERT(!createdPath.empty());
        TEST_ASSERT(DirectoryExists(createdPath));

        // Create staged files inside
        std::wstring subFile = tempDir.SubPath(L"staged_binary.bin");
        HANDLE hFile = CreateFileW(subFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hFile, "TEMP_BYTES", 10, &written, NULL);
            CloseHandle(hFile);
        }

        TEST_ASSERT(FileExists(subFile));
    }

    // After exiting scope, the temporary staging directory and all its files MUST be deleted
    TEST_ASSERT(!DirectoryExists(createdPath));
}

TEST_CASE(UpdaterTempCleanup, NoUserDataInStaging) {
    ScopedTempDirectory tempDir(L"Aperture_TestUpdate_");
    std::wstring dataDir = tempDir.SubPath(L"data");
    std::wstring settingsFile = tempDir.SubPath(L"data\\settings.json");
    std::wstring historyFile = tempDir.SubPath(L"data\\history.json");

    // Fresh temporary staging directory must not contain user data or history
    TEST_ASSERT(!FileExists(dataDir));
    TEST_ASSERT(!FileExists(settingsFile));
    TEST_ASSERT(!FileExists(historyFile));
}

} // namespace Aperture::Test
