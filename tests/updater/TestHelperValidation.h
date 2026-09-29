#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include <windows.h>
#include <vector>

namespace Aperture::Test {

TEST_CASE(UpdaterHelperValidation, InvalidCommandLineArguments) {
    // Empty args
    wchar_t* args1[] = { const_cast<wchar_t*>(L"Aperture.exe"), const_cast<wchar_t*>(L"--update-helper") };
    int res1 = RunHelperMode(2, args1);
    TEST_ASSERT_EQ(res1, 101);

    // Missing staging-dir
    wchar_t* args2[] = {
        const_cast<wchar_t*>(L"Aperture.exe"),
        const_cast<wchar_t*>(L"--update-helper"),
        const_cast<wchar_t*>(L"--parent-pid"),
        const_cast<wchar_t*>(L"1234"),
        const_cast<wchar_t*>(L"--target-dir"),
        const_cast<wchar_t*>(L"C:\\Aperture")
    };
    int res2 = RunHelperMode(6, args2);
    TEST_ASSERT_EQ(res2, 101);
}

TEST_CASE(UpdaterHelperValidation, NonExistentParentPidRejected) {
    ScopedTempDir tempDir;
    std::wstring staging = tempDir.SubPath(L"staged");
    CreateDirectoryW(staging.c_str(), NULL);

    // PID 99999999 is invalid/non-existent
    wchar_t* args[] = {
        const_cast<wchar_t*>(L"Aperture.exe"),
        const_cast<wchar_t*>(L"--update-helper"),
        const_cast<wchar_t*>(L"--parent-pid"),
        const_cast<wchar_t*>(L"99999999"),
        const_cast<wchar_t*>(L"--target-dir"),
        const_cast<wchar_t*>(tempDir.GetPath().c_str()),
        const_cast<wchar_t*>(L"--staging-dir"),
        const_cast<wchar_t*>(staging.c_str())
    };

    int res = RunHelperMode(8, args);
    TEST_ASSERT_EQ(res, 102); // Cannot open parent process
}

} // namespace Aperture::Test
