#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MpvPlayer.h"
#include "../../src/Json.h"
#include <windows.h>
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(CustomMpvConfig, SecurityDenylistBoundsAndRecovery) {
    const std::wstring testExeDir = L"C:\\ApertureApp";
    TEST_ASSERT(Settings::IsRootContainedMpvConfigPath("mpv.conf", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("../outside.conf", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("mpv|evil.conf", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("../outside.conf", testExeDir), std::string("mpv.conf"));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("C:\\ApertureApp", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("C:\\ApertureApp", testExeDir), std::string("mpv.conf"));
    TEST_ASSERT(Settings::IsRootContainedMpvConfigPath("C:\\ApertureApp\\", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("C:\\ApertureApp\\", testExeDir), std::string(""));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("  c:/APERTUREAPP/config/../Custom.conf  ", testExeDir + L"\\"), std::string("Custom.conf"));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("C:\\ApertureApp-other\\mpv.conf", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath(std::string(32768, 'a'), testExeDir), std::string("mpv.conf"));

    const std::vector<std::string> unsafeOptions = {
        "script", "scripts", "script-opts", "lua", "js", "load-scripts",
        "ipc", "input-ipc-server", "include", "config", "log-file",
        "record-file", "stream-dump", "http-proxy", "ytdl", "wid", "dll",
        "playlist", "chapters-file", "ordered-chapters-files", "merge-files",
        "lavfi-complex", "bluray-device", "dvd-device", "cdda-device",
        "http-header-fields", "http-header-fields-append", "stream-lavf-o-append"
    };
    for (const auto& opt : unsafeOptions) {
        std::string reason;
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe(opt, "value", &reason));
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("--" + opt, "value", &reason));
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("no-" + opt, "value", &reason));
    }

    std::string reason;
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36`whoami`", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36$(calc)", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "../other/file", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("vf", "movie=secret.mkv", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("af", "amovie=secret.wav", &reason));
    TEST_ASSERT(MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36", &reason));

    std::string mixedConfig =
        "deband=yes\n"
        "script=C:\\evil\\hack.lua\n"
        "input-ipc-server=\\\\.\\pipe\\mpv_ipc\n"
        "scale=spline36\n";
    CustomMpvParseResult parsed = MpvPlayer::ParseCustomMpvConfig(mixedConfig);
    TEST_ASSERT(parsed.success);
    TEST_ASSERT_EQ(parsed.options.size(), size_t(2));
    TEST_ASSERT_EQ(parsed.options[0].name, std::string("deband"));
    TEST_ASSERT_EQ(parsed.options[1].name, std::string("scale"));
    TEST_ASSERT_EQ(parsed.warnings.size(), size_t(2));

    std::string largeContent(65 * 1024, 'A');
    TEST_ASSERT(!MpvPlayer::ParseCustomMpvConfig(largeContent).success);

    ScopedTempDir temp;
    std::wstring confPath = temp.SubPath(L"mpv.conf");
    TEST_ASSERT(Settings::ResetMpvConfigToDefault(confPath).is_ok());
    std::string goodContent = "deband=yes\nscale=spline36\n";
    TEST_ASSERT(Settings::SaveMpvConfigLastKnownGood(confPath, goodContent).is_ok());
    TEST_ASSERT(AtomicWriteTextFile(confPath, "bad_broken_syntax\n").is_ok());
    TEST_ASSERT(Settings::RevertMpvConfigToLastKnownGood(confPath).is_ok());
    std::string restoredContent;
    TEST_ASSERT(ReadTextFile(confPath, restoredContent).is_ok());
    TEST_ASSERT_EQ(restoredContent, goodContent);

    MpvPlayer player;
    player.SetCustomMpvConfigEnabled(true);
    player.SetCustomMpvConfigPath("mpv.conf");
    CustomMpvApplyResult uninitRes = player.ApplyCustomMpvOptions(parsed.options);
    TEST_ASSERT(!uninitRes.success);
    TEST_ASSERT(uninitRes.errorMessage.find("not initialized") != std::string::npos);
}

} // namespace Aperture::Test
