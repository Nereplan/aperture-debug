#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Json.h"
#include "../../src/MediaLibrary.h"
#include "../../src/MpvPlayer.h"
#include "../../src/Settings.h"
#include "../../src/CommandDispatcher.h"
#include <cmath>
#include <limits>

namespace Aperture::Test {

TEST_CASE(VideoAdjustments, RotationValidation) {
    // Valid 90-degree steps
    TEST_ASSERT(MediaPlaybackPreferences::IsValidVideoRotate(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidVideoRotate(90));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidVideoRotate(180));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidVideoRotate(270));

    // Invalid angles
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidVideoRotate(45));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidVideoRotate(-90));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidVideoRotate(360));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidVideoRotate(100));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidVideoRotate(-1));
}

TEST_CASE(VideoAdjustments, ImageAdjustmentValidation) {
    // Brightness: [-100, 100]
    TEST_ASSERT(MediaPlaybackPreferences::IsValidBrightness(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidBrightness(-100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidBrightness(100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidBrightness(-50));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidBrightness(50));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidBrightness(-101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidBrightness(101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidBrightness(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidBrightness(std::numeric_limits<double>::infinity()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidBrightness(-std::numeric_limits<double>::infinity()));

    // Contrast: [-100, 100]
    TEST_ASSERT(MediaPlaybackPreferences::IsValidContrast(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidContrast(-100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidContrast(100));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidContrast(-101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidContrast(101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidContrast(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidContrast(std::numeric_limits<double>::infinity()));

    // Saturation: [-100, 100]
    TEST_ASSERT(MediaPlaybackPreferences::IsValidSaturation(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidSaturation(-100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidSaturation(100));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSaturation(-101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSaturation(101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSaturation(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidSaturation(std::numeric_limits<double>::infinity()));

    // Gamma: [-100, 100]
    TEST_ASSERT(MediaPlaybackPreferences::IsValidGamma(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidGamma(-100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidGamma(100));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidGamma(-101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidGamma(101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidGamma(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidGamma(std::numeric_limits<double>::infinity()));

    // Hue: [-100, 100]
    TEST_ASSERT(MediaPlaybackPreferences::IsValidHue(0));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidHue(-100));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidHue(100));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidHue(-101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidHue(101));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidHue(std::numeric_limits<double>::quiet_NaN()));
    TEST_ASSERT(!MediaPlaybackPreferences::IsValidHue(std::numeric_limits<double>::infinity()));
}

TEST_CASE(VideoAdjustments, PreferenceHasAny) {
    MediaPlaybackPreferences prefs;
    TEST_ASSERT(!prefs.HasAnyPreference());

    prefs.videoRotate = 90;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.videoRotate.reset();
    TEST_ASSERT(!prefs.HasAnyPreference());

    prefs.brightness = 10;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.brightness.reset();

    prefs.contrast = -15;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.contrast.reset();

    prefs.saturation = 20;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.saturation.reset();

    prefs.gamma = -5;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.gamma.reset();

    prefs.hue = 30;
    TEST_ASSERT(prefs.HasAnyPreference());
    prefs.hue.reset();
    TEST_ASSERT(!prefs.HasAnyPreference());
}

TEST_CASE(VideoAdjustments, SerializationRoundTripAndUnknownFields) {
    ScopedTempDir tempDir(L"Aperture_VideoAdj_");
    const std::wstring videoPath = tempDir.SubPath(L"sample.mp4");
    tempDir.WriteFile(L"sample.mp4", "dummy video stream data");

    // 1. Save preferences with rotation, image adjustments, and unknown extra fields
    {
        MediaLibrary lib;
        MediaItem* item = lib.AddOrUpdateMedia(videoPath);
        TEST_ASSERT(item != nullptr);

        MediaPlaybackPreferences prefs;
        prefs.videoRotate = 180;
        prefs.brightness = 25;
        prefs.contrast = -30;
        prefs.saturation = 40;
        prefs.gamma = -10;
        prefs.hue = 15;

        JsonValue extra = JsonValue::Object();
        extra["customPluginSetting"] = "customValue123";
        prefs.extraFields = extra;

        TEST_ASSERT(lib.SetMediaPreferences(videoPath, prefs));

        MediaLibrarySaveResult saveRes = lib.Save();
        TEST_ASSERT(saveRes.is_ok());
    }

    // 2. Load and verify deserialization and preservation
    {
        MediaLibrary loadedLib;
        StorageStatus loadStatus = loadedLib.Load();
        TEST_ASSERT(loadStatus.is_ok());
        const MediaItem* item = loadedLib.FindByPath(videoPath);
        TEST_ASSERT(item != nullptr);
        TEST_ASSERT(item->preferences.has_value());

        const auto& p = *item->preferences;
        TEST_ASSERT(p.videoRotate.has_value() && *p.videoRotate == 180);
        TEST_ASSERT(p.brightness.has_value() && *p.brightness == 25);
        TEST_ASSERT(p.contrast.has_value() && *p.contrast == -30);
        TEST_ASSERT(p.saturation.has_value() && *p.saturation == 40);
        TEST_ASSERT(p.gamma.has_value() && *p.gamma == -10);
        TEST_ASSERT(p.hue.has_value() && *p.hue == 15);

        TEST_ASSERT(p.extraFields.has_key("customPluginSetting"));
        TEST_ASSERT_EQ(p.extraFields.get("customPluginSetting").as_string(), std::string("customValue123"));
    }
}

TEST_CASE(VideoAdjustments, JsonValidationAndRejection) {
    // Test parsing JSON with valid, invalid, and out-of-bounds fields directly
    JsonValue prefJson = JsonValue::Object();
    prefJson["videoRotate"] = 45; // invalid (not multiple of 90)
    prefJson["brightness"] = 250; // out of range
    prefJson["contrast"] = -300; // out of range
    prefJson["saturation"] = 50;  // valid
    prefJson["gamma"] = 999;     // out of range
    prefJson["hue"] = -200;      // out of range

    MediaPlaybackPreferences prefs;
    if (prefJson.has_key("videoRotate") && prefJson.get("videoRotate").is_number()) {
        int r = prefJson.get("videoRotate").as_int();
        if (MediaPlaybackPreferences::IsValidVideoRotate(r)) {
            prefs.videoRotate = r;
        }
    }
    if (prefJson.has_key("brightness") && prefJson.get("brightness").is_number()) {
        double val = prefJson.get("brightness").as_number();
        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidBrightness((int)std::round(val))) {
            prefs.brightness = (int)std::round(val);
        }
    }
    if (prefJson.has_key("contrast") && prefJson.get("contrast").is_number()) {
        double val = prefJson.get("contrast").as_number();
        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidContrast((int)std::round(val))) {
            prefs.contrast = (int)std::round(val);
        }
    }
    if (prefJson.has_key("saturation") && prefJson.get("saturation").is_number()) {
        double val = prefJson.get("saturation").as_number();
        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidSaturation((int)std::round(val))) {
            prefs.saturation = (int)std::round(val);
        }
    }
    if (prefJson.has_key("gamma") && prefJson.get("gamma").is_number()) {
        double val = prefJson.get("gamma").as_number();
        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidGamma((int)std::round(val))) {
            prefs.gamma = (int)std::round(val);
        }
    }
    if (prefJson.has_key("hue") && prefJson.get("hue").is_number()) {
        double val = prefJson.get("hue").as_number();
        if (std::isfinite(val) && MediaPlaybackPreferences::IsValidHue((int)std::round(val))) {
            prefs.hue = (int)std::round(val);
        }
    }

    TEST_ASSERT(!prefs.videoRotate.has_value());
    TEST_ASSERT(!prefs.brightness.has_value());
    TEST_ASSERT(!prefs.contrast.has_value());
    TEST_ASSERT(prefs.saturation.has_value() && *prefs.saturation == 50);
    TEST_ASSERT(!prefs.gamma.has_value());
    TEST_ASSERT(!prefs.hue.has_value());
}

TEST_CASE(VideoAdjustments, MpvPlayerClampingAndNormalization) {
    MpvPlayer player;

    // Rotation normalization to 0, 90, 180, 270
    player.SetVideoRotate(90);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 90);
    player.SetVideoRotate(180);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 180);
    player.SetVideoRotate(270);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 270);
    player.SetVideoRotate(360);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 0);
    player.SetVideoRotate(450);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 90);
    player.SetVideoRotate(-90);
    TEST_ASSERT_EQ(player.GetVideoRotate(), 270);
    player.SetVideoRotate(45); // Non-multiple -> 0
    TEST_ASSERT_EQ(player.GetVideoRotate(), 0);

    // Image adjustments clamping
    player.SetBrightness(50);
    TEST_ASSERT_EQ(player.GetBrightness(), 50);
    player.SetBrightness(150);
    TEST_ASSERT_EQ(player.GetBrightness(), 100);
    player.SetBrightness(-200);
    TEST_ASSERT_EQ(player.GetBrightness(), -100);

    player.SetContrast(30);
    TEST_ASSERT_EQ(player.GetContrast(), 30);
    player.SetContrast(120);
    TEST_ASSERT_EQ(player.GetContrast(), 100);
    player.SetContrast(-120);
    TEST_ASSERT_EQ(player.GetContrast(), -100);

    player.SetSaturation(-40);
    TEST_ASSERT_EQ(player.GetSaturation(), -40);
    player.SetSaturation(200);
    TEST_ASSERT_EQ(player.GetSaturation(), 100);
    player.SetSaturation(-200);
    TEST_ASSERT_EQ(player.GetSaturation(), -100);

    player.SetGamma(15);
    TEST_ASSERT_EQ(player.GetGamma(), 15);
    player.SetGamma(110);
    TEST_ASSERT_EQ(player.GetGamma(), 100);
    player.SetGamma(-110);
    TEST_ASSERT_EQ(player.GetGamma(), -100);

    player.SetHue(-25);
    TEST_ASSERT_EQ(player.GetHue(), -25);
    player.SetHue(150);
    TEST_ASSERT_EQ(player.GetHue(), 100);
    player.SetHue(-150);
    TEST_ASSERT_EQ(player.GetHue(), -100);

    // Reset
    player.ResetVideoAdjustments();
    TEST_ASSERT_EQ(player.GetVideoRotate(), 0);
    TEST_ASSERT_EQ(player.GetBrightness(), 0);
    TEST_ASSERT_EQ(player.GetContrast(), 0);
    TEST_ASSERT_EQ(player.GetSaturation(), 0);
    TEST_ASSERT_EQ(player.GetGamma(), 0);
    TEST_ASSERT_EQ(player.GetHue(), 0);
}

class AdjustmentMockContext : public ICommandContext {
public:
    Settings settings;
    MpvPlayer player;
    PlaybackSession session;
    MediaLibrary library;

    bool osdShown = false;
    IconType lastOsdIcon = IconType::VolumeHigh;
    std::wstring lastOsdText;
    bool settingsSaved = false;
    bool librarySaved = false;

    Settings& GetSettings() override { return settings; }
    MpvPlayer& GetPlayer() override { return player; }
    PlaybackSession& GetPlaybackSession() override { return session; }
    MediaLibrary& GetLibrary() override { return library; }

    void ShowOSD(IconType icon, const std::wstring& text, float) override {
        osdShown = true;
        lastOsdIcon = icon;
        lastOsdText = text;
    }
    void TriggerDoubleTapFeedback(bool, int) override {}
    void TriggerPlayPauseBadge(bool) override {}

    bool SaveSettings() override { settingsSaved = true; return true; }
    bool SaveLibrary() override { librarySaved = true; return true; }

    void OpenAndPlayMedia(const std::wstring&, double, bool) override {}
    void PlayFirstInQueue() override {}
    void PlayNextInQueue() override {}
    void PlayPrevInQueue() override {}
    void PlayNextLibraryMedia() override {}

    void ToggleFullscreen() override {}
    void TogglePiP() override {}
    void ClosePlayer() override {}

    void OpenFileDialog() override {}
    void OpenSubtitleFileDialog() override {}
    void OpenInExplorer(const std::wstring&) override {}
    bool TakeScreenshot() override { return true; }

    void DismissPopups() override {}
    bool ArePopupsOpen() const override { return false; }
    void RequestRender() override {}
    void TogglePlaybackDiagnostics() override {}
    bool IsPlaybackDiagnosticsVisible() const override { return false; }
};

TEST_CASE(VideoAdjustments, EveryImageCommandPreservesMessagesAndPrivateIsolation) {
    struct Case {
        AppCommand command;
        int input;
        int expected;
        int Settings::*setting;
        std::optional<int> MediaPlaybackPreferences::*preference;
        const wchar_t* message;
    };
    const Case cases[] = {
        {AppCommand::BrightnessPlus, 0, 5, &Settings::brightness, &MediaPlaybackPreferences::brightness, L"Brightness: +5"},
        {AppCommand::BrightnessMinus, 12, -12, &Settings::brightness, &MediaPlaybackPreferences::brightness, L"Brightness: -12"},
        {AppCommand::SetBrightness, 200, 100, &Settings::brightness, &MediaPlaybackPreferences::brightness, L"Brightness: +100"},
        {AppCommand::BrightnessReset, 90, 0, &Settings::brightness, &MediaPlaybackPreferences::brightness, L"Brightness: 0"},
        {AppCommand::ContrastPlus, 0, 5, &Settings::contrast, &MediaPlaybackPreferences::contrast, L"Contrast: +5"},
        {AppCommand::ContrastMinus, 12, -12, &Settings::contrast, &MediaPlaybackPreferences::contrast, L"Contrast: -12"},
        {AppCommand::SetContrast, -200, -100, &Settings::contrast, &MediaPlaybackPreferences::contrast, L"Contrast: -100"},
        {AppCommand::ContrastReset, 90, 0, &Settings::contrast, &MediaPlaybackPreferences::contrast, L"Contrast: 0"},
        {AppCommand::SaturationPlus, 0, 5, &Settings::saturation, &MediaPlaybackPreferences::saturation, L"Saturation: +5"},
        {AppCommand::SaturationMinus, 12, -12, &Settings::saturation, &MediaPlaybackPreferences::saturation, L"Saturation: -12"},
        {AppCommand::SetSaturation, 0, 0, &Settings::saturation, &MediaPlaybackPreferences::saturation, L"Saturation: +0"},
        {AppCommand::SaturationReset, 90, 0, &Settings::saturation, &MediaPlaybackPreferences::saturation, L"Saturation: 0"},
        {AppCommand::GammaPlus, 0, 5, &Settings::gamma, &MediaPlaybackPreferences::gamma, L"Gamma: +5"},
        {AppCommand::GammaMinus, 12, -12, &Settings::gamma, &MediaPlaybackPreferences::gamma, L"Gamma: -12"},
        {AppCommand::SetGamma, 200, 100, &Settings::gamma, &MediaPlaybackPreferences::gamma, L"Gamma: +100"},
        {AppCommand::GammaReset, 90, 0, &Settings::gamma, &MediaPlaybackPreferences::gamma, L"Gamma: 0"},
        {AppCommand::HuePlus, 0, 5, &Settings::hue, &MediaPlaybackPreferences::hue, L"Hue: +5"},
        {AppCommand::HueMinus, 12, -12, &Settings::hue, &MediaPlaybackPreferences::hue, L"Hue: -12"},
        {AppCommand::SetHue, -200, -100, &Settings::hue, &MediaPlaybackPreferences::hue, L"Hue: -100"},
        {AppCommand::HueReset, 90, 0, &Settings::hue, &MediaPlaybackPreferences::hue, L"Hue: 0"},
    };
    for (bool privateMode : {false, true}) {
        for (const auto& test : cases) {
            AdjustmentMockContext ctx;
            const std::wstring path = L"C:\\videos\\command-matrix.mp4";
            ctx.session.OpenDirect(path, 0.0);
            ctx.session.OnPlayerOpened(true);
            ctx.session.SetPrivateMode(privateMode);
            ctx.library.AddOrUpdateMedia(path);
            CommandDispatcher dispatcher(&ctx);
            CommandArgs args;
            args.intVal = test.input;
            TEST_ASSERT(dispatcher.Execute(test.command, args));
            TEST_ASSERT_EQ(ctx.settings.*test.setting, test.expected);
            TEST_ASSERT_EQ(ctx.lastOsdText, std::wstring(test.message));
            TEST_ASSERT(ctx.settingsSaved);
            TEST_ASSERT_EQ(ctx.librarySaved, !privateMode);
            const auto* prefs = ctx.library.GetMediaPreferences(path);
            if (privateMode) {
                TEST_ASSERT(prefs == nullptr);
            } else {
                TEST_ASSERT(prefs != nullptr);
                TEST_ASSERT((prefs->*test.preference).has_value());
                TEST_ASSERT_EQ(*(prefs->*test.preference), test.expected);
            }
        }
    }
}

TEST_CASE(VideoAdjustments, CommandExecutionAndPreferences) {
    AdjustmentMockContext ctx;
    CommandDispatcher dispatcher(&ctx);

    // Open a mock session
    ctx.session.OpenDirect(L"C:\\videos\\movie.mp4", 0.0);
    ctx.session.OnPlayerOpened(true);
    ctx.library.AddOrUpdateMedia(L"C:\\videos\\movie.mp4");

    // 1. RotateVideo
    TEST_ASSERT(dispatcher.Execute(AppCommand::RotateVideo));
    TEST_ASSERT_EQ(ctx.player.GetVideoRotate(), 90);
    TEST_ASSERT_EQ(ctx.settings.videoRotate, 90);
    const MediaPlaybackPreferences* p1 = ctx.library.GetMediaPreferences(L"C:\\videos\\movie.mp4");
    TEST_ASSERT(p1 != nullptr && p1->videoRotate.has_value() && *p1->videoRotate == 90);

    TEST_ASSERT(dispatcher.Execute(AppCommand::RotateVideo));
    TEST_ASSERT_EQ(ctx.player.GetVideoRotate(), 180);

    // 2. SetVideoRotate
    CommandArgs rotArgs;
    rotArgs.intVal = 270;
    TEST_ASSERT(dispatcher.Execute(AppCommand::SetVideoRotate, rotArgs));
    TEST_ASSERT_EQ(ctx.player.GetVideoRotate(), 270);

    // 3. Brightness Plus / Minus / Reset
    TEST_ASSERT(dispatcher.Execute(AppCommand::BrightnessPlus));
    TEST_ASSERT_EQ(ctx.player.GetBrightness(), 5);
    TEST_ASSERT(dispatcher.Execute(AppCommand::BrightnessPlus));
    TEST_ASSERT_EQ(ctx.player.GetBrightness(), 10);
    TEST_ASSERT(dispatcher.Execute(AppCommand::BrightnessMinus));
    TEST_ASSERT_EQ(ctx.player.GetBrightness(), 5);

    const MediaPlaybackPreferences* pBright = ctx.library.GetMediaPreferences(L"C:\\videos\\movie.mp4");
    TEST_ASSERT(pBright != nullptr && pBright->brightness.has_value() && *pBright->brightness == 5);

    TEST_ASSERT(dispatcher.Execute(AppCommand::BrightnessReset));
    TEST_ASSERT_EQ(ctx.player.GetBrightness(), 0);

    // 4. Contrast, Saturation, Gamma, Hue
    TEST_ASSERT(dispatcher.Execute(AppCommand::ContrastPlus));
    TEST_ASSERT_EQ(ctx.player.GetContrast(), 5);

    TEST_ASSERT(dispatcher.Execute(AppCommand::SaturationPlus));
    TEST_ASSERT_EQ(ctx.player.GetSaturation(), 5);

    TEST_ASSERT(dispatcher.Execute(AppCommand::GammaMinus));
    TEST_ASSERT_EQ(ctx.player.GetGamma(), -5);

    TEST_ASSERT(dispatcher.Execute(AppCommand::HuePlus));
    TEST_ASSERT_EQ(ctx.player.GetHue(), 5);

    // 5. Reset All Adjustments
    TEST_ASSERT(dispatcher.Execute(AppCommand::ResetVideoAdjustments));
    TEST_ASSERT_EQ(ctx.player.GetVideoRotate(), 0);
    TEST_ASSERT_EQ(ctx.player.GetBrightness(), 0);
    TEST_ASSERT_EQ(ctx.player.GetContrast(), 0);
    TEST_ASSERT_EQ(ctx.player.GetSaturation(), 0);
    TEST_ASSERT_EQ(ctx.player.GetGamma(), 0);
    TEST_ASSERT_EQ(ctx.player.GetHue(), 0);

    const MediaPlaybackPreferences* pReset = ctx.library.GetMediaPreferences(L"C:\\videos\\movie.mp4");
    TEST_ASSERT(pReset != nullptr);
    TEST_ASSERT(pReset->videoRotate.has_value() && *pReset->videoRotate == 0);
    TEST_ASSERT(pReset->brightness.has_value() && *pReset->brightness == 0);
    TEST_ASSERT(pReset->contrast.has_value() && *pReset->contrast == 0);
    TEST_ASSERT(pReset->saturation.has_value() && *pReset->saturation == 0);
    TEST_ASSERT(pReset->gamma.has_value() && *pReset->gamma == 0);
    TEST_ASSERT(pReset->hue.has_value() && *pReset->hue == 0);

    // 6. Command string mappings
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("rotateVideo"), AppCommand::RotateVideo);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("brightnessPlus"), AppCommand::BrightnessPlus);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("contrastMinus"), AppCommand::ContrastMinus);
    TEST_ASSERT_EQ(CommandDispatcher::CommandIdFromString("resetVideoAdjustments"), AppCommand::ResetVideoAdjustments);

    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::RotateVideo), std::string("rotateVideo"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::BrightnessPlus), std::string("brightnessPlus"));
    TEST_ASSERT_EQ(CommandDispatcher::StringFromCommandId(AppCommand::ResetVideoAdjustments), std::string("resetVideoAdjustments"));
}

} // namespace Aperture::Test
