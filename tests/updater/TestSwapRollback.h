#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Updater.h"
#include <windows.h>
#include <string>
#include <filesystem>

namespace Aperture::Test {

namespace fs = std::filesystem;

TEST_CASE(UpdaterSwapRollback, SuccessfulAtomicSwap) {
    ScopedTempDir targetDir;
    ScopedTempDir stagingDir;

    // 1. Setup target directory with original files and data folder
    targetDir.WriteFile(L"Aperture.exe", "ORIGINAL_APERTURE_BINARY_V1");
    targetDir.WriteFile(L"libmpv-2.dll", "ORIGINAL_LIBMPV_DLL_V1");
    targetDir.WriteFile(L"data\\settings.json", "{\"preserved\": true, \"customUser\": 123}");
    targetDir.WriteFile(L"data\\library.json", "{\"mediaCount\": 42}");

    // 2. Setup staging directory with new files
    stagingDir.WriteFile(L"Aperture.exe", "NEW_APERTURE_BINARY_V2");
    stagingDir.WriteFile(L"libmpv-2.dll", "NEW_LIBMPV_DLL_V2");

    std::wstring backupDir = targetDir.SubPath(L"_backup_update");
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::vector<std::pair<std::wstring, std::wstring>> swappedFiles;
    bool swapFailed = false;

    for (const auto& entry : fs::directory_iterator(fs::path(stagingDir.GetPath()))) {
        if (entry.is_regular_file()) {
            std::wstring fileName = entry.path().filename().wstring();
            std::wstring targetPath = targetDir.GetPath() + L"\\" + fileName;
            std::wstring backupPath = backupDir + L"\\" + fileName;
            std::wstring stagedPath = entry.path().wstring();

            if (FileExists(targetPath)) {
                if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    swapFailed = true;
                    break;
                }
            }

            if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                swapFailed = true;
                break;
            }

            swappedFiles.push_back({ targetPath, backupPath });
        }
    }

    TEST_ASSERT(!swapFailed);
    TEST_ASSERT_EQ(swappedFiles.size(), (size_t)2);

    // Verify target files were updated
    std::string newExe, newDll, dataSettings, dataLib;
    ReadTextFile(targetDir.SubPath(L"Aperture.exe"), newExe);
    ReadTextFile(targetDir.SubPath(L"libmpv-2.dll"), newDll);
    ReadTextFile(targetDir.SubPath(L"data\\settings.json"), dataSettings);
    ReadTextFile(targetDir.SubPath(L"data\\library.json"), dataLib);

    TEST_ASSERT_EQ(newExe, "NEW_APERTURE_BINARY_V2");
    TEST_ASSERT_EQ(newDll, "NEW_LIBMPV_DLL_V2");

    // Verify user data was preserved untouched
    TEST_ASSERT(dataSettings.find("customUser") != std::string::npos);
    TEST_ASSERT(dataLib.find("mediaCount") != std::string::npos);

    // Cleanup backup dir
    std::error_code ec;
    fs::remove_all(fs::path(backupDir), ec);
}

TEST_CASE(UpdaterSwapRollback, SimulatedFailureTriggersCleanRollback) {
    ScopedTempDir targetDir;
    ScopedTempDir stagingDir;

    // 1. Setup target directory with original files
    targetDir.WriteFile(L"Aperture.exe", "ORIGINAL_APERTURE_V1");
    targetDir.WriteFile(L"libmpv-2.dll", "ORIGINAL_LIBMPV_V1");
    targetDir.WriteFile(L"vulkan-1.dll", "ORIGINAL_VULKAN_V1");

    // 2. Setup staging directory with 3 new files
    stagingDir.WriteFile(L"Aperture.exe", "NEW_APERTURE_V2");
    stagingDir.WriteFile(L"libmpv-2.dll", "NEW_LIBMPV_V2");
    stagingDir.WriteFile(L"vulkan-1.dll", "NEW_VULKAN_V2");

    std::wstring backupDir = targetDir.SubPath(L"_backup_update");
    CreateDirectoryW(backupDir.c_str(), NULL);

    std::vector<std::pair<std::wstring, std::wstring>> swappedFiles;
    bool swapFailed = false;

    // Simulate swap failure on the 3rd file (vulkan-1.dll)
    int fileIndex = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(stagingDir.GetPath()))) {
        if (entry.is_regular_file()) {
            fileIndex++;
            std::wstring fileName = entry.path().filename().wstring();
            std::wstring targetPath = targetDir.GetPath() + L"\\" + fileName;
            std::wstring backupPath = backupDir + L"\\" + fileName;
            std::wstring stagedPath = entry.path().wstring();

            if (fileIndex == 3) {
                // Simulate failure on 3rd file move
                swapFailed = true;
                break;
            }

            if (FileExists(targetPath)) {
                if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    swapFailed = true;
                    break;
                }
            }

            if (!MoveFileExW(stagedPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                swapFailed = true;
                break;
            }

            swappedFiles.push_back({ targetPath, backupPath });
        }
    }

    TEST_ASSERT(swapFailed);
    TEST_ASSERT_EQ(swappedFiles.size(), (size_t)2);

    // Perform Rollback
    for (const auto& pair : swappedFiles) {
        if (FileExists(pair.second)) {
            MoveFileExW(pair.second.c_str(), pair.first.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    // Verify all original files were restored
    std::string exeContent, dllContent, vulkanContent;
    ReadTextFile(targetDir.SubPath(L"Aperture.exe"), exeContent);
    ReadTextFile(targetDir.SubPath(L"libmpv-2.dll"), dllContent);
    ReadTextFile(targetDir.SubPath(L"vulkan-1.dll"), vulkanContent);

    TEST_ASSERT_EQ(exeContent, "ORIGINAL_APERTURE_V1");
    TEST_ASSERT_EQ(dllContent, "ORIGINAL_LIBMPV_V1");
    TEST_ASSERT_EQ(vulkanContent, "ORIGINAL_VULKAN_V1");

    // Cleanup backup dir
    std::error_code ec;
    fs::remove_all(fs::path(backupDir), ec);
}

} // namespace Aperture::Test
