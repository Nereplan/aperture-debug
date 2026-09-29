#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(AbsoluteProfile, ExternalDrivesRemainAbsolute) {
    std::wstring baseDir = L"C:\\PortableApp";

    std::wstring p1 = L"D:\\ExternalDrive\\Movies\\SciFi\\Dune.mkv";
    TEST_ASSERT_EQ(ToRelocatablePath(p1, baseDir), p1);

    std::wstring p2 = L"E:\\Backups\\Music\\Concert.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p2, baseDir), p2);

    std::wstring p3 = L"Z:\\NetworkDrive\\Videos\\series.mkv";
    TEST_ASSERT_EQ(ToRelocatablePath(p3, baseDir), p3);
}

TEST_CASE(AbsoluteProfile, SameDriveOutsideBaseRemainsAbsolute) {
    std::wstring baseDir = L"C:\\PortableApp";

    std::wstring p1 = L"C:\\Users\\ExampleUser\\Videos\\movie.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p1, baseDir), p1);

    std::wstring p2 = L"C:\\Downloads\\trailer.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p2, baseDir), p2);

    // Sibling directory with similar prefix
    std::wstring p3 = L"C:\\PortableAppExtra\\extra.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p3, baseDir), p3);
}

TEST_CASE(AbsoluteProfile, UNCSharesRemainAbsolute) {
    std::wstring baseDir = L"C:\\PortableApp";

    std::wstring uncPath = L"\\\\nas\\movies\\4k\\sample.mkv";
    TEST_ASSERT_EQ(ToRelocatablePath(uncPath, baseDir), uncPath);
    TEST_ASSERT_EQ(ResolveAppPath(uncPath, baseDir), uncPath);
}

TEST_CASE(AbsoluteProfile, MixedRelativeAndAbsoluteItems) {
    ScopedTempDir tempDir;
    std::wstring stagedBase = tempDir.GetPath();

    std::wstring localItem = tempDir.SubPath(L"media\\local.mp4");
    std::wstring extItem1 = L"D:\\External\\movie.mp4";
    std::wstring extItem2 = L"C:\\Users\\Public\\Videos\\sample.mp4";

    std::vector<std::wstring> items = { localItem, extItem1, extItem2 };

    JsonValue serializedArr = JsonValue::Array();
    for (const auto& itemPath : items) {
        serializedArr.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(itemPath, stagedBase))));
    }

    // Verify local is relative, external are absolute
    TEST_ASSERT_EQ(serializedArr[0].as_string(), ".\\media\\local.mp4");
    TEST_ASSERT_EQ(serializedArr[1].as_string(), "D:\\External\\movie.mp4");
    TEST_ASSERT_EQ(serializedArr[2].as_string(), "C:\\Users\\Public\\Videos\\sample.mp4");

    // Deserialize and resolve
    std::vector<std::wstring> resolvedItems;
    for (const auto& v : serializedArr.as_array()) {
        resolvedItems.push_back(ResolveAppPath(v.as_wstring(), stagedBase));
    }

    TEST_ASSERT_EQ(_wcsicmp(resolvedItems[0].c_str(), localItem.c_str()), 0);
    TEST_ASSERT_EQ(_wcsicmp(resolvedItems[1].c_str(), extItem1.c_str()), 0);
    TEST_ASSERT_EQ(_wcsicmp(resolvedItems[2].c_str(), extItem2.c_str()), 0);
}

TEST_CASE(AbsoluteProfile, LoadAbsolutePathsFixture) {
    std::string fixtureContent;
    std::wstring fixturePath = GetExeDir() + L"\\tests\\fixtures\\library_absolute_paths.json";
    // If running from build dir or root, locate fixture
    if (!FileExists(fixturePath)) {
        fixturePath = L"tests\\fixtures\\library_absolute_paths.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\tests\\fixtures\\library_absolute_paths.json";
    }
    if (!FileExists(fixturePath)) {
        fixturePath = L"..\\..\\tests\\fixtures\\library_absolute_paths.json";
    }

    TEST_ASSERT(ReadTextFile(fixturePath, fixtureContent).is_ok());

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(fixtureContent, root));
    TEST_ASSERT(root.is_array());
    TEST_ASSERT_EQ(root.as_array().size(), 3);

    std::wstring baseDir = L"C:\\ApertureApp";
    for (const auto& item : root.as_array()) {
        std::wstring rawPath = item.get("filePath").as_wstring();
        std::wstring resolved = ResolveAppPath(rawPath, baseDir);
        std::wstring relocatable = ToRelocatablePath(resolved, baseDir);

        // Should remain exactly the original absolute path
        TEST_ASSERT_EQ(relocatable, rawPath);
    }
}

} // namespace Aperture::Test
