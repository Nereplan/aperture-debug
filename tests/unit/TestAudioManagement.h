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

TEST_CASE(AudioManagement, PlanCompositionAndDeviceFallback) {
    std::vector<AudioDeviceInfo> devices = {
        { "auto", L"Autoselect device", true },
        { "wasapi/{0.0.0.00000000}.{Speakers}", L"Realtek High Definition Audio", false },
        { "wasapi/{0.0.0.00000001}.{Headphones}", L"USB Wireless Headset", false }
    };

    bool fellBack = false;
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioDevice("wasapi/{0.0.0.00000001}.{Headphones}", devices, &fellBack),
        std::string("wasapi/{0.0.0.00000001}.{Headphones}"));
    TEST_ASSERT(!fellBack);

    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioDevice("wasapi/{9.9.9.99999999}.{Vanished}", devices, &fellBack),
        std::string("auto"));
    TEST_ASSERT(fellBack);

    auto vanishedPlan = AudioConfig::BuildAudioPlan("wasapi/{9.9.9.99999999}.{Vanished}", "stereo", "auto", true, "off", devices);
    TEST_ASSERT_EQ(vanishedPlan.requestedDevice, std::string("wasapi/{9.9.9.99999999}.{Vanished}"));
    TEST_ASSERT_EQ(vanishedPlan.normalizedDevice, std::string("auto"));
    TEST_ASSERT(vanishedPlan.deviceFellBackToAuto);

    {
        auto downmixPlan = AudioConfig::BuildAudioPlan("auto", "5.1", "downmix-stereo", true, "off");
        TEST_ASSERT_EQ(downmixPlan.normalizedLayout, std::string("5.1"));
        TEST_ASSERT_EQ(downmixPlan.normalizedChannelMix, std::string("downmix-stereo"));
        bool foundStereo = false, foundNormalize = false;
        for (const auto& act : downmixPlan.actions) {
            if (act.property == "audio-channels" && act.value == "stereo") foundStereo = true;
            if (act.property == "audio-normalize-downmix" && act.value == "yes") foundNormalize = true;
        }
        TEST_ASSERT(foundStereo && foundNormalize);
    }

    {
        auto upmixPlan = AudioConfig::BuildAudioPlan("auto", "stereo", "upmix-surround", false, "off");
        TEST_ASSERT_EQ(upmixPlan.normalizedChannelMix, std::string("upmix-surround"));
        bool found51 = false, foundNoNormalize = false;
        for (const auto& act : upmixPlan.actions) {
            if (act.property == "audio-channels" && act.value == "5.1") found51 = true;
            if (act.property == "audio-normalize-downmix" && act.value == "no") foundNoNormalize = true;
        }
        TEST_ASSERT(found51 && foundNoNormalize);
    }

    {
        auto combinedPlan = AudioConfig::BuildAudioPlan("auto", "stereo", "auto", true, "strong", 0.75, "swap");
        TEST_ASSERT_EQ(combinedPlan.normalizedNightMode, std::string("strong"));
        TEST_ASSERT_EQ(combinedPlan.normalizedChannelMap, std::string("swap"));
        TEST_ASSERT_EQ(combinedPlan.filterString,
            std::string("lavfi=[dynaudnorm=f=150:g=27:p=0.95:m=15.0],lavfi=[pan=stereo|c0=c1|c1=c0]"));
        bool foundBalance = false, foundAf = false;
        for (const auto& act : combinedPlan.actions) {
            if (act.property == "balance" && act.value == "0.75") foundBalance = true;
            if (act.property == "af" && act.value == combinedPlan.filterString) foundAf = true;
        }
        TEST_ASSERT(foundBalance && foundAf);
    }

    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(1.5), 1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(-2.0), -1.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioBalance(std::numeric_limits<double>::quiet_NaN()), 0.0);
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("left-only"), std::string("mono-left"));
    TEST_ASSERT_EQ(AudioConfig::NormalizeAudioChannelMap("invalid_map"), std::string("auto"));

    MpvPlayer player;
    auto results = player.SetAudioPreferences("wasapi/{Speakers}", "5.1", "downmix-stereo", true, "medium", 0.5, "swap");
    TEST_ASSERT(!results.empty());
    for (const auto& res : results) {
        TEST_ASSERT(!res.success);
        TEST_ASSERT_EQ(res.mpvErrorCode, -1);
    }
    auto diag = player.GetDiagnostics();
    TEST_ASSERT_EQ(diag.activeAudioLayout, L"5.1");
    TEST_ASSERT_EQ(diag.activeAudioChannelMix, L"downmix-stereo");
    TEST_ASSERT_EQ(diag.activeNightMode, L"medium");
    TEST_ASSERT_EQ(diag.activeAudioBalance, 0.5);
    TEST_ASSERT_EQ(diag.activeAudioChannelMap, L"swap");
}

} // namespace Aperture::Test
