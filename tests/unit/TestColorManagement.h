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

TEST_CASE(ColorManagement, IccContainmentAndToneMapPlan) {
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(0), 0);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(1000), 1000);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(-500), 0);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(50000), 10000);

    std::wstring exeDir = L"C:\\Apps\\Aperture";
    TEST_ASSERT(Settings::IsRootContainedIccPath("", exeDir));
    TEST_ASSERT(Settings::IsRootContainedIccPath("color\\calibrated.icc", exeDir));
    TEST_ASSERT(Settings::IsRootContainedIccPath("C:\\Apps\\Aperture\\color\\profile.icc", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("C:\\Apps\\Aperture\\color\\profile.icc", exeDir), std::string("color\\profile.icc"));

    TEST_ASSERT(!Settings::IsRootContainedIccPath("..\\..\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("..\\..\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir), std::string(""));
    TEST_ASSERT(!Settings::IsRootContainedIccPath("C:\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir));
    TEST_ASSERT(!Settings::IsRootContainedIccPath("\\\\server\\share\\profile.icc", exeDir));
    TEST_ASSERT(!Settings::IsRootContainedIccPath("color<bad>.icc", exeDir));

    {
        auto plan = MpvPlayer::BuildColorManagementPlan("tone-map", "hable", 400, "desaturate", "", exeDir);
        TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("tone-map"));
        bool hasHintNo = false, hasTmHable = false, hasPeak400 = false, hasCompute = false, hasGamut = false;
        for (const auto& act : plan.actions) {
            if (act.property == "target-colorspace-hint" && act.value == "no") hasHintNo = true;
            if (act.property == "tone-mapping" && act.value == "hable") hasTmHable = true;
            if (act.property == "target-peak" && act.value == "400") hasPeak400 = true;
            if (act.property == "hdr-compute-peak" && act.value == "yes") hasCompute = true;
            if (act.property == "gamut-mapping-mode" && act.value == "desaturate") hasGamut = true;
        }
        TEST_ASSERT(hasHintNo && hasTmHable && hasPeak400 && hasCompute && hasGamut);
    }

    {
        auto plan = MpvPlayer::BuildColorManagementPlan("passthrough", "bt.2390", 1000, "clip", "", exeDir);
        TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("passthrough"));
        bool hasHintYes = false, hasPeak1000 = false;
        for (const auto& act : plan.actions) {
            if (act.property == "target-colorspace-hint" && act.value == "yes") hasHintYes = true;
            if (act.property == "target-peak" && act.value == "1000") hasPeak1000 = true;
        }
        TEST_ASSERT(hasHintYes && hasPeak1000);
    }

    MpvPlayer player;
    auto results = player.SetColorPreferences("auto", "bt.2390", 400, "clip", "");
    TEST_ASSERT(!results.empty());
    for (const auto& res : results) {
        TEST_ASSERT(!res.success);
        TEST_ASSERT_EQ(res.mpvErrorCode, -1);
    }
    auto lastPlan = player.GetLastColorPlan();
    TEST_ASSERT_EQ(lastPlan.normalizedToneMapping, std::string("bt.2390"));
    TEST_ASSERT_EQ(lastPlan.normalizedTargetPeak, 400);
}

} // namespace Aperture::Test
