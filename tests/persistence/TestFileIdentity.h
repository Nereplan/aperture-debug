#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"
#include <charconv>

namespace Aperture::Test {

TEST_CASE(FileIdentity, WindowsFileIdentityQuery) {
    ScopedTempDir tempDir(L"Aperture_FileId_");
    std::wstring filePath = tempDir.SubPath(L"sample_video.mp4");
    tempDir.WriteFile(L"sample_video.mp4", "file identity test stream");

    WindowsFileIdentity id = GetWindowsFileIdentity(filePath);
    TEST_ASSERT(id.valid);
    TEST_ASSERT(id.fileIndex != 0 || id.volumeSerialNumber != 0);
}

TEST_CASE(FileIdentity, ExactIdentityStringRoundTrip) {
    constexpr uint64_t highIdentity = UINT64_C(0xfedcba9876543210);

    JsonValue j = JsonValue::Object();
    j["fileIndex"] = std::to_string(highIdentity);
    j["volumeSerialNumber"] = UINT64_C(0xffffffff);

    std::string jsonStr = j.stringify(false);
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(jsonStr, parsed));

    uint64_t parsedIdentity = 0;
    const std::string identityText = parsed.get("fileIndex").as_string();
    const auto result = std::from_chars(
        identityText.data(), identityText.data() + identityText.size(), parsedIdentity, 10);
    TEST_ASSERT(result.ec == std::errc());
    TEST_ASSERT(result.ptr == identityText.data() + identityText.size());
    TEST_ASSERT_EQ(parsedIdentity, highIdentity);
    TEST_ASSERT_EQ(parsed.get("volumeSerialNumber").as_uint64(), UINT64_C(0xffffffff));
}

TEST_CASE(FileIdentity, LegacyNumericIdentityCompatibility) {
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse("{\"fileIndex\":9007199254740992}", parsed));
    TEST_ASSERT_EQ(parsed.get("fileIndex").as_uint64(), UINT64_C(9007199254740992));
}

TEST_CASE(FileIdentity, MalformedIdentityStringIsNotNumeric) {
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse("{\"fileIndex\":\"18446744073709551616\"}", parsed));
    TEST_ASSERT(parsed.get("fileIndex").is_string());
    TEST_ASSERT_EQ(parsed.get("fileIndex").as_uint64(0), UINT64_C(0));
}

TEST_CASE(FileIdentity, RelocatableInRootWithExternalAbsolute) {
    ScopedTempDir rootDir(L"Aperture_Root_");
    std::wstring root = rootDir.GetPath();

    std::wstring inRootMedia = rootDir.SubPath(L"media\\local_video.mp4");
    std::wstring externalMedia = L"E:\\ExternalLibrary\\movie.mp4";

    std::wstring relocatable = ToRelocatablePath(inRootMedia, root);
    std::wstring extRelocatable = ToRelocatablePath(externalMedia, root);

    // In-root media becomes relative
    TEST_ASSERT_EQ(relocatable, L".\\media\\local_video.mp4");

    // External media remains absolute
    TEST_ASSERT_EQ(extRelocatable, L"E:\\ExternalLibrary\\movie.mp4");

    // Resolving restores exact full path
    TEST_ASSERT_EQ(ResolveAppPath(relocatable, root), inRootMedia);
    TEST_ASSERT_EQ(ResolveAppPath(extRelocatable, root), externalMedia);
}

} // namespace Aperture::Test
