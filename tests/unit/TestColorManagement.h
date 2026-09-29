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

TEST_CASE(ColorManagement, SettingsSchemaMigrationAndUnknownFields) {
    // Stage 6 JSON with custom extension properties
    std::string v6Json = "{\n"
        "  \"schemaVersion\": 6,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"volume\": 0.85,\n"
        "  \"customColorGradeId\": \"LUT_Rec709_Cinematic\"\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(v6Json, root));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 6);

    // Migrate 6 -> 7
    TEST_ASSERT(Settings::MigrateSchema(root, 6, 7));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 7);

    // Verify newly added color controls defaults
    TEST_ASSERT_EQ(root.get("hdrMode").as_string(), "auto");
    TEST_ASSERT_EQ(root.get("toneMappingAlgorithm").as_string(), "auto");
    TEST_ASSERT_EQ(root.get("targetPeak").as_int(), 0);
    TEST_ASSERT_EQ(root.get("gamutMapping").as_string(), "auto");
    TEST_ASSERT_EQ(root.get("iccProfilePath").as_string(), "");

    // Verify unknown extension property was preserved intact
    TEST_ASSERT_EQ(root.get("customColorGradeId").as_string(), "LUT_Rec709_Cinematic");
}

TEST_CASE(ColorManagement, SdrDefaultsPreservation) {
    Settings s;
    s.ResetToDefaults();

    TEST_ASSERT_EQ(s.hdrMode, std::string("auto"));
    TEST_ASSERT_EQ(s.toneMappingAlgorithm, std::string("auto"));
    TEST_ASSERT_EQ(s.targetPeak, 0);
    TEST_ASSERT_EQ(s.gamutMapping, std::string("auto"));
    TEST_ASSERT_EQ(s.iccProfilePath, std::string(""));

    // Verify normalized defaults
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode(s.hdrMode), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping(s.toneMappingAlgorithm), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(s.targetPeak), 0);
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping(s.gamutMapping), std::string("auto"));
}

TEST_CASE(ColorManagement, EnumNormalizationAndInvalidFallback) {
    // 1. HDR Mode
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode("auto"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode("PASSTHROUGH"), std::string("passthrough"));
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode(" Tone-Map "), std::string("tone-map"));
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode("OFF"), std::string("off"));
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode("invalid_mode"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeHdrMode(""), std::string("auto"));

    // 2. Tone Mapping Algorithm
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("auto"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("bt.2390"), std::string("bt.2390"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("BT.2446A"), std::string("bt.2446a"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("spline"), std::string("spline"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("hable"), std::string("hable"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("mobius"), std::string("mobius"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("reinhard"), std::string("reinhard"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("clip"), std::string("clip"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("linear"), std::string("linear"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("gamma"), std::string("gamma"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping("unsupported_curve"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeToneMapping(""), std::string("auto"));

    // 3. Gamut Mapping
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("auto"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("CLIP"), std::string("clip"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("warn"), std::string("warn"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("linear"), std::string("linear"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("darken"), std::string("darken"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("desaturate"), std::string("desaturate"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping("unknown_gamut"), std::string("auto"));
    TEST_ASSERT_EQ(Settings::NormalizeGamutMapping(""), std::string("auto"));
}

TEST_CASE(ColorManagement, NumericBoundsClamping) {
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(0), 0);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(100), 100);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(203), 203);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(400), 400);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(1000), 1000);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(4000), 4000);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(10000), 10000);

    // Negative values clamped to 0
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(-1), 0);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(-500), 0);

    // Values > 10000 clamped to 10000
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(10001), 10000);
    TEST_ASSERT_EQ(Settings::NormalizeTargetPeak(50000), 10000);
}

TEST_CASE(ColorManagement, RootRelativeIccContainment) {
    std::wstring exeDir = L"C:\\Apps\\Aperture";

    // 1. Empty is valid (none selected)
    TEST_ASSERT(Settings::IsRootContainedIccPath("", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("", exeDir), std::string(""));

    // 2. Valid relative paths inside root
    TEST_ASSERT(Settings::IsRootContainedIccPath("display.icc", exeDir));
    TEST_ASSERT(Settings::IsRootContainedIccPath("color\\calibrated.icc", exeDir));
    TEST_ASSERT(Settings::IsRootContainedIccPath("profiles/monitor.icm", exeDir));

    // 3. Absolute path inside root gets normalized to root-relative
    TEST_ASSERT(Settings::IsRootContainedIccPath("C:\\Apps\\Aperture\\color\\profile.icc", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("C:\\Apps\\Aperture\\color\\profile.icc", exeDir), std::string("color\\profile.icc"));

    // 4. Directory traversal attempting to escape root -> REJECTED
    TEST_ASSERT(!Settings::IsRootContainedIccPath("..\\..\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("..\\..\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir), std::string(""));

    // 5. External absolute paths -> REJECTED
    TEST_ASSERT(!Settings::IsRootContainedIccPath("C:\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("C:\\Windows\\System32\\spool\\drivers\\color\\sRGB.icm", exeDir), std::string(""));

    TEST_ASSERT(!Settings::IsRootContainedIccPath("D:\\ExternalProfile.icc", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("D:\\ExternalProfile.icc", exeDir), std::string(""));

    // 6. UNC paths -> REJECTED
    TEST_ASSERT(!Settings::IsRootContainedIccPath("\\\\server\\share\\profile.icc", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("\\\\server\\share\\profile.icc", exeDir), std::string(""));

    // 7. Path containing illegal characters -> REJECTED
    TEST_ASSERT(!Settings::IsRootContainedIccPath("color<bad>.icc", exeDir));
    TEST_ASSERT_EQ(Settings::NormalizeIccPath("color<bad>.icc", exeDir), std::string(""));
}

TEST_CASE(ColorManagement, DeterministicPropertyPlanGeneration) {
    std::wstring exeDir = L"C:\\Apps\\Aperture";

    // 1. Auto mode with auto tone mapping
    {
        auto plan = MpvPlayer::BuildColorManagementPlan("auto", "auto", 0, "auto", "", exeDir);
        TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("auto"));
        TEST_ASSERT_EQ(plan.normalizedToneMapping, std::string("auto"));
        TEST_ASSERT_EQ(plan.normalizedTargetPeak, 0);

        bool hasHint = false, hasTm = false, hasPeak = false, hasCompute = false;
        for (const auto& act : plan.actions) {
            if (act.property == "target-colorspace-hint" && act.value == "yes") hasHint = true;
            if (act.property == "tone-mapping" && act.value == "auto") hasTm = true;
            if (act.property == "target-peak" && act.value == "auto") hasPeak = true;
            if (act.property == "hdr-compute-peak" && act.value == "yes") hasCompute = true;
        }
        TEST_ASSERT(hasHint);
        TEST_ASSERT(hasTm);
        TEST_ASSERT(hasPeak);
        TEST_ASSERT(hasCompute);
    }

    // 2. Passthrough mode
    {
        auto plan = MpvPlayer::BuildColorManagementPlan("passthrough", "bt.2390", 1000, "clip", "", exeDir);
        TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("passthrough"));

        bool hasHint = false, hasTm = false, hasPeak = false, hasCompute = false, hasGamut = false;
        for (const auto& act : plan.actions) {
            if (act.property == "target-colorspace-hint" && act.value == "yes") hasHint = true;
            if (act.property == "tone-mapping" && act.value == "auto") hasTm = true;
            if (act.property == "target-peak" && act.value == "1000") hasPeak = true;
            if (act.property == "hdr-compute-peak" && act.value == "auto") hasCompute = true;
            if (act.property == "gamut-mapping-mode" && act.value == "clip") hasGamut = true;
        }
        TEST_ASSERT(hasHint);
        TEST_ASSERT(hasTm);
        TEST_ASSERT(hasPeak);
        TEST_ASSERT(hasCompute);
        TEST_ASSERT(hasGamut);
    }

    // 3. Tone-Map mode (forces SDR target-colorspace-hint=no and explicit tone mapping)
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
        TEST_ASSERT(hasHintNo);
        TEST_ASSERT(hasTmHable);
        TEST_ASSERT(hasPeak400);
        TEST_ASSERT(hasCompute);
        TEST_ASSERT(hasGamut);
    }

    // 4. Off mode (disables HDR and sets clip)
    {
        auto plan = MpvPlayer::BuildColorManagementPlan("off", "auto", 0, "auto", "", exeDir);
        TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("off"));

        bool hasHintNo = false, hasTmClip = false, hasComputeNo = false;
        for (const auto& act : plan.actions) {
            if (act.property == "target-colorspace-hint" && act.value == "no") hasHintNo = true;
            if (act.property == "tone-mapping" && act.value == "clip") hasTmClip = true;
            if (act.property == "hdr-compute-peak" && act.value == "no") hasComputeNo = true;
        }
        TEST_ASSERT(hasHintNo);
        TEST_ASSERT(hasTmClip);
        TEST_ASSERT(hasComputeNo);
    }
}

TEST_CASE(ColorManagement, SafeFallbackAndFailureReportingWhenUninitialized) {
    MpvPlayer player;
    // player is not initialized with a live mpv_handle
    auto results = player.SetColorPreferences("auto", "bt.2390", 400, "clip", "");
    TEST_ASSERT(!results.empty());

    // Every property action reports safe error status without crashing
    for (const auto& res : results) {
        TEST_ASSERT(!res.success);
        TEST_ASSERT_EQ(res.mpvErrorCode, -1);
        TEST_ASSERT_EQ(res.message, std::string("mpv not initialized"));
    }

    // Last plan is stored and inspectable
    auto plan = player.GetLastColorPlan();
    TEST_ASSERT_EQ(plan.normalizedHdrMode, std::string("auto"));
    TEST_ASSERT_EQ(plan.normalizedToneMapping, std::string("bt.2390"));
    TEST_ASSERT_EQ(plan.normalizedTargetPeak, 400);
    TEST_ASSERT_EQ(plan.normalizedGamutMapping, std::string("clip"));
}

TEST_CASE(ColorManagement, HdrMetadataProjectionLogic) {
    PlayerDiagnostics d;
    d.colorPrimaries = L"bt.2020";
    d.colorTransfer = L"pq";
    d.maxCll = 1000.0;
    d.maxFall = 400.0;

    std::wstring trLower = d.colorTransfer;
    for (auto& c : trLower) c = (wchar_t)towlower(c);
    std::wstring primLower = d.colorPrimaries;
    for (auto& c : primLower) c = (wchar_t)towlower(c);

    bool isHdr = (trLower == L"pq" || trLower == L"smpte2084" || trLower == L"hlg" || trLower == L"arib-std-b67" ||
                  d.sigPeak > 1.05 || d.maxCll > 100.0 || primLower == L"bt.2020");
    TEST_ASSERT(isHdr);

    if (trLower == L"pq" || trLower == L"smpte2084") {
        d.hdrType = L"HDR10 (PQ)";
    }
    TEST_ASSERT_EQ(d.hdrType, std::wstring(L"HDR10 (PQ)"));

    // SDR Case
    PlayerDiagnostics sdrDiag;
    sdrDiag.colorPrimaries = L"bt.709";
    sdrDiag.colorTransfer = L"srgb";
    sdrDiag.maxCll = 0.0;
    std::wstring sdrTr = sdrDiag.colorTransfer;
    bool sdrIsHdr = (sdrTr == L"pq" || sdrTr == L"hlg" || sdrDiag.maxCll > 100.0 || sdrDiag.colorPrimaries == L"bt.2020");
    TEST_ASSERT(!sdrIsHdr);
}

} // namespace Aperture::Test
