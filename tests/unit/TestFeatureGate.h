#pragma once

#include "../support/TestFramework.h"
#include "../../src/Common.h"
#include "../../src/FeatureGate.h"
#include "../../src/Json.h"
#include "../../src/Settings.h"
#include <string>
#include <vector>

namespace Aperture::Test {

TEST_CASE(FeatureGate, CatalogIntegrity) {
    const auto& catalog = FeatureGate::Catalog();
    TEST_ASSERT_EQ(catalog.size(), static_cast<size_t>(35));
    for (size_t i = 0; i < catalog.size(); ++i) {
        TEST_ASSERT(catalog[i].key != nullptr && catalog[i].key[0] != '\0');
        TEST_ASSERT(catalog[i].group != nullptr && catalog[i].group[0] != '\0');
        TEST_ASSERT(catalog[i].displayNameKey != nullptr && catalog[i].displayNameKey[0] != '\0');
        TEST_ASSERT(std::string(catalog[i].displayNameKey).rfind("settings.experience.feature.", 0) == 0);
        const bool knownGroup = std::string(catalog[i].group) == "audio" || std::string(catalog[i].group) == "subtitles" ||
                                std::string(catalog[i].group) == "video" || std::string(catalog[i].group) == "playback";
        TEST_ASSERT(knownGroup);
        TEST_ASSERT(!catalog[i].menuIds.empty());
        // Keys must be unique across the catalog.
        for (size_t j = 0; j < i; ++j) {
            TEST_ASSERT(std::string(catalog[i].key) != std::string(catalog[j].key));
        }
        // No menu id may appear in two different features.
        for (size_t j = 0; j < i; ++j) {
            for (const char* menuId : catalog[i].menuIds) {
                for (const char* other : catalog[j].menuIds) {
                    TEST_ASSERT_MSG(std::string(menuId) != std::string(other),
                        (std::string("menu id '") + menuId + "' appears in both " + catalog[i].key +
                         " and " + catalog[j].key).c_str());
                }
            }
            // Prefixes must be unique across features.
            for (const char* prefix : catalog[i].menuIdPrefixes) {
                for (const char* other : catalog[j].menuIdPrefixes) {
                    TEST_ASSERT_MSG(std::string(prefix) != std::string(other),
                        (std::string("menu id prefix '") + prefix + "' appears in both " + catalog[i].key +
                         " and " + catalog[j].key).c_str());
                }
            }
        }
    }
}

TEST_CASE(FeatureGate, ModeResolution) {
    TEST_ASSERT(FeatureGate::ModeFromString("simple") == ExperienceMode::Simple);
    TEST_ASSERT(FeatureGate::ModeFromString("advanced") == ExperienceMode::Advanced);
    TEST_ASSERT(FeatureGate::ModeFromString("custom") == ExperienceMode::Custom);
    TEST_ASSERT(FeatureGate::ModeFromString("bogus") == ExperienceMode::Simple);
    TEST_ASSERT(FeatureGate::ModeFromString("") == ExperienceMode::Simple);
    TEST_ASSERT_EQ(std::string(FeatureGate::ModeToString(ExperienceMode::Simple)), "simple");
    TEST_ASSERT_EQ(std::string(FeatureGate::ModeToString(ExperienceMode::Advanced)), "advanced");
    TEST_ASSERT_EQ(std::string(FeatureGate::ModeToString(ExperienceMode::Custom)), "custom");
    TEST_ASSERT_EQ(std::string(FeatureGate::NormalizeModeString("advanced")), "advanced");
    TEST_ASSERT_EQ(std::string(FeatureGate::NormalizeModeString("unknown")), "simple");
    FeatureGate::FeatureId byKey;
    TEST_ASSERT(FeatureGate::TryFeatureByKey("ab_repeat", byKey));
    TEST_ASSERT(byKey == FeatureGate::FeatureId::ABRepeat);
    TEST_ASSERT(!FeatureGate::TryFeatureByKey("bogus", byKey));

    const std::vector<std::string> empty;
    // Simple mode: Advanced-tier hidden, Simple-tier shown.
    TEST_ASSERT(!FeatureGate::FeatureEnabled(ExperienceMode::Simple, empty, FeatureGate::FeatureId::AudioBalance));
    TEST_ASSERT(FeatureGate::FeatureEnabled(ExperienceMode::Simple, empty, FeatureGate::FeatureId::SleepTimer));
    // Advanced mode: everything shown.
    TEST_ASSERT(FeatureGate::FeatureEnabled(ExperienceMode::Advanced, empty, FeatureGate::FeatureId::AudioBalance));
    TEST_ASSERT(FeatureGate::FeatureEnabled(ExperienceMode::Advanced, empty, FeatureGate::FeatureId::SleepTimer));
    // Custom mode: only keys present in the list (empty list hides everything).
    TEST_ASSERT(!FeatureGate::FeatureEnabled(ExperienceMode::Custom, empty, FeatureGate::FeatureId::AudioBalance));
    TEST_ASSERT(!FeatureGate::FeatureEnabled(ExperienceMode::Custom, empty, FeatureGate::FeatureId::SleepTimer));
    const std::vector<std::string> onlyAb{ "ab_repeat" };
    TEST_ASSERT(FeatureGate::FeatureEnabled(ExperienceMode::Custom, onlyAb, FeatureGate::FeatureId::ABRepeat));
    TEST_ASSERT(!FeatureGate::FeatureEnabled(ExperienceMode::Custom, onlyAb, FeatureGate::FeatureId::AudioBalance));
    TEST_ASSERT(!FeatureGate::FeatureEnabled(ExperienceMode::Custom, onlyAb, FeatureGate::FeatureId::SleepTimer));
}

TEST_CASE(FeatureGate, MenuItemVisibility) {
    const std::vector<std::string> empty;
    // Unknown ids are always visible (default-open surface).
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Simple, empty, "totally_unknown_id"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Advanced, empty, "totally_unknown_id"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Custom, empty, "totally_unknown_id"));
    // Known Advanced-tier feature menu id.
    TEST_ASSERT(!FeatureGate::MenuItemVisible(ExperienceMode::Simple, empty, "audio_balance_menu"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Advanced, empty, "audio_balance_menu"));
    TEST_ASSERT(!FeatureGate::MenuItemVisible(ExperienceMode::Custom, empty, "audio_balance_menu"));
    const std::vector<std::string> balance{ "audio_balance" };
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Custom, balance, "audio_balance_menu"));
    // Simple-tier feature menu id is hidden in Custom when absent from the list.
    TEST_ASSERT(!FeatureGate::MenuItemVisible(ExperienceMode::Custom, empty, "sleep_timer"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Simple, empty, "sleep_timer"));
    // Dynamic menu ids resolve via menuIdPrefixes (dual subtitles tracks).
    TEST_ASSERT(!FeatureGate::MenuItemVisible(ExperienceMode::Simple, empty, "sec_track_5"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Advanced, empty, "sec_track_5"));
    const std::vector<std::string> dual{ "dual_subtitles" };
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Custom, dual, "sec_track_5"));
    TEST_ASSERT(!FeatureGate::MenuItemVisible(ExperienceMode::Custom, empty, "sec_track_5"));
    // An id that merely starts similarly is NOT covered by the prefix; stays visible.
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Simple, empty, "sec_bogus_1"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Advanced, empty, "sec_bogus_1"));
    TEST_ASSERT(FeatureGate::MenuItemVisible(ExperienceMode::Custom, dual, "sec_bogus_1"));
}

TEST_CASE(FeatureGate, SettingsSchemaMigration) {
    JsonValue root;
    TEST_ASSERT(JsonValue::parse("{\"schemaVersion\":15}", root));
    TEST_ASSERT(Settings::MigrateSchema(root, 15, 16));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 16);
    TEST_ASSERT_EQ(root.get("experienceMode").as_string(), "simple");
    TEST_ASSERT(root.get("customEnabledFeatures").is_array());
    TEST_ASSERT(root.get("customEnabledFeatures").as_array().empty());

    JsonValue advancedRoot;
    TEST_ASSERT(JsonValue::parse("{\"schemaVersion\":15,\"experienceMode\":\"advanced\"}", advancedRoot));
    TEST_ASSERT(Settings::MigrateSchema(advancedRoot, 15, 16));
    TEST_ASSERT_EQ(advancedRoot.get("schemaVersion").as_int(), 16);
    TEST_ASSERT_EQ(advancedRoot.get("experienceMode").as_string(), "advanced");
}

TEST_CASE(FeatureGate, SanitizeFeatureKeys) {
    const std::vector<std::string> dirty{ "ab_repeat", "bogus", "ab_repeat", "sleep_timer" };
    const auto clean = FeatureGate::SanitizeFeatureKeys(dirty);
    TEST_ASSERT_EQ(clean.size(), static_cast<size_t>(2));
    TEST_ASSERT_EQ(clean[0], "ab_repeat");
    TEST_ASSERT_EQ(clean[1], "sleep_timer");
    const auto empty = FeatureGate::SanitizeFeatureKeys({});
    TEST_ASSERT(empty.empty());
}

// Mouse-button/wheel pseudo-key bindings (Feature 4): display names must parse
// back to the shared 0x100..0x104 range and FormatShortcutKey must re-emit the
// same name, with or without the Ctrl modifier prefix.
TEST_CASE(ShortcutMouseBindings, RoundTrip) {
    struct Entry { UINT vk; const wchar_t* display; const char* persisted; };
    const Entry entries[] = {
        { 0x100, L"Middle Click", "MiddleClick" },
        { 0x101, L"X Button 1", "XButton1" },
        { 0x102, L"X Button 2", "XButton2" },
        { 0x103, L"Wheel Up", "WheelUp" },
        { 0x104, L"Wheel Down", "WheelDown" },
    };
    for (const auto& e : entries) {
        const std::wstring display(e.display);
        TEST_ASSERT_EQ(Settings::StringToVK(e.persisted), e.vk);
        // Parse the display form too (the capture OSD shows the display name).
        std::string narrowDisplay;
        narrowDisplay.reserve(display.size());
        for (wchar_t c : display) narrowDisplay.push_back(static_cast<char>(c));
        TEST_ASSERT_EQ(Settings::StringToVK(narrowDisplay), e.vk);
        // format(parse) round-trips without modifiers and with Ctrl held.
        ShortcutBinding bare;
        bare.actionId = "testAction";
        bare.displayName = "Test Action";
        bare.vkCode = e.vk;
        TEST_ASSERT_EQ(Settings::FormatShortcutKey(bare), display);
        ShortcutBinding withCtrl = bare;
        withCtrl.ctrl = true;
        TEST_ASSERT_EQ(Settings::FormatShortcutKey(withCtrl), L"Ctrl + " + display);
        // Persisted id survives the VKToString hop.
        TEST_ASSERT_EQ(std::string(Settings::VKToString(e.vk)), std::string(e.persisted));
    }
}

} // namespace Aperture::Test
