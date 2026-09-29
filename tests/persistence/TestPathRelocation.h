#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"

namespace Aperture::Test {

TEST_CASE(PathRelocation, TreeRelocationSimulation) {
    // Stage A: Origin installation
    ScopedTempDir stageA(L"Aperture_StageA_");
    std::wstring rootA = stageA.GetPath();

    stageA.WriteFile(L"videos\\intro.mp4", "mock intro video stream");
    stageA.WriteFile(L"videos\\outro.mkv", "mock outro video stream");

    // Write library in Stage A with relative paths
    std::string libJson = "[\n"
        "  {\n"
        "    \"id\": \"item_intro\",\n"
        "    \"filePath\": \".\\\\videos\\\\intro.mp4\",\n"
        "    \"title\": \"Intro Video\",\n"
        "    \"durationSec\": 15.0,\n"
        "    \"status\": \"done\"\n"
        "  },\n"
        "  {\n"
        "    \"id\": \"item_outro\",\n"
        "    \"filePath\": \".\\\\videos\\\\outro.mkv\",\n"
        "    \"title\": \"Outro Video\",\n"
        "    \"durationSec\": 30.0,\n"
        "    \"status\": \"new\"\n"
        "  },\n"
        "  {\n"
        "    \"id\": \"item_external\",\n"
        "    \"filePath\": \"D:\\\\ExternalDrive\\\\movie.mp4\",\n"
        "    \"title\": \"External Movie\",\n"
        "    \"durationSec\": 7200.0,\n"
        "    \"status\": \"new\"\n"
        "  }\n"
        "]";
    stageA.WriteFile(L"data\\library.json", libJson);

    // Write playlists in Stage A
    std::string plJson = "[\n"
        "  {\n"
        "    \"id\": \"pl_intro\",\n"
        "    \"name\": \"Intro Playlist\",\n"
        "    \"itemPaths\": [\".\\\\videos\\\\intro.mp4\"]\n"
        "  }\n"
        "]";
    stageA.WriteFile(L"data\\playlists.json", plJson);

    // Write history in Stage A
    std::string histJson = "[\n"
        "  {\n"
        "    \"filePath\": \".\\\\videos\\\\intro.mp4\",\n"
        "    \"title\": \"Intro Video\",\n"
        "    \"lastPosSec\": 15.0,\n"
        "    \"durationSec\": 15.0,\n"
        "    \"watchedTimestamp\": 1788001000\n"
        "  }\n"
        "]";
    stageA.WriteFile(L"data\\history.json", histJson);

    // Stage B: Relocated installation (e.g. copied to another folder or drive)
    ScopedTempDir stageB(L"Aperture_StageB_");
    std::wstring rootB = stageB.GetPath();

    // Copy entire tree from Stage A to Stage B
    TEST_ASSERT(stageA.CopyTo(rootB));

    // Verify files copied to Stage B
    TEST_ASSERT(FileExists(stageB.SubPath(L"videos\\intro.mp4")));
    TEST_ASSERT(FileExists(stageB.SubPath(L"videos\\outro.mkv")));
    TEST_ASSERT(FileExists(stageB.SubPath(L"data\\library.json")));

    // Read library.json from Stage B and resolve paths using rootB as base
    std::string libContentB;
    TEST_ASSERT(ReadTextFile(stageB.SubPath(L"data\\library.json"), libContentB).is_ok());

    JsonValue parsedLibB;
    TEST_ASSERT(JsonValue::parse(libContentB, parsedLibB));
    TEST_ASSERT(parsedLibB.is_array());
    TEST_ASSERT_EQ(parsedLibB.as_array().size(), 3);

    // Verify item 1 resolved to rootB
    std::wstring resolvedIntro = ResolveAppPath(parsedLibB[0].get("filePath").as_wstring(), rootB);
    std::wstring expectedIntro = stageB.SubPath(L"videos\\intro.mp4");
    TEST_ASSERT_EQ(_wcsicmp(resolvedIntro.c_str(), expectedIntro.c_str()), 0);
    TEST_ASSERT(FileExists(resolvedIntro));

    // Verify item 2 resolved to rootB
    std::wstring resolvedOutro = ResolveAppPath(parsedLibB[1].get("filePath").as_wstring(), rootB);
    std::wstring expectedOutro = stageB.SubPath(L"videos\\outro.mkv");
    TEST_ASSERT_EQ(_wcsicmp(resolvedOutro.c_str(), expectedOutro.c_str()), 0);
    TEST_ASSERT(FileExists(resolvedOutro));

    // Verify item 3 (external) remained external
    std::wstring resolvedExt = ResolveAppPath(parsedLibB[2].get("filePath").as_wstring(), rootB);
    TEST_ASSERT_EQ(resolvedExt, L"D:\\ExternalDrive\\movie.mp4");

    // Add a new item in Stage B and re-serialize
    stageB.WriteFile(L"videos\\bonus.mp4", "mock bonus content");
    std::wstring bonusPath = stageB.SubPath(L"videos\\bonus.mp4");

    JsonValue newLibArray = parsedLibB;
    JsonValue bonusItem = JsonValue::Object();
    bonusItem["id"] = "item_bonus";
    bonusItem["filePath"] = Utf16ToUtf8(ToRelocatablePath(bonusPath, rootB));
    bonusItem["title"] = "bonus.mp4";
    bonusItem["durationSec"] = 60.0;
    newLibArray.push_back(bonusItem);

    std::string savedLibB = newLibArray.stringify(false);
    TEST_ASSERT(AtomicWriteTextFile(stageB.SubPath(L"data\\library.json"), savedLibB).is_ok());

    // Read back and verify bonus.mp4 is saved as relative path
    JsonValue reloadedLibB;
    std::string reloadedContentB;
    TEST_ASSERT(ReadTextFile(stageB.SubPath(L"data\\library.json"), reloadedContentB).is_ok());
    TEST_ASSERT(JsonValue::parse(reloadedContentB, reloadedLibB));
    TEST_ASSERT_EQ(reloadedLibB.as_array().size(), 4);
    TEST_ASSERT_EQ(reloadedLibB[3].get("filePath").as_string(), ".\\videos\\bonus.mp4");
}

} // namespace Aperture::Test
