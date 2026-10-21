#pragma once

#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Common.h"
#include "../../src/Localization.h"
#include "../../src/Settings.h"
#include "../../src/Json.h"
#include <thread>
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(Localization, CatalogSafetyFormattingAndConcurrency) {
    auto& loc = Localization::Instance();
    ScopedTempDir tempDir(L"Aperture_Loc_Test_");
    TEST_ASSERT(tempDir.CreateSubDir(L"locales"));
    // Check built-ins before loading any English overlay into the singleton fallback maps.
    TEST_ASSERT(loc.Initialize(tempDir.GetPath(), "en-US"));
    const auto& catalog = Localization::GetCanonicalEnglishCatalog();
    TEST_ASSERT(catalog.size() >= 100);
    for (const auto& kv : catalog) {
        TEST_ASSERT(!kv.first.empty() && !kv.second.empty());
        TEST_ASSERT_EQ(loc.GetUtf8(kv.first), kv.second);
        TEST_ASSERT_EQ(loc.Get(kv.first), Utf8ToUtf16(kv.second));
    }

    const auto& plurals = Localization::GetCanonicalEnglishPlurals();
    TEST_ASSERT(!plurals.empty());
    auto verifyEnglishPlurals = [&]() {
        for (const auto& kv : plurals) {
            TEST_ASSERT(!kv.first.empty() && !kv.second.one.empty() && !kv.second.other.empty());
            TEST_ASSERT(kv.second.zero.empty() && kv.second.two.empty() && kv.second.few.empty() && kv.second.many.empty());
            for (int count : { 0, 1, 2, 5 }) {
                const auto expected = Localization::FormatPatternUtf8(count == 1 ? kv.second.one : kv.second.other, { std::to_string(count) });
                TEST_ASSERT_EQ(loc.FormatPluralUtf8(kv.first, count), expected);
                TEST_ASSERT_EQ(loc.FormatPlural(kv.first, count), Utf8ToUtf16(expected));
            }
        }
    };
    verifyEnglishPlurals();

    const auto sourceLocale = fs::path(__FILE__).parent_path().parent_path().parent_path() / L"locales" / L"en-US.json";
    std::string englishJson;
    TEST_ASSERT(ReadTextFile(sourceLocale.wstring(), englishJson).is_ok());
    JsonValue englishRoot;
    TEST_ASSERT(JsonValue::parse(englishJson, englishRoot));
    TEST_ASSERT_EQ(englishRoot.get("schemaVersion").as_int(), 1);
    const std::unordered_map<std::string, std::string> overrides = {
        { "settings.page.appearance", "Set the look of the interface, its typography, and display language." },
        { "settings.appearance.theme_palette", "THEME PRESET PALETTE" },
        { "settings.playback.frame_step_header", "FRAME SEEK STEP (CTRL + ARROW KEYS)" },
        { "settings.playback.frame_step_desc", "Number of video frames jumped per Ctrl + Left / Right arrow key" }
    };
    TEST_ASSERT_EQ(englishRoot.get("strings").as_object().size(), overrides.size());
    auto expectedCatalog = catalog;
    for (const auto& kv : overrides) {
        TEST_ASSERT_EQ(englishRoot.get("strings").get(kv.first).as_string(), kv.second);
        expectedCatalog[kv.first] = kv.second;
    }
    TEST_ASSERT(tempDir.WriteFile(L"locales\\en-US.json", englishJson));
    TEST_ASSERT(loc.SetLocale("en-US"));
    for (const auto& kv : expectedCatalog) {
        TEST_ASSERT_EQ(loc.GetUtf8(kv.first), kv.second);
        TEST_ASSERT_EQ(loc.Get(kv.first), Utf8ToUtf16(kv.second));
    }
    verifyEnglishPlurals();

    TEST_ASSERT(Localization::IsValidLocaleTag("en-US"));
    TEST_ASSERT(Localization::IsValidLocaleTag("es-ES"));
    TEST_ASSERT(Localization::IsValidLocaleTag("de-DE"));
    TEST_ASSERT(Localization::IsValidLocaleTag("zh-CN"));
    TEST_ASSERT(!Localization::IsValidLocaleTag(""));
    TEST_ASSERT(!Localization::IsValidLocaleTag(".."));
    TEST_ASSERT(!Localization::IsValidLocaleTag("../locales"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("..\\secret"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en/US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en\\US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en:US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en*"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("a_very_long_invalid_tag_that_exceeds_allowed_length_limit_by_far"));

    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("en_us"), "en-US");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("ES-es"), "es-ES");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("de"), "de-DE");

    std::wstring baseDir = L"C:\\Aperture\\locales";
    TEST_ASSERT(Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\en-US.json", baseDir));
    TEST_ASSERT(Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\sub\\de-DE.json", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\..\\secret.json", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Windows\\System32\\cmd.exe", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\en-US.json", L""));

    TEST_ASSERT_EQ(Localization::FormatPattern(L"{0} has {1} items and {2} errors.", { L"Library", L"42", L"0" }),
                   L"Library has 42 items and 0 errors.");
    TEST_ASSERT_EQ(Localization::FormatPattern(L"Use {{0}} to format '{0}'", { L"Value" }), L"Use {0} to format 'Value'");
    TEST_ASSERT_EQ(Localization::FormatPattern(L"Missing: {0}, {1}, {2}", { L"First" }), L"Missing: First, {1}, {2}");
    TEST_ASSERT_EQ(Localization::FormatPatternUtf8("Hello {0}!", { "Aperture" }), "Hello Aperture!");

    loc.SetLocale("en-US");
    TEST_ASSERT_EQ(loc.FormatPlural("plural.videos", 1), L"1 video");
    TEST_ASSERT_EQ(loc.FormatPlural("plural.videos", 5), L"5 videos");
    TEST_ASSERT_EQ(loc.FormatPlural("plural.videos", 0), L"0 videos");
    TEST_ASSERT_EQ(loc.FormatPluralUtf8("plural.files", 1), "1 file");
    TEST_ASSERT_EQ(loc.FormatPluralUtf8("plural.files", 10), "10 files");

    tempDir.WriteFile(L"locales\\fr-FR.json",
        "{\"schemaVersion\":1,\"tag\":\"fr-FR\",\"displayName\":\"Français\",\"englishName\":\"French\","
        "\"strings\":{\"nav.media\":\"Médias\",\"nav.playlists\":\"Listes de lecture\"}}");
    tempDir.WriteFile(L"locales\\es-ES.json",
        "{\"tag\":\"es-ES\",\"displayName\":\"Español\",\"englishName\":\"Spanish\",\"strings\":{}}");
    tempDir.WriteFile(L"locales\\de-DE.json",
        "{\"tag\":\"de-DE\",\"displayName\":\"Deutsch\",\"englishName\":\"German\",\"strings\":{}}");

    loc.Initialize(tempDir.GetPath(), "fr-FR");
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "fr-FR");
    TEST_ASSERT(loc.Get("nav.media") == L"M\u00e9dias");
    TEST_ASSERT_EQ(loc.Get("nav.playlists"), L"Listes de lecture");
    TEST_ASSERT_EQ(loc.Get("nav.settings"), L"Settings");
    TEST_ASSERT_EQ(loc.Get("completely.unknown.key.123"), L"completely.unknown.key.123");
    for (const auto& kv : expectedCatalog) {
        if (kv.first == "nav.media" || kv.first == "nav.playlists") continue;
        TEST_ASSERT_EQ(loc.GetUtf8(kv.first), kv.second);
        TEST_ASSERT_EQ(loc.Get(kv.first), Utf8ToUtf16(kv.second));
    }
    verifyEnglishPlurals();

    TEST_ASSERT(!loc.SetLocale("../invalid/path"));
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "en-US");
    TEST_ASSERT(loc.HadFallback());
    TEST_ASSERT(!loc.GetLastDiagnostic().empty());

    TEST_ASSERT(!loc.SetLocale("xx-YY"));
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "en-US");
    TEST_ASSERT(loc.HadFallback());

    auto discovered = loc.DiscoverLocales(tempDir.SubPath(L"locales"));
    TEST_ASSERT(discovered.size() >= 3);
    TEST_ASSERT_EQ(discovered[0].tag, "en-US");
    TEST_ASSERT_EQ(discovered[0].displayName, "English (US)");
    TEST_ASSERT_EQ(discovered[0].englishName, "English (US)");
    TEST_ASSERT_EQ(discovered[0].direction, "ltr");
    TEST_ASSERT(!loc.IsRtl());
    bool hasEs = false, hasDe = false;
    for (const auto& item : discovered) {
        if (item.tag == "es-ES") hasEs = true;
        if (item.tag == "de-DE") hasDe = true;
    }
    TEST_ASSERT(hasEs && hasDe);

    JsonValue rootLocale;
    TEST_ASSERT(JsonValue::parse("{\"schemaVersion\":14,\"locale\":\"es-ES\"}", rootLocale));
    TEST_ASSERT(Settings::MigrateSchema(rootLocale, 14, 15));
    TEST_ASSERT_EQ(rootLocale.get("appLanguage").as_string(), "es-ES");

    std::atomic<bool> startFlag{ false };
    std::atomic<int> errorCount{ 0 };
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load()) std::this_thread::yield();
            for (int i = 0; i < 400; ++i) {
                if (t == 0 && i % 50 == 0) {
                    loc.SetLocale((i % 100 == 0) ? "en-US" : "es-ES");
                } else {
                    if (loc.Get("nav.media").empty()) errorCount++;
                    if (loc.Format("osd.volume", i % 100).empty()) errorCount++;
                    if (loc.FormatPlural("plural.videos", i).empty()) errorCount++;
                }
            }
        });
    }
    startFlag.store(true);
    for (auto& w : workers) w.join();
    TEST_ASSERT_EQ(errorCount.load(), 0);
    loc.Initialize(GetExeDir(), "en-US");
}

} // namespace Aperture::Test
