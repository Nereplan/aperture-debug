#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/Settings.h"
#include "../../src/Json.h"
#include "../../src/MpvPlayer.h"
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(SubtitleProfiles, SettingsSchemaMigrationAndDefaults) {
    // Stage 8 JSON with custom extension properties
    std::string v8Json = "{\n"
        "  \"schemaVersion\": 8,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"volume\": 0.95,\n"
        "  \"hdrMode\": \"auto\",\n"
        "  \"audioDevice\": \"auto\",\n"
        "  \"customPluginSetting\": \"SubtitleSyncPro\"\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(v8Json, root));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 8);

    // Migrate 8 -> 9
    TEST_ASSERT(Settings::MigrateSchema(root, 8, 9));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 9);

    // Verify newly added subtitle profile defaults
    TEST_ASSERT(root.has_key("subtitleProfiles"));
    TEST_ASSERT(root.get("subtitleProfiles").is_array());
    TEST_ASSERT_EQ(root.get("subtitleProfiles").as_array().size(), 0);
    TEST_ASSERT_EQ(root.get("activeSubtitleProfile").as_string(), "");

    // Verify unknown extension property was preserved intact
    TEST_ASSERT_EQ(root.get("customPluginSetting").as_string(), "SubtitleSyncPro");
}

TEST_CASE(SubtitleProfiles, ProfileValidationAndSanitization) {
    // Valid profile names
    TEST_ASSERT(Settings::IsValidProfileName("Anime"));
    TEST_ASSERT(Settings::IsValidProfileName("Movie Night"));
    TEST_ASSERT(Settings::IsValidProfileName("High-Contrast (Yellow)"));
    TEST_ASSERT(Settings::IsValidProfileName("Profile_123"));

    // Invalid profile names
    TEST_ASSERT(!Settings::IsValidProfileName(""));
    TEST_ASSERT(!Settings::IsValidProfileName("   "));
    TEST_ASSERT(!Settings::IsValidProfileName("\t\n"));
    
    // Control character rejection
    std::string badName = "Style";
    badName.push_back(static_cast<char>(7)); // Bell
    TEST_ASSERT(!Settings::IsValidProfileName(badName));

    // Length boundary (max 64)
    std::string exact64(64, 'A');
    TEST_ASSERT(Settings::IsValidProfileName(exact64));
    std::string over64(65, 'A');
    TEST_ASSERT(!Settings::IsValidProfileName(over64));

    // Sanitization
    TEST_ASSERT_EQ(Settings::SanitizeProfileName("  Cinema Style  "), std::string("Cinema Style"));
    std::string withCtrl = "Anime\x1F Style";
    TEST_ASSERT_EQ(Settings::SanitizeProfileName(withCtrl), std::string("Anime Style"));
    std::string over64WithSpaces = "   " + std::string(100, 'X') + "   ";
    std::string sanitized = Settings::SanitizeProfileName(over64WithSpaces);
    TEST_ASSERT_EQ(sanitized.length(), 64);
    TEST_ASSERT_EQ(sanitized, std::string(64, 'X'));
}

TEST_CASE(SubtitleProfiles, StyleNormalizationAndFallbacks) {
    // 1. Font Size normalization
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("14px"), std::string("14px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("18PX"), std::string("18px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize(" 20px "), std::string("20px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("24px"), std::string("24px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("32px"), std::string("32px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("18"), std::string("18px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("24"), std::string("24px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize("invalid_size"), std::string("20px"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileFontSize(""), std::string("20px"));

    // 2. Color Hex normalization
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("#ffffff"), std::string("#ffffff"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("#FFFF00"), std::string("#ffff00"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("#00E6A8"), std::string("#00e6a8"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("ff0000"), std::string("#ff0000"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("not-a-color"), std::string("#cad3f5"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor("#12345"), std::string("#cad3f5"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileColor(""), std::string("#cad3f5"));

    // 3. Background Style normalization
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle("vlc-outline"), std::string("vlc-outline"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle("TRANSLUCENT"), std::string("translucent"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle(" Solid "), std::string("solid"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle("clean"), std::string("clean"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle("invalid_style"), std::string("vlc-outline"));
    TEST_ASSERT_EQ(Settings::NormalizeProfileBgStyle(""), std::string("vlc-outline"));

    // 4. Position normalization
    TEST_ASSERT_EQ(Settings::NormalizeProfilePosition("bottom"), std::string("bottom"));
    TEST_ASSERT_EQ(Settings::NormalizeProfilePosition("ELEVATED"), std::string("elevated"));
    TEST_ASSERT_EQ(Settings::NormalizeProfilePosition(" Top "), std::string("top"));
    TEST_ASSERT_EQ(Settings::NormalizeProfilePosition("middle"), std::string("bottom"));
    TEST_ASSERT_EQ(Settings::NormalizeProfilePosition(""), std::string("bottom"));
}

TEST_CASE(SubtitleProfiles, ProfileCreationUpdateAndBounds) {
    Settings s;
    s.ResetToDefaults();
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 0);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "");

    // Add profile 1
    SubtitleStyleProfile p1;
    p1.name = "Yellow Big";
    p1.fontSize = "24px";
    p1.colorHex = "#ffff00";
    p1.bgStyle = "translucent";
    p1.position = "bottom";
    TEST_ASSERT(s.AddOrUpdateSubtitleProfile(p1));
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 1);

    const SubtitleStyleProfile* found = s.FindSubtitleProfile("Yellow Big");
    TEST_ASSERT(found != nullptr);
    TEST_ASSERT_EQ(found->name, std::string("Yellow Big"));
    TEST_ASSERT_EQ(found->fontSize, std::string("24px"));
    TEST_ASSERT_EQ(found->colorHex, std::string("#ffff00"));
    TEST_ASSERT_EQ(found->bgStyle, std::string("translucent"));
    TEST_ASSERT_EQ(found->position, std::string("bottom"));

    // Case-insensitive lookup
    TEST_ASSERT(s.FindSubtitleProfile("yellow big") != nullptr);

    // In-place update with case-insensitivity
    SubtitleStyleProfile p1Updated;
    p1Updated.name = "YELLOW BIG";
    p1Updated.fontSize = "32px";
    p1Updated.colorHex = "#00e6a8";
    p1Updated.bgStyle = "solid";
    p1Updated.position = "elevated";
    TEST_ASSERT(s.AddOrUpdateSubtitleProfile(p1Updated));
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 1);

    found = s.FindSubtitleProfile("Yellow Big");
    TEST_ASSERT(found != nullptr);
    TEST_ASSERT_EQ(found->fontSize, std::string("32px"));
    TEST_ASSERT_EQ(found->colorHex, std::string("#00e6a8"));
    TEST_ASSERT_EQ(found->bgStyle, std::string("solid"));
    TEST_ASSERT_EQ(found->position, std::string("elevated"));

    // Fill up to bound (kMaxSubtitleProfiles = 32)
    for (size_t i = 2; i <= Settings::kMaxSubtitleProfiles; ++i) {
        SubtitleStyleProfile pi;
        pi.name = "Profile " + std::to_string(i);
        pi.fontSize = "18px";
        pi.colorHex = "#ffffff";
        pi.bgStyle = "vlc-outline";
        pi.position = "bottom";
        TEST_ASSERT(s.AddOrUpdateSubtitleProfile(pi));
    }
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), Settings::kMaxSubtitleProfiles);

    // 33rd profile must be rejected due to bounds limit
    SubtitleStyleProfile pExcess;
    pExcess.name = "Excess Profile";
    TEST_ASSERT(!s.AddOrUpdateSubtitleProfile(pExcess));
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), Settings::kMaxSubtitleProfiles);
    TEST_ASSERT(s.FindSubtitleProfile("Excess Profile") == nullptr);
}

TEST_CASE(SubtitleProfiles, ProfileApplicationAndActiveState) {
    Settings s;
    s.ResetToDefaults();

    // Default style
    TEST_ASSERT_EQ(s.subFontSize, "20px");
    TEST_ASSERT_EQ(s.subColorHex, "#cad3f5");
    TEST_ASSERT_EQ(s.subBgStyle, "vlc-outline");
    TEST_ASSERT_EQ(s.subPosition, "bottom");

    // Create current style snapshot
    s.subFontSize = "24px";
    s.subColorHex = "#ffff00";
    s.subBgStyle = "solid";
    s.subPosition = "elevated";

    SubtitleStyleProfile snap = s.GetCurrentSubtitleStyleAsProfile("Anime Preset");
    TEST_ASSERT_EQ(snap.name, "Anime Preset");
    TEST_ASSERT_EQ(snap.fontSize, "24px");
    TEST_ASSERT_EQ(snap.colorHex, "#ffff00");
    TEST_ASSERT_EQ(snap.bgStyle, "solid");
    TEST_ASSERT_EQ(snap.position, "elevated");

    TEST_ASSERT(s.AddOrUpdateSubtitleProfile(snap));

    // Reset settings to default
    s.ResetSubtitleStyleToDefault();
    TEST_ASSERT_EQ(s.subFontSize, "20px");
    TEST_ASSERT_EQ(s.subColorHex, "#cad3f5");
    TEST_ASSERT_EQ(s.subBgStyle, "vlc-outline");
    TEST_ASSERT_EQ(s.subPosition, "bottom");
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "");

    // Apply "Anime Preset"
    TEST_ASSERT(s.ApplySubtitleProfile("Anime Preset"));
    TEST_ASSERT_EQ(s.subFontSize, "24px");
    TEST_ASSERT_EQ(s.subColorHex, "#ffff00");
    TEST_ASSERT_EQ(s.subBgStyle, "solid");
    TEST_ASSERT_EQ(s.subPosition, "elevated");
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Anime Preset");

    // Applying non-existent profile fails and preserves settings
    TEST_ASSERT(!s.ApplySubtitleProfile("NonExistent"));
    TEST_ASSERT_EQ(s.subFontSize, "24px");
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Anime Preset");
}

TEST_CASE(SubtitleProfiles, ProfileRenamingAndDeletion) {
    Settings s;
    s.ResetToDefaults();

    SubtitleStyleProfile p1; p1.name = "Compact"; p1.fontSize = "14px";
    SubtitleStyleProfile p2; p2.name = "Large"; p2.fontSize = "32px";
    s.AddOrUpdateSubtitleProfile(p1);
    s.AddOrUpdateSubtitleProfile(p2);

    s.ApplySubtitleProfile("Large");
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Large");

    // 1. Rename active profile
    TEST_ASSERT(s.RenameSubtitleProfile("Large", "Cinema Giant"));
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Cinema Giant");
    TEST_ASSERT(s.FindSubtitleProfile("Large") == nullptr);
    TEST_ASSERT(s.FindSubtitleProfile("Cinema Giant") != nullptr);
    TEST_ASSERT_EQ(s.FindSubtitleProfile("Cinema Giant")->fontSize, "32px");

    // 2. Reject rename collision with another existing profile
    TEST_ASSERT(!s.RenameSubtitleProfile("Compact", "Cinema Giant"));
    TEST_ASSERT(s.FindSubtitleProfile("Compact") != nullptr);

    // 3. Rename non-active profile
    TEST_ASSERT(s.RenameSubtitleProfile("Compact", "Subtle"));
    TEST_ASSERT(s.FindSubtitleProfile("Compact") == nullptr);
    TEST_ASSERT(s.FindSubtitleProfile("Subtle") != nullptr);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Cinema Giant"); // Active profile untouched

    // 4. Delete non-active profile
    TEST_ASSERT(s.DeleteSubtitleProfile("Subtle"));
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 1);
    TEST_ASSERT(s.FindSubtitleProfile("Subtle") == nullptr);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Cinema Giant");

    // 5. Delete active profile
    TEST_ASSERT(s.DeleteSubtitleProfile("Cinema Giant"));
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 0);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, ""); // Active profile cleared

    // 6. Delete non-existent profile returns false
    TEST_ASSERT(!s.DeleteSubtitleProfile("Ghost"));
}

TEST_CASE(SubtitleProfiles, RestoreDefaultStyle) {
    Settings s;
    s.ResetToDefaults();

    SubtitleStyleProfile p; p.name = "Custom 1";
    s.AddOrUpdateSubtitleProfile(p);
    s.ApplySubtitleProfile("Custom 1");

    s.subFontSize = "32px";
    s.subColorHex = "#ff0000";
    s.subBgStyle = "clean";
    s.subPosition = "top";

    s.ResetSubtitleStyleToDefault();

    TEST_ASSERT_EQ(s.subFontSize, "20px");
    TEST_ASSERT_EQ(s.subColorHex, "#cad3f5");
    TEST_ASSERT_EQ(s.subBgStyle, "vlc-outline");
    TEST_ASSERT_EQ(s.subPosition, "bottom");
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "");

    // Profiles list must remain preserved
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 1);
    TEST_ASSERT(s.FindSubtitleProfile("Custom 1") != nullptr);
}

TEST_CASE(SubtitleProfiles, UnknownFieldsPreservationAndRoundTrip) {
    // Stage 9 JSON with custom profile extension properties and root extension properties
    std::string fullJson = "{\n"
        "  \"schemaVersion\": 9,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"subFontSize\": \"24px\",\n"
        "  \"subColor\": \"#ffff00\",\n"
        "  \"subBgStyle\": \"solid\",\n"
        "  \"subPosition\": \"elevated\",\n"
        "  \"activeSubtitleProfile\": \"Anime Glow\",\n"
        "  \"customRootPluginData\": { \"installed\": true },\n"
        "  \"subtitleProfiles\": [\n"
        "    {\n"
        "      \"name\": \"Anime Glow\",\n"
        "      \"fontSize\": \"24px\",\n"
        "      \"colorHex\": \"#ffff00\",\n"
        "      \"bgStyle\": \"solid\",\n"
        "      \"position\": \"elevated\",\n"
        "      \"glowRadius\": 4.5,\n"
        "      \"shadowBlur\": 2.0\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"Minimal\",\n"
        "      \"subFontSize\": \"14px\",\n"
        "      \"subColor\": \"#cccccc\",\n"
        "      \"subBgStyle\": \"clean\",\n"
        "      \"subPosition\": \"bottom\",\n"
        "      \"opacity\": 0.8\n"
        "    }\n"
        "  ]\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(fullJson, root));

    // Simulate settings load from parsed JSON
    Settings s;
    s.ResetToDefaults();

    s.activeSubtitleProfile = root.get("activeSubtitleProfile").as_string("");
    s.subFontSize = root.get("subFontSize").as_string(s.subFontSize);
    s.subColorHex = root.get("subColor").as_string(s.subColorHex);
    s.subBgStyle = root.get("subBgStyle").as_string(s.subBgStyle);
    s.subPosition = root.get("subPosition").as_string(s.subPosition);

    const auto& arr = root.get("subtitleProfiles").as_array();
    for (const auto& item : arr) {
        SubtitleStyleProfile prof;
        prof.name = Settings::SanitizeProfileName(item.get("name").as_string(""));
        std::string sz = item.has_key("fontSize") ? item.get("fontSize").as_string("") : item.get("subFontSize").as_string("");
        prof.fontSize = Settings::NormalizeProfileFontSize(sz);
        std::string col = item.has_key("color") ? item.get("color").as_string("") : (item.has_key("colorHex") ? item.get("colorHex").as_string("") : item.get("subColor").as_string(""));
        prof.colorHex = Settings::NormalizeProfileColor(col);
        std::string bg = item.has_key("bgStyle") ? item.get("bgStyle").as_string("") : item.get("subBgStyle").as_string("");
        prof.bgStyle = Settings::NormalizeProfileBgStyle(bg);
        std::string pos = item.has_key("position") ? item.get("position").as_string("") : item.get("subPosition").as_string("");
        prof.position = Settings::NormalizeProfilePosition(pos);

        for (const auto& kv : item.as_object()) {
            const std::string& k = kv.first;
            if (k != "name" && k != "fontSize" && k != "subFontSize" && k != "color" && k != "colorHex" && k != "subColor" && k != "bgStyle" && k != "subBgStyle" && k != "position" && k != "subPosition") {
                prof.unknownFields[k] = kv.second;
            }
        }
        s.AddOrUpdateSubtitleProfile(prof);
    }

    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Anime Glow");
    TEST_ASSERT_EQ(s.subtitleProfiles.size(), 2);

    const SubtitleStyleProfile* p1 = s.FindSubtitleProfile("Anime Glow");
    TEST_ASSERT(p1 != nullptr);
    TEST_ASSERT_EQ(p1->fontSize, "24px");
    TEST_ASSERT_EQ(p1->colorHex, "#ffff00");
    TEST_ASSERT_EQ(p1->bgStyle, "solid");
    TEST_ASSERT_EQ(p1->position, "elevated");
    TEST_ASSERT(p1->unknownFields.has_key("glowRadius"));
    TEST_ASSERT_EQ(p1->unknownFields.get("glowRadius").as_number(), 4.5);
    TEST_ASSERT(p1->unknownFields.has_key("shadowBlur"));
    TEST_ASSERT_EQ(p1->unknownFields.get("shadowBlur").as_number(), 2.0);

    const SubtitleStyleProfile* p2 = s.FindSubtitleProfile("Minimal");
    TEST_ASSERT(p2 != nullptr);
    TEST_ASSERT_EQ(p2->fontSize, "14px");
    TEST_ASSERT_EQ(p2->colorHex, "#cccccc");
    TEST_ASSERT_EQ(p2->bgStyle, "clean");
    TEST_ASSERT_EQ(p2->position, "bottom");
    TEST_ASSERT(p2->unknownFields.has_key("opacity"));
    TEST_ASSERT_EQ(p2->unknownFields.get("opacity").as_number(), 0.8);
}

TEST_CASE(SubtitleProfiles, SafeAreaValidationAndNormalization) {
    // 1. Safe Area percentage validation and clamping [0, 30]
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(0), 0);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(5), 5);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(10), 10);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(20), 20);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(30), 30);

    // Out of bounds values
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(-1), 0);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(-100), 0);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(31), 30);
    TEST_ASSERT_EQ(Settings::NormalizeSafeAreaPercent(100), 30);
}

TEST_CASE(SubtitleProfiles, SafeAreaPresetsAndLookup) {
    auto presets = Settings::GetSubtitleSafeAreaPresets();
    TEST_ASSERT_EQ(presets.size(), 5);

    TEST_ASSERT_EQ(presets[0].id, "none");
    TEST_ASSERT_EQ(presets[0].x, 0);
    TEST_ASSERT_EQ(presets[0].y, 0);

    TEST_ASSERT_EQ(presets[1].id, "action");
    TEST_ASSERT_EQ(presets[1].x, 5);
    TEST_ASSERT_EQ(presets[1].y, 5);

    TEST_ASSERT_EQ(presets[2].id, "title");
    TEST_ASSERT_EQ(presets[2].x, 10);
    TEST_ASSERT_EQ(presets[2].y, 10);

    TEST_ASSERT_EQ(presets[3].id, "wide");
    TEST_ASSERT_EQ(presets[3].x, 15);
    TEST_ASSERT_EQ(presets[3].y, 15);

    TEST_ASSERT_EQ(presets[4].id, "cinema");
    TEST_ASSERT_EQ(presets[4].x, 20);
    TEST_ASSERT_EQ(presets[4].y, 20);

    // Case-insensitive lookup by ID and display name
    int x = -1, y = -1;
    TEST_ASSERT(Settings::GetSubtitleSafeAreaPreset("action", x, y));
    TEST_ASSERT_EQ(x, 5);
    TEST_ASSERT_EQ(y, 5);

    TEST_ASSERT(Settings::GetSubtitleSafeAreaPreset("ACTION", x, y));
    TEST_ASSERT_EQ(x, 5);
    TEST_ASSERT_EQ(y, 5);

    TEST_ASSERT(Settings::GetSubtitleSafeAreaPreset("Title Safe (10%)", x, y));
    TEST_ASSERT_EQ(x, 10);
    TEST_ASSERT_EQ(y, 10);

    TEST_ASSERT(Settings::GetSubtitleSafeAreaPreset("wide", x, y));
    TEST_ASSERT_EQ(x, 15);
    TEST_ASSERT_EQ(y, 15);

    TEST_ASSERT(!Settings::GetSubtitleSafeAreaPreset("non_existent", x, y));
    TEST_ASSERT(!Settings::GetSubtitleSafeAreaPreset("", x, y));
}

TEST_CASE(SubtitleProfiles, SafeAreaPlanGenerationAndMpvProperties) {
    // 1. Off / Default (0%, 0%)
    auto plan0 = MpvPlayer::BuildSubtitleSafeAreaPlan(0, 0);
    TEST_ASSERT_EQ(plan0.requestedSafeAreaX, 0);
    TEST_ASSERT_EQ(plan0.requestedSafeAreaY, 0);
    TEST_ASSERT_EQ(plan0.normalizedSafeAreaX, 0);
    TEST_ASSERT_EQ(plan0.normalizedSafeAreaY, 0);
    TEST_ASSERT_EQ(plan0.marginX, 0);
    TEST_ASSERT_EQ(plan0.marginY, 0);
    TEST_ASSERT_EQ(plan0.actions.size(), 3);
    TEST_ASSERT_EQ(plan0.actions[0].first, "sub-margin-x");
    TEST_ASSERT_EQ(plan0.actions[0].second, "0");
    TEST_ASSERT_EQ(plan0.actions[1].first, "sub-margin-y");
    TEST_ASSERT_EQ(plan0.actions[1].second, "0");
    TEST_ASSERT_EQ(plan0.actions[2].first, "sub-use-margins");
    TEST_ASSERT_EQ(plan0.actions[2].second, "yes");

    // 2. Action Safe (5%, 5%) -> 1280 * 0.05 = 64, 720 * 0.05 = 36
    auto plan5 = MpvPlayer::BuildSubtitleSafeAreaPlan(5, 5);
    TEST_ASSERT_EQ(plan5.normalizedSafeAreaX, 5);
    TEST_ASSERT_EQ(plan5.normalizedSafeAreaY, 5);
    TEST_ASSERT_EQ(plan5.marginX, 64);
    TEST_ASSERT_EQ(plan5.marginY, 36);
    TEST_ASSERT_EQ(plan5.actions[0].second, "64");
    TEST_ASSERT_EQ(plan5.actions[1].second, "36");

    // 3. Title Safe (10%, 10%) -> 1280 * 0.10 = 128, 720 * 0.10 = 72
    auto plan10 = MpvPlayer::BuildSubtitleSafeAreaPlan(10, 10);
    TEST_ASSERT_EQ(plan10.normalizedSafeAreaX, 10);
    TEST_ASSERT_EQ(plan10.normalizedSafeAreaY, 10);
    TEST_ASSERT_EQ(plan10.marginX, 128);
    TEST_ASSERT_EQ(plan10.marginY, 72);
    TEST_ASSERT_EQ(plan10.actions[0].second, "128");
    TEST_ASSERT_EQ(plan10.actions[1].second, "72");

    // 4. Asymmetric Safe Area (15% H, 20% V) -> 1280 * 0.15 = 192, 720 * 0.20 = 144
    auto planAsym = MpvPlayer::BuildSubtitleSafeAreaPlan(15, 20);
    TEST_ASSERT_EQ(planAsym.normalizedSafeAreaX, 15);
    TEST_ASSERT_EQ(planAsym.normalizedSafeAreaY, 20);
    TEST_ASSERT_EQ(planAsym.marginX, 192);
    TEST_ASSERT_EQ(planAsym.marginY, 144);

    // 5. Clamping in plan builder (-10% H, 50% V) -> 0% H, 30% V (720 * 0.30 = 216)
    auto planClamp = MpvPlayer::BuildSubtitleSafeAreaPlan(-10, 50);
    TEST_ASSERT_EQ(planClamp.normalizedSafeAreaX, 0);
    TEST_ASSERT_EQ(planClamp.normalizedSafeAreaY, 30);
    TEST_ASSERT_EQ(planClamp.marginX, 0);
    TEST_ASSERT_EQ(planClamp.marginY, 216);
}

TEST_CASE(SubtitleProfiles, SafeAreaProfileIntegrationAndPrecedence) {
    Settings s;
    s.ResetToDefaults();

    TEST_ASSERT_EQ(s.subSafeAreaX, 0);
    TEST_ASSERT_EQ(s.subSafeAreaY, 0);

    // Create a profile with custom safe area settings
    SubtitleStyleProfile prof;
    prof.name = "Broadcast Safe";
    prof.fontSize = "24px";
    prof.colorHex = "#ffff00";
    prof.bgStyle = "translucent";
    prof.position = "bottom";
    prof.safeAreaX = 10;
    prof.safeAreaY = 10;
    TEST_ASSERT(s.AddOrUpdateSubtitleProfile(prof));

    const SubtitleStyleProfile* found = s.FindSubtitleProfile("Broadcast Safe");
    TEST_ASSERT(found != nullptr);
    TEST_ASSERT_EQ(found->safeAreaX, 10);
    TEST_ASSERT_EQ(found->safeAreaY, 10);

    // Applying the profile sets global subSafeAreaX/Y
    TEST_ASSERT(s.ApplySubtitleProfile("Broadcast Safe"));
    TEST_ASSERT_EQ(s.subSafeAreaX, 10);
    TEST_ASSERT_EQ(s.subSafeAreaY, 10);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "Broadcast Safe");

    // Global tweaks take immediate effect
    s.subSafeAreaX = 15;
    s.subSafeAreaY = 20;
    TEST_ASSERT_EQ(s.subSafeAreaX, 15);
    TEST_ASSERT_EQ(s.subSafeAreaY, 20);

    // GetCurrentSubtitleStyleAsProfile captures active safe-area values
    SubtitleStyleProfile snap = s.GetCurrentSubtitleStyleAsProfile("Custom Broadcast");
    TEST_ASSERT_EQ(snap.name, "Custom Broadcast");
    TEST_ASSERT_EQ(snap.safeAreaX, 15);
    TEST_ASSERT_EQ(snap.safeAreaY, 20);

    // Resetting safe area
    s.ResetSubtitleSafeArea();
    TEST_ASSERT_EQ(s.subSafeAreaX, 0);
    TEST_ASSERT_EQ(s.subSafeAreaY, 0);

    // Re-applying profile restores profile safe area
    TEST_ASSERT(s.ApplySubtitleProfile("Broadcast Safe"));
    TEST_ASSERT_EQ(s.subSafeAreaX, 10);
    TEST_ASSERT_EQ(s.subSafeAreaY, 10);

    // Resetting default style resets safe area to 0
    s.ResetSubtitleStyleToDefault();
    TEST_ASSERT_EQ(s.subSafeAreaX, 0);
    TEST_ASSERT_EQ(s.subSafeAreaY, 0);
    TEST_ASSERT_EQ(s.activeSubtitleProfile, "");
}

TEST_CASE(SubtitleProfiles, SafeAreaSerializationAndRoundTrip) {
    // Stage 9 JSON containing subSafeAreaX, subSafeAreaY, and profiles with safeAreaX/Y
    std::string fullJson = "{\n"
        "  \"schemaVersion\": 9,\n"
        "  \"themePreset\": \"default\",\n"
        "  \"subSafeAreaX\": 10,\n"
        "  \"subSafeAreaY\": 15,\n"
        "  \"subtitleProfiles\": [\n"
        "    {\n"
        "      \"name\": \"Action Cinema\",\n"
        "      \"fontSize\": \"24px\",\n"
        "      \"colorHex\": \"#ffff00\",\n"
        "      \"bgStyle\": \"solid\",\n"
        "      \"position\": \"bottom\",\n"
        "      \"safeAreaX\": 5,\n"
        "      \"safeAreaY\": 20,\n"
        "      \"customExtra\": 42\n"
        "    }\n"
        "  ]\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(fullJson, root));
    TEST_ASSERT_EQ(root.get("subSafeAreaX").as_int(), 10);
    TEST_ASSERT_EQ(root.get("subSafeAreaY").as_int(), 15);

    Settings s;
    s.ResetToDefaults();
    s.subSafeAreaX = Settings::NormalizeSafeAreaPercent(root.get("subSafeAreaX").as_int(0));
    s.subSafeAreaY = Settings::NormalizeSafeAreaPercent(root.get("subSafeAreaY").as_int(0));

    const auto& arr = root.get("subtitleProfiles").as_array();
    for (const auto& item : arr) {
        SubtitleStyleProfile prof;
        prof.name = Settings::SanitizeProfileName(item.get("name").as_string(""));
        prof.fontSize = Settings::NormalizeProfileFontSize(item.get("fontSize").as_string(""));
        prof.colorHex = Settings::NormalizeProfileColor(item.get("colorHex").as_string(""));
        prof.bgStyle = Settings::NormalizeProfileBgStyle(item.get("bgStyle").as_string(""));
        prof.position = Settings::NormalizeProfilePosition(item.get("position").as_string(""));
        prof.safeAreaX = Settings::NormalizeSafeAreaPercent(item.get("safeAreaX").as_int(0));
        prof.safeAreaY = Settings::NormalizeSafeAreaPercent(item.get("safeAreaY").as_int(0));

        for (const auto& kv : item.as_object()) {
            const std::string& k = kv.first;
            if (k != "name" && k != "fontSize" && k != "subFontSize" && k != "color" && k != "colorHex" && k != "subColor" && k != "bgStyle" && k != "subBgStyle" && k != "position" && k != "subPosition" && k != "safeAreaX" && k != "safeAreaY" && k != "subSafeAreaX" && k != "subSafeAreaY") {
                prof.unknownFields[k] = kv.second;
            }
        }
        s.AddOrUpdateSubtitleProfile(prof);
    }

    TEST_ASSERT_EQ(s.subSafeAreaX, 10);
    TEST_ASSERT_EQ(s.subSafeAreaY, 15);

    const SubtitleStyleProfile* loadedProf = s.FindSubtitleProfile("Action Cinema");
    TEST_ASSERT(loadedProf != nullptr);
    TEST_ASSERT_EQ(loadedProf->safeAreaX, 5);
    TEST_ASSERT_EQ(loadedProf->safeAreaY, 20);
    TEST_ASSERT(loadedProf->unknownFields.has_key("customExtra"));
    TEST_ASSERT_EQ(loadedProf->unknownFields.get("customExtra").as_int(), 42);
    // Verify safe area fields did not leak into unknownFields
    TEST_ASSERT(!loadedProf->unknownFields.has_key("safeAreaX"));
    TEST_ASSERT(!loadedProf->unknownFields.has_key("safeAreaY"));
}

TEST_CASE(SubtitleProfiles, SafeAreaDiagnosticsProjection) {
    PlayerDiagnostics diag;
    diag.activeSubSafeAreaX = 10;
    diag.activeSubSafeAreaY = 15;
    diag.activeSubSafeArea = std::to_wstring(diag.activeSubSafeAreaX) + L"% H, " + std::to_wstring(diag.activeSubSafeAreaY) + L"% V";

    TEST_ASSERT_EQ(diag.activeSubSafeAreaX, 10);
    TEST_ASSERT_EQ(diag.activeSubSafeAreaY, 15);
    TEST_ASSERT_EQ(diag.activeSubSafeArea, std::wstring(L"10% H, 15% V"));
}

} // namespace Aperture::Test
