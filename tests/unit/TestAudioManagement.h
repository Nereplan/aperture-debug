#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/MpvPlayer.h"
#include "../../src/AudioConfig.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Json.h"
#include <windows.h>
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(AudioManagement, SettingsSchemaMigrationAndDefaults) {
    // Stage 7 JSON with custom extension properties
    std::string v7Json = "{\n"
        "  \"schemaVersion\": 7,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"volume\": 0.90,\n"
        "  \"hdrMode\": \"passthrough\",\n"
        "  \"customEqualizerProfile\": \"VocalBoost\"\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(v7Json, root));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 7);

    // Migrate 7 -> 8
    TEST_ASSERT(Settings::MigrateSchema(root, 7, 8));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 8);

    // Verify newly added audio controls defaults
    TEST_ASSERT_EQ(root.get("audioDevice").as_string(), "auto");
    TEST_ASSERT_EQ(root.get("audioLayout").as_string(), "auto-safe");
    TEST_ASSERT_EQ(root.get("audioChannelMix").as_string(), "auto");
    TEST_ASSERT_EQ(root.get("audioNormalizeDownmix").as_bool(), true);
    TEST_ASSERT_EQ(root.get("nightMode").as_string(), "off");
    TEST_ASSERT_EQ(root.get("audioBalance").as_number(), 0.0);
    TEST_ASSERT_EQ(root.get("audioChannelMap").as_string(), "auto");

    // Verify unknown extension property was preserved intact
    TEST_ASSERT_EQ(root.get("customEqualizerProfile").as_string(), "VocalBoost");
}

TEST_CASE(AudioManagement, SettingsRoundTripAndSdrPreservation) {
    Settings s;
    s.ResetToDefaults();

    // Verify defaults
    TEST_ASSERT_EQ(s.audioDevice, std::string("auto"));
    TEST_ASSERT_EQ(s.audioLayout, std::string("auto-safe"));
    TEST_ASSERT_EQ(s.audioChannelMix, std::string("auto"));
    TEST_ASSERT_EQ(s.audioNormalizeDownmix, true);
    TEST_ASSERT_EQ(s.nightMode, std::string("off"));
    TEST_ASSERT_EQ(s.audioBalance, 0.0);
    TEST_ASSERT_EQ(s.audioChannelMap, std::string("auto"));

    // Verify normalized defaults
    TEST_ASSERT_EQ(Settings::NormalizeAudioDevice(s.audioDevice), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeAudioLayout(s.audioLayout), std::string("auto-safe"));
    TEST_ASSERT_EQ(Settings::NormalizeAudioChannelMix(s.audioChannelMix), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeNightMode(s.nightMode), std::string("off"));
    TEST_ASSERT_EQ(Settings::NormalizeAudioBalance(s.audioBalance), 0.0);
    TEST_ASSERT_EQ(Settings::NormalizeAudioChannelMap(s.audioChannelMap), std::string("auto"));

    // Verify SDR / playback defaults are untouched
    TEST_ASSERT_EQ(s.hdrMode, std::string("auto"));
    TEST_ASSERT_EQ(s.toneMappingAlgorithm, std::string("auto"));
    TEST_ASSERT_EQ(s.volume, 1.0);
    TEST_ASSERT_EQ(s.muted, false);
}

TEST_CASE(AudioManagement, EnumNormalizationAndInvalidFallback) {
    // 1. Audio Layout Normalization
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("auto-safe"), std::string("auto-safe"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("AUTO"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("stereo"), std::string("stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("2.0"), std::string("stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("5.1"), std::string("5.1"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("surround51"), std::string("5.1"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("7.1"), std::string("7.1"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("surround71"), std::string("7.1"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout("invalid_layout"), std::string("auto-safe"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioLayout(""), std::string("auto-safe"));

    // 2. Audio Channel Mix Normalization
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("auto"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("none"), std::string("none"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("DIRECT"), std::string("none"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("downmix-stereo"), std::string("downmix-stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("downmix"), std::string("downmix-stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("upmix-surround"), std::string("upmix-surround"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("upmix"), std::string("upmix-surround"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix("invalid_mix"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMix(""), std::string("auto"));

    // 3. Night Mode Normalization
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("off"), std::string("off"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("NONE"), std::string("off"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("light"), std::string("light"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("LOW"), std::string("light"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("medium"), std::string("medium"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("standard"), std::string("medium"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("strong"), std::string("strong"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("HIGH"), std::string("strong"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode("invalid_mode"), std::string("off"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeNightMode(""), std::string("off"));
}

TEST_CASE(AudioManagement, VanishedDeviceFallback) {
    std::vector<AudioDeviceInfo> devices = {
        { "auto", L"Autoselect device", true },
        { "wasapi/{0.0.0.00000000}.{Speakers}", L"Realtek High Definition Audio", false },
        { "wasapi/{0.0.0.00000001}.{Headphones}", L"USB Wireless Headset", false }
    };

    // 1. Existing device -> accepted without fallback
    bool fellBack1 = false;
    std::string norm1 = AudioConfig::NormalizeAudioDevice("wasapi/{0.0.0.00000001}.{Headphones}", devices, &fellBack1);
    TEST_ASSERT_EQ(norm1, std::string("wasapi/{0.0.0.00000001}.{Headphones}"));
    TEST_ASSERT(!fellBack1);

    // 2. Disconnected / Vanished device -> falls back to "auto" with fallback flag set
    bool fellBack2 = false;
    std::string norm2 = AudioConfig::NormalizeAudioDevice("wasapi/{9.9.9.99999999}.{Vanished}", devices, &fellBack2);
    TEST_ASSERT_EQ(norm2, std::string("auto"));
    TEST_ASSERT(fellBack2);

    // 3. Explicit "auto" / "default" -> "auto" without fallback flag
    bool fellBack3 = false;
    std::string norm3 = AudioConfig::NormalizeAudioDevice("auto", devices, &fellBack3);
    TEST_ASSERT_EQ(norm3, std::string("auto"));
    TEST_ASSERT(!fellBack3);

    // 4. Plan generation with vanished device sets deviceFellBackToAuto
    auto plan = AudioConfig::BuildAudioPlan("wasapi/{9.9.9.99999999}.{Vanished}", "stereo", "auto", true, "off", devices);
    TEST_ASSERT_EQ(plan.requestedDevice, std::string("wasapi/{9.9.9.99999999}.{Vanished}"));
    TEST_ASSERT_EQ(plan.normalizedDevice, std::string("auto"));
    TEST_ASSERT(plan.deviceFellBackToAuto);
}

TEST_CASE(AudioManagement, SurroundLayoutsAndChannelMixPropertyPlans) {
    // 1. Stereo layout
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off");
        TEST_ASSERT_EQ(plan.normalizedLayout, std::string("stereo"));
        bool foundChannels = false;
        for (const auto& act : plan.actions) {
            if (act.property == "audio-channels" && act.value == "stereo") foundChannels = true;
        }
        TEST_ASSERT(foundChannels);
    }

    // 2. 5.1 Surround layout
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "5.1", "auto", true, "off");
        TEST_ASSERT_EQ(plan.normalizedLayout, std::string("5.1"));
        bool foundChannels = false;
        for (const auto& act : plan.actions) {
            if (act.property == "audio-channels" && act.value == "5.1") foundChannels = true;
        }
        TEST_ASSERT(foundChannels);
    }

    // 3. 7.1 Surround layout
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "7.1", "auto", true, "off");
        TEST_ASSERT_EQ(plan.normalizedLayout, std::string("7.1"));
        bool foundChannels = false;
        for (const auto& act : plan.actions) {
            if (act.property == "audio-channels" && act.value == "7.1") foundChannels = true;
        }
        TEST_ASSERT(foundChannels);
    }

    // 4. Safe Downmix Mix on 5.1 layout forces stereo output
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "5.1", "downmix-stereo", true, "off");
        TEST_ASSERT_EQ(plan.normalizedLayout, std::string("5.1"));
        TEST_ASSERT_EQ(plan.normalizedChannelMix, std::string("downmix-stereo"));
        bool foundStereo = false;
        bool foundNormalize = false;
        for (const auto& act : plan.actions) {
            if (act.property == "audio-channels" && act.value == "stereo") foundStereo = true;
            if (act.property == "audio-normalize-downmix" && act.value == "yes") foundNormalize = true;
        }
        TEST_ASSERT(foundStereo);
        TEST_ASSERT(foundNormalize);
    }

    // 5. Upmix Mix on Stereo layout forces 5.1 output
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "upmix-surround", false, "off");
        TEST_ASSERT_EQ(plan.normalizedChannelMix, std::string("upmix-surround"));
        bool found51 = false;
        bool foundNoNormalize = false;
        for (const auto& act : plan.actions) {
            if (act.property == "audio-channels" && act.value == "5.1") found51 = true;
            if (act.property == "audio-normalize-downmix" && act.value == "no") foundNoNormalize = true;
        }
        TEST_ASSERT(found51);
        TEST_ASSERT(foundNoNormalize);
    }
}

TEST_CASE(AudioManagement, NightPresetsAndPropertyPlans) {
    // 1. Off Preset -> empty af
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("off"));
        TEST_ASSERT_EQ(plan.filterString, std::string(""));
        bool foundAf = false;
        for (const auto& act : plan.actions) {
            if (act.property == "af" && act.value == "") foundAf = true;
        }
        TEST_ASSERT(foundAf);
    }

    // 2. Light Preset -> dynaudnorm light parameters
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "light");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("light"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=250:g=15:p=0.90:m=5.0]"));
        bool foundAf = false;
        for (const auto& act : plan.actions) {
            if (act.property == "af" && act.value == plan.filterString) foundAf = true;
        }
        TEST_ASSERT(foundAf);
    }

    // 3. Medium Preset -> dynaudnorm medium parameters
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "medium");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("medium"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=200:g=21:p=0.95:m=10.0]"));
        bool foundAf = false;
        for (const auto& act : plan.actions) {
            if (act.property == "af" && act.value == plan.filterString) foundAf = true;
        }
        TEST_ASSERT(foundAf);
    }

    // 4. Strong Preset -> dynaudnorm strong parameters
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "strong");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("strong"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=150:g=27:p=0.95:m=15.0]"));
        bool foundAf = false;
        for (const auto& act : plan.actions) {
            if (act.property == "af" && act.value == plan.filterString) foundAf = true;
        }
        TEST_ASSERT(foundAf);
    }

    // 5. Invalid Preset -> fallback to off
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "unknown_preset");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("off"));
        TEST_ASSERT_EQ(plan.filterString, std::string(""));
    }
}

TEST_CASE(AudioManagement, SafeFallbackAndFailureProjectionWhenUninitialized) {
    MpvPlayer player;
    // player is not initialized with a live mpv_handle
    auto results = player.SetAudioPreferences("wasapi/{Speakers}", "5.1", "downmix-stereo", true, "medium");
    TEST_ASSERT(!results.empty());

    // Every property action reports safe error status without crashing or breaking state
    for (const auto& res : results) {
        TEST_ASSERT(!res.success);
        TEST_ASSERT_EQ(res.mpvErrorCode, -1);
        TEST_ASSERT_EQ(res.message, std::string("mpv not initialized"));
    }

    // Last plan is stored and inspectable
    auto plan = player.GetLastAudioPlan();
    TEST_ASSERT_EQ(plan.requestedDevice, std::string("wasapi/{Speakers}"));
    TEST_ASSERT_EQ(plan.normalizedLayout, std::string("5.1"));
    TEST_ASSERT_EQ(plan.normalizedChannelMix, std::string("downmix-stereo"));
    TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("medium"));
    TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=200:g=21:p=0.95:m=10.0]"));

    // Diagnostics projection from uninitialized player reflects plan
    auto diag = player.GetDiagnostics();
    TEST_ASSERT_EQ(diag.activeAudioLayout, L"5.1");
    TEST_ASSERT_EQ(diag.activeAudioChannelMix, L"downmix-stereo");
    TEST_ASSERT_EQ(diag.activeNightMode, L"medium");
}

TEST_CASE(AudioManagement, DeviceAndLayoutDiagnosticsProjection) {
    PlayerDiagnostics d;
    d.activeAudioDevice = L"wasapi/{0.0.0.00000001}.{Headphones}";
    d.activeAudioDeviceDescription = L"USB Wireless Headset";
    d.activeAudioLayout = L"5.1";
    d.activeAudioChannelMix = L"downmix-stereo";
    d.activeNightMode = L"medium";
    d.audioDeviceFallback = false;

    TEST_ASSERT_EQ(d.activeAudioDevice, L"wasapi/{0.0.0.00000001}.{Headphones}");
    TEST_ASSERT_EQ(d.activeAudioDeviceDescription, L"USB Wireless Headset");
    TEST_ASSERT_EQ(d.activeAudioLayout, L"5.1");
    TEST_ASSERT_EQ(d.activeAudioChannelMix, L"downmix-stereo");
    TEST_ASSERT_EQ(d.activeNightMode, L"medium");
    TEST_ASSERT(!d.audioDeviceFallback);

    // Test fallback scenario
    d.activeAudioDevice = L"auto";
    d.activeAudioDeviceDescription = L"Autoselect device (Default)";
    d.audioDeviceFallback = true;
    TEST_ASSERT(d.audioDeviceFallback);
}

TEST_CASE(AudioManagement, AudioBalanceNormalizationAndFallback) {
    // 1. Exact finite values within [-1.0, 1.0]
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(0.0), 0.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(0.5), 0.5);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(-0.5), -0.5);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(1.0), 1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(-1.0), -1.0);

    // 2. Clamping beyond bounds
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(1.5), 1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(-2.0), -1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(100.0), 1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(-999.0), -1.0);

    // 3. Non-finite values fallback safely to 0.0
    double nanVal = std::numeric_limits<double>::quiet_NaN();
    double infVal = std::numeric_limits<double>::infinity();
    double negInfVal = -std::numeric_limits<double>::infinity();
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(nanVal), 0.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(infVal), 0.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(negInfVal), 0.0);
}

TEST_CASE(AudioManagement, AudioChannelMapNormalizationAndFallback) {
    // 1. Standard modes
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("auto"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("stereo"), std::string("stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("mono-left"), std::string("mono-left"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("mono-right"), std::string("mono-right"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("swap"), std::string("swap"));

    // 2. Case-insensitivity & whitespace trimming
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("  STEREO  "), std::string("stereo"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("Mono-Left"), std::string("mono-left"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("MONO_RIGHT"), std::string("mono-right"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap(" Swap "), std::string("swap"));

    // 3. Common synonyms
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("default"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("left"), std::string("mono-left"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("left-only"), std::string("mono-left"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("right"), std::string("mono-right"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("right-only"), std::string("mono-right"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("swapped"), std::string("swap"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("reverse"), std::string("swap"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("flip"), std::string("swap"));

    // 4. Invalid fallback
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("invalid_channel_map"), std::string("auto"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap(""), std::string("auto"));
}

TEST_CASE(AudioManagement, AudioFilterCompositionWithChannelMapAndNightMode) {
    // 1. Channel map filters isolated
    TEST_ASSERT_EQ(AudioConfig::GetChannelMapFilter("auto"), std::string(""));
    TEST_ASSERT_EQ(AudioConfig::GetChannelMapFilter("stereo"), std::string(""));
    TEST_ASSERT_EQ(AudioConfig::GetChannelMapFilter("mono-left"), std::string("lavfi=[pan=stereo|c0=c0|c1=c0]"));
    TEST_ASSERT_EQ(AudioConfig::GetChannelMapFilter("mono-right"), std::string("lavfi=[pan=stereo|c0=c1|c1=c1]"));
    TEST_ASSERT_EQ(AudioConfig::GetChannelMapFilter("swap"), std::string("lavfi=[pan=stereo|c0=c1|c1=c0]"));

    // 2. Channel map only in plan
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off", -0.4, "mono-left");
        TEST_ASSERT_EQ(plan.normalizedBalance, -0.4);
        TEST_ASSERT_EQ(plan.normalizedChannelMap, std::string("mono-left"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[pan=stereo|c0=c0|c1=c0]"));
    }

    // 3. Night mode DRC only in plan
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "medium", 0.0, "auto");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("medium"));
        TEST_ASSERT_EQ(plan.normalizedChannelMap, std::string("auto"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=200:g=21:p=0.95:m=10.0]"));
    }

    // 4. Combined Night mode + Channel Map filter chain
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "strong", 0.3, "swap");
        TEST_ASSERT_EQ(plan.normalizedNightMode, std::string("strong"));
        TEST_ASSERT_EQ(plan.normalizedChannelMap, std::string("swap"));
        TEST_ASSERT_EQ(plan.filterString, std::string("lavfi=[dynaudnorm=f=150:g=27:p=0.95:m=15.0],lavfi=[pan=stereo|c0=c1|c1=c0]"));
    }
}

TEST_CASE(AudioManagement, AudioBalanceAndChannelMapPropertyPlans) {
    // 1. Verify balance property action generated correctly
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off", 0.75, "mono-right");
        bool foundBalance = false;
        bool foundAf = false;
        for (const auto& act : plan.actions) {
            if (act.property == "balance" && act.value == "0.75") foundBalance = true;
            if (act.property == "af" && act.value == "lavfi=[pan=stereo|c0=c1|c1=c1]") foundAf = true;
        }
        TEST_ASSERT(foundBalance);
        TEST_ASSERT(foundAf);
    }

    // 2. Full left balance
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off", -1.0, "auto");
        bool foundBalance = false;
        for (const auto& act : plan.actions) {
            if (act.property == "balance" && act.value == "-1.00") foundBalance = true;
        }
        TEST_ASSERT(foundBalance);
    }

    // 3. Full right balance
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off", 1.0, "auto");
        bool foundBalance = false;
        for (const auto& act : plan.actions) {
            if (act.property == "balance" && act.value == "1.00") foundBalance = true;
        }
        TEST_ASSERT(foundBalance);
    }

    // 4. Center balance
    {
        auto plan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "off", 0.0, "auto");
        bool foundBalance = false;
        for (const auto& act : plan.actions) {
            if (act.property == "balance" && act.value == "0.00") foundBalance = true;
        }
        TEST_ASSERT(foundBalance);
    }
}

TEST_CASE(AudioManagement, PerMediaPreferencesAudioBalanceAndChannelMapRoundTrip) {
    MediaPlaybackPreferences prefs;
    prefs.audioBalance = -0.5;
    prefs.audioChannelMap = "swap";
    prefs.extraFields = JsonValue::Object();
    prefs.extraFields["customAudioDSP"] = "Crystalizer";

    TEST_ASSERT(prefs.HasAnyPreference());
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAudioBalance(*prefs.audioBalance));
    TEST_ASSERT(MediaPlaybackPreferences::IsValidAudioChannelMap(*prefs.audioChannelMap));

    // Save and reload via library save/load
    MediaLibrary lib;
    std::wstring mediaPath = L"C:\\test\\media_audio.mp4";
    MediaItem* item = lib.AddOrUpdateMedia(mediaPath);
    TEST_ASSERT(item != nullptr);
    item->preferences = prefs;

    auto saveRes = lib.Save();
    TEST_ASSERT(saveRes.is_ok());

    MediaLibrary reloadedLib;
    auto loadStatus = reloadedLib.Load();
    TEST_ASSERT(loadStatus.is_ok());

    MediaItem* loadedItem = reloadedLib.FindByPath(mediaPath);
    TEST_ASSERT(loadedItem != nullptr);
    TEST_ASSERT(loadedItem->preferences.has_value());
    TEST_ASSERT(loadedItem->preferences->audioBalance.has_value());
    TEST_ASSERT_EQ(*loadedItem->preferences->audioBalance, -0.5);
    TEST_ASSERT(loadedItem->preferences->audioChannelMap.has_value());
    TEST_ASSERT_EQ(*loadedItem->preferences->audioChannelMap, std::string("swap"));

    // Unknown field preservation
    TEST_ASSERT(loadedItem->preferences->extraFields.has_key("customAudioDSP"));
    TEST_ASSERT_EQ(loadedItem->preferences->extraFields.get("customAudioDSP").as_string(), "Crystalizer");
}

TEST_CASE(AudioManagement, AudioBalanceAndChannelMapDiagnosticsProjection) {
    PlayerDiagnostics d;
    d.activeAudioBalance = -0.25;
    d.activeAudioChannelMap = L"mono-left";

    TEST_ASSERT_EQ(d.activeAudioBalance, -0.25);
    TEST_ASSERT_EQ(d.activeAudioChannelMap, L"mono-left");

    MpvPlayer player;
    player.SetAudioPreferences("auto", "stereo", "auto", true, "off", 0.5, "swap");
    auto diag = player.GetDiagnostics();
    TEST_ASSERT_EQ(diag.activeAudioBalance, 0.5);
    TEST_ASSERT_EQ(diag.activeAudioChannelMap, L"swap");
}

} // namespace Aperture::Test
