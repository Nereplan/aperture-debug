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
#include <unordered_set>

namespace Aperture::Test {

TEST_CASE(Localization, CanonicalEnglishCatalogCompleteness) {
    const auto& catalog = Localization::GetCanonicalEnglishCatalog();
    TEST_ASSERT(!catalog.empty());
    TEST_ASSERT(catalog.size() >= 100);

    for (const auto& kv : catalog) {
        TEST_ASSERT(!kv.first.empty());
        TEST_ASSERT(!kv.second.empty());
    }

    const auto& plurals = Localization::GetCanonicalEnglishPlurals();
    TEST_ASSERT(!plurals.empty());
    for (const auto& kv : plurals) {
        TEST_ASSERT(!kv.first.empty());
        TEST_ASSERT(!kv.second.other.empty() || !kv.second.one.empty());
    }
}

TEST_CASE(Localization, TagValidationAndNormalization) {
    TEST_ASSERT(Localization::IsValidLocaleTag("en-US"));
    TEST_ASSERT(Localization::IsValidLocaleTag("es-ES"));
    TEST_ASSERT(Localization::IsValidLocaleTag("de-DE"));
    TEST_ASSERT(Localization::IsValidLocaleTag("en"));
    TEST_ASSERT(Localization::IsValidLocaleTag("fr-FR"));
    TEST_ASSERT(Localization::IsValidLocaleTag("pt-BR"));
    TEST_ASSERT(Localization::IsValidLocaleTag("zh-CN"));

    // Traversal and invalid tag rejections
    TEST_ASSERT(!Localization::IsValidLocaleTag(""));
    TEST_ASSERT(!Localization::IsValidLocaleTag(".."));
    TEST_ASSERT(!Localization::IsValidLocaleTag("../locales"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("..\\secret"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en/US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en\\US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en:US"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("en*"));
    TEST_ASSERT(!Localization::IsValidLocaleTag("a_very_long_invalid_tag_that_exceeds_allowed_length_limit_by_far"));

    // Normalization
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("en_us"), "en-US");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("EN-us"), "en-US");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("ES-es"), "es-ES");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("es_ES"), "es-ES");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("en"), "en-US");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("es"), "es-ES");
    TEST_ASSERT_EQ(Localization::NormalizeLocaleTag("de"), "de-DE");
}

TEST_CASE(Localization, PathSafetyAndContainment) {
    std::wstring baseDir = L"C:\\Aperture\\locales";
    TEST_ASSERT(Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\en-US.json", baseDir));
    TEST_ASSERT(Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\es-ES.json", baseDir));
    TEST_ASSERT(Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\sub\\de-DE.json", baseDir));

    // Traversal attempts
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\..\\secret.json", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Windows\\System32\\cmd.exe", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"", baseDir));
    TEST_ASSERT(!Localization::IsSafeLocalePath(L"C:\\Aperture\\locales\\en-US.json", L""));
}

TEST_CASE(Localization, PlaceholderFormatting) {
    std::wstring p1 = L"Hello {0}!";
    TEST_ASSERT_EQ(Localization::FormatPattern(p1, { L"World" }), L"Hello World!");

    std::wstring p2 = L"{0} has {1} items and {2} errors.";
    TEST_ASSERT_EQ(Localization::FormatPattern(p2, { L"Library", L"42", L"0" }), L"Library has 42 items and 0 errors.");

    // Escaped braces
    std::wstring p3 = L"Use {{0}} to format '{0}'";
    TEST_ASSERT_EQ(Localization::FormatPattern(p3, { L"Value" }), L"Use {0} to format 'Value'");

    // Missing arguments (safe fallback, no crash, placeholder retained)
    std::wstring p4 = L"Missing: {0}, {1}, {2}";
    TEST_ASSERT_EQ(Localization::FormatPattern(p4, { L"First" }), L"Missing: First, {1}, {2}");

    // UTF-8 pattern formatting
    std::string u1 = "Hello {0}!";
    TEST_ASSERT_EQ(Localization::FormatPatternUtf8(u1, { "Aperture" }), "Hello Aperture!");
}

TEST_CASE(Localization, PluralFormatting) {
    auto& loc = Localization::Instance();
    loc.SetLocale("en-US");

    std::wstring oneVid = loc.FormatPlural("plural.videos", 1);
    TEST_ASSERT_EQ(oneVid, L"1 video");

    std::wstring fiveVids = loc.FormatPlural("plural.videos", 5);
    TEST_ASSERT_EQ(fiveVids, L"5 videos");

    std::wstring zeroVids = loc.FormatPlural("plural.videos", 0);
    TEST_ASSERT_EQ(zeroVids, L"0 videos");

    std::string oneFile = loc.FormatPluralUtf8("plural.files", 1);
    TEST_ASSERT_EQ(oneFile, "1 file");

    std::string multiFiles = loc.FormatPluralUtf8("plural.files", 10);
    TEST_ASSERT_EQ(multiFiles, "10 files");
}

TEST_CASE(Localization, FallbackHierarchyAndMissingKeys) {
    ScopedTempDir tempDir(L"Aperture_Loc_Test_");
    tempDir.CreateSubDir(L"locales");

    // Create a partial custom locale file
    std::string partialJson = "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"tag\": \"fr-FR\",\n"
        "  \"displayName\": \"Français\",\n"
        "  \"englishName\": \"French\",\n"
        "  \"strings\": {\n"
        "    \"nav.media\": \"Médias\",\n"
        "    \"nav.playlists\": \"Listes de lecture\"\n"
        "  }\n"
        "}";
    tempDir.WriteFile(L"locales\\fr-FR.json", partialJson);

    auto& loc = Localization::Instance();
    loc.Initialize(tempDir.GetPath(), "fr-FR");
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "fr-FR");

    // Overridden keys
    TEST_ASSERT(loc.Get("nav.media") == L"M\u00e9dias");
    TEST_ASSERT_EQ(loc.Get("nav.playlists"), L"Listes de lecture");

    // Missing key in fr-FR falls back cleanly to English
    TEST_ASSERT_EQ(loc.Get("nav.settings"), L"Settings");
    TEST_ASSERT_EQ(loc.Get("player.play"), L"Play");

    // Unknown nonexistent key returns the key itself safely
    TEST_ASSERT_EQ(loc.Get("completely.unknown.key.123"), L"completely.unknown.key.123");
}

TEST_CASE(Localization, MissingOrInvalidLocaleFallback) {
    auto& loc = Localization::Instance();

    // Invalid tag falls back cleanly to en-US
    bool ok = loc.SetLocale("../invalid/path");
    TEST_ASSERT(!ok);
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "en-US");
    TEST_ASSERT(loc.HadFallback());
    TEST_ASSERT(!loc.GetLastDiagnostic().empty());

    // Nonexistent locale falls back cleanly
    bool ok2 = loc.SetLocale("xx-YY");
    TEST_ASSERT(!ok2);
    TEST_ASSERT_EQ(loc.GetCurrentLocale(), "en-US");
    TEST_ASSERT(loc.HadFallback());
}

TEST_CASE(Localization, LocaleDiscovery) {
    ScopedTempDir tempDir(L"Aperture_Loc_Disc_");
    tempDir.CreateSubDir(L"locales");

    std::string esJson = "{\"tag\":\"es-ES\",\"displayName\":\"Español\",\"englishName\":\"Spanish\",\"strings\":{}}";
    std::string deJson = "{\"tag\":\"de-DE\",\"displayName\":\"Deutsch\",\"englishName\":\"German\",\"strings\":{}}";
    tempDir.WriteFile(L"locales\\es-ES.json", esJson);
    tempDir.WriteFile(L"locales\\de-DE.json", deJson);

    auto& loc = Localization::Instance();
    auto discovered = loc.DiscoverLocales(tempDir.SubPath(L"locales"));
    TEST_ASSERT(discovered.size() >= 3); // en-US (built-in) + es-ES + de-DE

    // en-US must always be first
    TEST_ASSERT_EQ(discovered[0].tag, "en-US");

    bool hasEs = false;
    bool hasDe = false;
    for (const auto& item : discovered) {
        if (item.tag == "es-ES") hasEs = true;
        if (item.tag == "de-DE") hasDe = true;
    }
    TEST_ASSERT(hasEs);
    TEST_ASSERT(hasDe);
}

TEST_CASE(Localization, SettingsPersistenceAndSchema15Migration) {
    // Legacy schema 14 settings JSON
    std::string v14Json = "{\n"
        "  \"schemaVersion\": 14,\n"
        "  \"themePreset\": \"cyberpunk\",\n"
        "  \"autoplay\": true\n"
        "}";

    JsonValue root;
    TEST_ASSERT(JsonValue::parse(v14Json, root));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 14);
    TEST_ASSERT(!root.has_key("appLanguage"));

    // Migrate to Schema 15
    TEST_ASSERT(Settings::MigrateSchema(root, 14, 15));
    TEST_ASSERT_EQ(root.get("schemaVersion").as_int(), 15);
    TEST_ASSERT_EQ(root.get("appLanguage").as_string(), "en-US");

    // Migration with explicit legacy "locale" field
    std::string v14LocaleJson = "{\n"
        "  \"schemaVersion\": 14,\n"
        "  \"locale\": \"es-ES\"\n"
        "}";
    JsonValue rootLocale;
    TEST_ASSERT(JsonValue::parse(v14LocaleJson, rootLocale));
    TEST_ASSERT(Settings::MigrateSchema(rootLocale, 14, 15));
    TEST_ASSERT_EQ(rootLocale.get("appLanguage").as_string(), "es-ES");
}

TEST_CASE(Localization, ConcurrentThreadSafety) {
    auto& loc = Localization::Instance();
    std::atomic<bool> startFlag{ false };
    std::atomic<int> errorCount{ 0 };

    const int kThreads = 8;
    const int kIterations = 1000;
    std::vector<std::thread> workers;

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            while (!startFlag.load()) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kIterations; ++i) {
                if (t == 0 && i % 50 == 0) {
                    loc.SetLocale((i % 100 == 0) ? "en-US" : "es-ES");
                } else {
                    std::wstring mediaStr = loc.Get("nav.media");
                    if (mediaStr.empty()) errorCount++;

                    std::wstring fmt = loc.Format("osd.volume", i % 100);
                    if (fmt.empty()) errorCount++;

                    std::wstring pl = loc.FormatPlural("plural.videos", i);
                    if (pl.empty()) errorCount++;
                }
            }
        });
    }

    startFlag.store(true);
    for (auto& w : workers) {
        w.join();
    }

    TEST_ASSERT_EQ(errorCount.load(), 0);
    // Reset back to English
    loc.SetLocale("en-US");
}

} // namespace Aperture::Test
