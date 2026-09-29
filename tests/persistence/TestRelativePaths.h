#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(RelativePaths, ToRelocatablePathBasic) {
    std::wstring baseDir = L"C:\\PortableApp";

    // Direct child
    std::wstring p1 = L"C:\\PortableApp\\movie.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p1, baseDir), L".\\movie.mp4");

    // Subdirectory child
    std::wstring p2 = L"C:\\PortableApp\\media\\movies\\action\\film.mkv";
    TEST_ASSERT_EQ(ToRelocatablePath(p2, baseDir), L".\\media\\movies\\action\\film.mkv");

    // Data directory child
    std::wstring p3 = L"C:\\PortableApp\\data\\library.json";
    TEST_ASSERT_EQ(ToRelocatablePath(p3, baseDir), L".\\data\\library.json");
}

TEST_CASE(RelativePaths, ToRelocatablePathCaseInsensitivity) {
    std::wstring baseDir = L"C:\\AperturePortable";

    // Lowercase path against mixed-case base
    std::wstring p1 = L"c:\\apertureportable\\media\\clip.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p1, baseDir), L".\\media\\clip.mp4");

    // Uppercase path against mixed-case base
    std::wstring p2 = L"C:\\APERTUREPORTABLE\\VIDEOS\\SAMPLE.MKV";
    TEST_ASSERT_EQ(ToRelocatablePath(p2, baseDir), L".\\VIDEOS\\SAMPLE.MKV");
}

TEST_CASE(RelativePaths, ToRelocatablePathForwardSlashes) {
    std::wstring baseDir = L"C:/ApertureApp/";

    std::wstring p1 = L"C:/ApertureApp/videos/sample.mp4";
    TEST_ASSERT_EQ(ToRelocatablePath(p1, baseDir), L".\\videos\\sample.mp4");
}

TEST_CASE(RelativePaths, ResolveAppPathRelativeVariations) {
    std::wstring baseDir = L"C:\\ApertureApp";

    // With leading relative dot-slash
    std::wstring p1 = L".\\videos\\movie.mp4";
    TEST_ASSERT_EQ(ResolveAppPath(p1, baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");

    // Without leading relative dot-slash
    std::wstring p2 = L"videos\\movie.mp4";
    TEST_ASSERT_EQ(ResolveAppPath(p2, baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");

    // With forward slashes
    std::wstring p3 = L"./videos/movie.mp4";
    TEST_ASSERT_EQ(ResolveAppPath(p3, baseDir), L"C:\\ApertureApp\\videos\\movie.mp4");
}

TEST_CASE(RelativePaths, FullRoundTripIdentity) {
    std::wstring baseDir = L"C:\\Tools\\Aperture";

    std::vector<std::wstring> samplePaths = {
        L"C:\\Tools\\Aperture\\video.mp4",
        L"C:\\Tools\\Aperture\\media\\sub1\\video.mkv",
        L"C:\\Tools\\Aperture\\data\\audio\\track.flac",
        L"c:\\tools\\aperture\\anime\\ep01.mp4"
    };

    for (const auto& original : samplePaths) {
        std::wstring relocatable = ToRelocatablePath(original, baseDir);
        TEST_ASSERT(relocatable.rfind(L".\\", 0) == 0);
        std::wstring resolved = ResolveAppPath(relocatable, baseDir);
        std::wstring normalizedExpected = ResolveAppPath(original, baseDir);
        TEST_ASSERT_EQ(_wcsicmp(resolved.c_str(), normalizedExpected.c_str()), 0);
    }
}

TEST_CASE(RelativePaths, MediaLibraryItemPathRoundTrip) {
    ScopedTempDir tempDir;
    std::wstring stagedBase = tempDir.GetPath();

    std::wstring mediaFile = tempDir.SubPath(L"media\\demo.mp4");
    tempDir.WriteFile(L"media\\demo.mp4", "fake video data");

    // Construct mock library JSON using relative path
    std::string libJson = "[\n"
        "  {\n"
        "    \"id\": \"test_rel_1\",\n"
        "    \"filePath\": \".\\\\media\\\\demo.mp4\",\n"
        "    \"title\": \"demo.mp4\",\n"
        "    \"durationSec\": 120.0,\n"
        "    \"status\": \"new\"\n"
        "  }\n"
        "]";

    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(libJson, parsed));
    TEST_ASSERT(parsed.is_array());
    TEST_ASSERT_EQ(parsed.as_array().size(), 1);

    // Resolve path using stagedBase
    std::wstring rawPath = parsed.as_array()[0].get("filePath").as_wstring();
    std::wstring resolvedPath = ResolveAppPath(rawPath, stagedBase);
    TEST_ASSERT_EQ(_wcsicmp(resolvedPath.c_str(), mediaFile.c_str()), 0);

    // Now serialize back using ToRelocatablePath
    std::wstring savedRelPath = ToRelocatablePath(resolvedPath, stagedBase);
    TEST_ASSERT_EQ(savedRelPath, L".\\media\\demo.mp4");
}

TEST_CASE(RelativePaths, PlaylistsAndHistoryRoundTrip) {
    ScopedTempDir tempDir;
    std::wstring stagedBase = tempDir.GetPath();

    std::wstring item1 = tempDir.SubPath(L"clips\\intro.mp4");
    std::wstring item2 = tempDir.SubPath(L"clips\\outro.mp4");

    // Playlists
    std::vector<std::wstring> originalItems = { item1, item2 };
    JsonValue playlistJson = JsonValue::Object();
    playlistJson["id"] = "pl_main";
    playlistJson["name"] = "Main Playlist";
    JsonValue itemsArr = JsonValue::Array();
    for (const auto& p : originalItems) {
        itemsArr.push_back(JsonValue(Utf16ToUtf8(ToRelocatablePath(p, stagedBase))));
    }
    playlistJson["itemPaths"] = itemsArr;

    // Verify serialized values are relative
    const auto& arr = playlistJson.get("itemPaths").as_array();
    TEST_ASSERT_EQ(arr.size(), 2);
    TEST_ASSERT_EQ(arr[0].as_string(), ".\\clips\\intro.mp4");
    TEST_ASSERT_EQ(arr[1].as_string(), ".\\clips\\outro.mp4");

    // Deserialize and resolve
    std::vector<std::wstring> loadedItems;
    for (const auto& v : arr) {
        loadedItems.push_back(ResolveAppPath(v.as_wstring(), stagedBase));
    }
    TEST_ASSERT_EQ(loadedItems.size(), 2);
    TEST_ASSERT_EQ(_wcsicmp(loadedItems[0].c_str(), item1.c_str()), 0);
    TEST_ASSERT_EQ(_wcsicmp(loadedItems[1].c_str(), item2.c_str()), 0);
}

} // namespace Aperture::Test
