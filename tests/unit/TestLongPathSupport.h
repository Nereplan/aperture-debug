#pragma once

#include "../support/TestFramework.h"
#include "../../src/IpcProtocol.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>

namespace Aperture::Test {

TEST_CASE(LongPathSupport, ResolveFullPathLongBasics) {
    // 1. Empty path returns empty
    TEST_ASSERT(Ipc::ResolveFullPathLong(L"").empty());

    // 2. Standard path resolves without failure
    std::wstring currentPath = L".";
    std::wstring resolvedCurrent = Ipc::ResolveFullPathLong(currentPath);
    TEST_ASSERT(!resolvedCurrent.empty());
    TEST_ASSERT(resolvedCurrent.length() >= 2);

    // 3. Path with exceeding length does not crash or truncate
    std::wstring longSegment(300, L'a');
    std::wstring fakeLong = L"C:\\" + longSegment + L"\\test.mp4";
    TEST_ASSERT(fakeLong.length() > MAX_PATH);
    std::wstring resolvedLong = Ipc::ResolveFullPathLong(fakeLong);
    TEST_ASSERT(!resolvedLong.empty());
    TEST_ASSERT(resolvedLong.length() > MAX_PATH);
}

TEST_CASE(LongPathSupport, MultiSelectBufferParsingSingleFile) {
    // Simulate GetOpenFileNameW single file result in dynamically allocated buffer
    std::wstring singlePath = L"C:\\MediaFolder\\ApertureLongPathVideoFile.mp4";
    std::vector<wchar_t> buffer(256 * 1024, L'\0');
    std::copy(singlePath.begin(), singlePath.end(), buffer.begin());
    buffer[singlePath.length()] = L'\0';
    buffer[singlePath.length() + 1] = L'\0';

    const wchar_t* base = buffer.data();
    std::wstring dir = base;
    const wchar_t* p = base + dir.length() + 1;
    std::vector<std::wstring> selectedFiles;

    if (*p == L'\0') {
        selectedFiles.push_back(Ipc::ResolveFullPathLong(dir));
    } else {
        while (*p != L'\0') {
            std::wstring fullPath = dir;
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
                fullPath += L'\\';
            }
            fullPath += p;
            selectedFiles.push_back(Ipc::ResolveFullPathLong(fullPath));
            p += wcslen(p) + 1;
        }
    }

    TEST_ASSERT_EQ(selectedFiles.size(), 1ull);
    TEST_ASSERT(!selectedFiles[0].empty());
}

TEST_CASE(LongPathSupport, MultiSelectBufferParsingMultipleFiles) {
    // Simulate GetOpenFileNameW multi-select result in dynamically allocated buffer
    std::wstring dir = L"C:\\Media\\DeepNestedDirectoryStructureThatMayBeQuiteLong";
    std::wstring file1 = L"episode_01_very_long_title_with_extra_metadata_description.mp4";
    std::wstring file2 = L"episode_02_very_long_title_with_extra_metadata_description.mp4";
    std::wstring file3 = L"episode_03_very_long_title_with_extra_metadata_description.mp4";

    std::vector<wchar_t> buffer(256 * 1024, L'\0');
    size_t offset = 0;
    auto appendStr = [&](const std::wstring& s) {
        std::copy(s.begin(), s.end(), buffer.begin() + offset);
        offset += s.length();
        buffer[offset++] = L'\0';
    };

    appendStr(dir);
    appendStr(file1);
    appendStr(file2);
    appendStr(file3);
    buffer[offset++] = L'\0'; // Double-null terminator

    const wchar_t* base = buffer.data();
    std::wstring parsedDir = base;
    const wchar_t* p = base + parsedDir.length() + 1;
    std::vector<std::wstring> selectedFiles;

    if (*p == L'\0') {
        selectedFiles.push_back(Ipc::ResolveFullPathLong(parsedDir));
    } else {
        while (*p != L'\0') {
            std::wstring fullPath = parsedDir;
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
                fullPath += L'\\';
            }
            fullPath += p;
            selectedFiles.push_back(Ipc::ResolveFullPathLong(fullPath));
            p += wcslen(p) + 1;
        }
    }

    TEST_ASSERT_EQ(selectedFiles.size(), 3ull);
    for (const auto& sf : selectedFiles) {
        TEST_ASSERT(!sf.empty());
    }
}

TEST_CASE(LongPathSupport, DynamicModuleFileNameResolution) {
    // Test the dynamic GetModuleFileNameW algorithm used in RegisterFileAssociations
    std::wstring exePath;
    DWORD bufferSize = 1024;
    while (bufferSize <= Ipc::MAX_IPC_PATH_CHARS) {
        std::vector<wchar_t> buffer(bufferSize, L'\0');
        DWORD len = GetModuleFileNameW(NULL, buffer.data(), bufferSize);
        if (len == 0) break;
        if (len < bufferSize) {
            exePath.assign(buffer.data(), len);
            break;
        }
        if (bufferSize >= Ipc::MAX_IPC_PATH_CHARS) break;
        bufferSize = (std::min)(bufferSize * 2, static_cast<DWORD>(Ipc::MAX_IPC_PATH_CHARS));
    }

    TEST_ASSERT(!exePath.empty());
    TEST_ASSERT(exePath.find(L"Aperture_Unit_Tests.exe") != std::wstring::npos ||
                exePath.find(L"Aperture") != std::wstring::npos);
}

} // namespace Aperture::Test
