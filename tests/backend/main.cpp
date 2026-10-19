// Behavioral backend gate: actual stores, filesystem operations and allocations.
// The runner places this executable in a disposable directory, beside every write.
#include "../support/TestRunner.h"
#include "Settings.h"
#include "MediaLibrary.h"
#include "../benchmark/AllocationProbe.h"
#include <filesystem>
#include <locale>
#include <random>

using namespace Aperture;
namespace fs = std::filesystem;

static std::wstring Root(const wchar_t* name) {
    const auto path = GetExeDir() + L"\\" + name;
    fs::create_directories(path + L"\\data");
    return path;
}
static void Write(const std::wstring& path, const std::string& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    TEST_ASSERT(AtomicWriteTextFile(path, bytes).is_ok());
}
static JsonValue Read(const std::wstring& path) {
    std::string text;
    TEST_ASSERT(ReadTextFile(path, text).is_ok());
    JsonValue parsed;
    TEST_ASSERT(JsonValue::parse(text, parsed));
    return parsed;
}

TEST_CASE(BackendStores, SettingsDefaultsCustomBudgetsAndExtensionsSurviveRestart) {
    Settings fresh;
    TEST_ASSERT(fresh.Load().is_ok());
    const CacheBudgetConfig defaults;
    TEST_ASSERT_EQ(fresh.GetCacheBudgets().totalCeilingBytes, defaults.totalCeilingBytes);
    fresh.ResetToDefaults();
    TEST_ASSERT_EQ(fresh.GetCacheBudgets().thumbnailBudgetBytes, defaults.thumbnailBudgetBytes);
    TEST_ASSERT_EQ(GetGlobalCacheBudgets().totalCeilingBytes, defaults.totalCeilingBytes);
    TEST_ASSERT(fresh.Save().is_ok());
    auto saved = Read(GetDataDir() + L"\\settings.json");
    TEST_ASSERT_EQ(saved.get("cacheTotalBudgetMB").as_int(), 160);
    saved["cacheTotalBudgetMB"] = 240;
    saved["cacheThumbnailBudgetMB"] = 96;
    saved["cacheShaderBudgetMB"] = 64;
    saved["cacheSubtitleBudgetMB"] = 32;
    saved["cacheGenericBudgetMB"] = 48;
    saved["extension"]["nested"] = "retained";
    saved["shortcuts"]["togglePlay"]["plugin"] = "shortcut-extension";
    saved["shortcuts"]["pluginAction"]["vk"] = 88;
    saved["shortcuts"]["pluginAction"]["plugin"] = "unknown-action";
    Write(GetDataDir() + L"\\settings.json", saved.stringify());
    Settings reloaded;
    TEST_ASSERT(reloaded.Load().is_ok());
    TEST_ASSERT_EQ(reloaded.cacheTotalBudgetMB, 240);
    reloaded.volume = 0.625;
    TEST_ASSERT(reloaded.Save().is_ok());
    Settings restarted;
    TEST_ASSERT(restarted.Load().is_ok());
    TEST_ASSERT_EQ(restarted.volume, 0.625);
    TEST_ASSERT_EQ(restarted.cacheThumbnailBudgetMB, 96);
    const auto result = Read(GetDataDir() + L"\\settings.json");
    TEST_ASSERT_EQ(result.get("extension").get("nested").as_string(), "retained");
    TEST_ASSERT_EQ(result.get("shortcuts").get("togglePlay").get("plugin").as_string(), "shortcut-extension");
    TEST_ASSERT_EQ(result.get("shortcuts").get("pluginAction").get("plugin").as_string(), "unknown-action");
}

TEST_CASE(BackendStores, NestedPreferencesBookmarksAndUnknownFieldsSurviveSaveAndRelocation) {
    const auto root = Root(L"nested");
    Write(root + L"\\data\\library.json", R"({"schemaVersion":1,"items":[
      {"id":"a","filePath":".\\media\\日本語.mkv","title":"Café","durationSec":600.25,
       "customMediaRating":5,"preferences":{"playbackSpeed":1.5,"aspectRatio":"16:9",
        "audioTrackId":2,"audioTrackTitle":"Japanese","audioTrackLang":"jpn",
        "subtitleTrackId":3,"subtitleTrackTitle":"English","subtitleTrackLang":"eng",
        "audioDelayMs":-150,"subDelayMs":250,"subtitlesVisible":false,
        "extension":{"values":[1,"two",null]}},
       "bookmarks":[{"id":"b","timeSec":125.5,"label":"Scene","createdTimestamp":1690000000,
        "color":"#00FF88","nested":{"value":42}}],
       "overrides":{"title":"Override","year":2024,"series":"Series","plugin":"override-data"}},
      {"id":"bare","filePath":".\\media\\bare.mp4","title":"Bare"},
      {"id":"invalid","filePath":".\\media\\invalid.mp4","preferences":{
        "playbackSpeed":999,"aspectRatio":"invalid","audioDelayMs":999999,"subDelayMs":-999999}}
    ]})");
    Write(root + L"\\data\\playlists.json", R"([{"id":"p","name":"List","itemPaths":[".\\media\\日本語.mkv"],"curator":"retained"}])");
    Write(root + L"\\data\\history.json", R"([{"filePath":".\\media\\日本語.mkv","title":"Recent","lastPosSec":12.5,"device":"portable"}])");
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(library.mediaItems.size(), size_t(3));
    auto& item = library.mediaItems.at(0);
    TEST_ASSERT(item.preferences.has_value());
    TEST_ASSERT_EQ(item.preferences->playbackSpeed.value(), 1.5);
    TEST_ASSERT_EQ(item.preferences->aspectRatio.value(), "16:9");
    TEST_ASSERT_EQ(item.preferences->audioTrackId.value(), 2);
    TEST_ASSERT_EQ(item.preferences->audioTrackTitle.value(), L"Japanese");
    TEST_ASSERT_EQ(item.preferences->audioTrackLang.value(), "jpn");
    TEST_ASSERT_EQ(item.preferences->subtitleTrackId.value(), 3);
    TEST_ASSERT_EQ(item.preferences->subtitleTrackTitle.value(), L"English");
    TEST_ASSERT_EQ(item.preferences->subtitleTrackLang.value(), "eng");
    TEST_ASSERT_EQ(item.preferences->audioDelayMs.value(), -150);
    TEST_ASSERT_EQ(item.preferences->subDelayMs.value(), 250);
    TEST_ASSERT(!item.preferences->subtitlesVisible.value());
    TEST_ASSERT(!library.mediaItems.at(1).preferences.has_value());
    const auto& invalid = library.mediaItems.at(2).preferences;
    TEST_ASSERT(!invalid || !invalid->HasAnyPreference());
    item.preferences->playbackSpeed = 1.75;
    item.bookmarks.at(0).label = L"Changed 日本語";
    TEST_ASSERT(library.Save().is_ok());
    const auto disk = Read(root + L"\\data\\library.json");
    TEST_ASSERT(disk.is_array());
    TEST_ASSERT_EQ(disk.as_array().size(), size_t(3));
    TEST_ASSERT_EQ(disk[1].get("id").as_string(), "bare");
    TEST_ASSERT(!disk[1].has_key("preferences"));
    const auto relocated = Root(L"relocated");
    fs::copy(root + L"\\data", relocated + L"\\data", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    MediaLibrary restarted;
    TEST_ASSERT(restarted.LoadFromRoot(relocated).is_ok());
    const auto& actual = restarted.mediaItems.at(0);
    TEST_ASSERT_EQ(actual.filePath, relocated + L"\\media\\日本語.mkv");
    TEST_ASSERT_EQ(actual.preferences->playbackSpeed.value(), 1.75);
    TEST_ASSERT_EQ(actual.preferences->aspectRatio.value(), "16:9");
    TEST_ASSERT_EQ(actual.preferences->audioTrackId.value(), 2);
    TEST_ASSERT_EQ(actual.preferences->audioTrackTitle.value(), L"Japanese");
    TEST_ASSERT_EQ(actual.preferences->audioTrackLang.value(), "jpn");
    TEST_ASSERT_EQ(actual.preferences->subtitleTrackId.value(), 3);
    TEST_ASSERT_EQ(actual.preferences->subtitleTrackTitle.value(), L"English");
    TEST_ASSERT_EQ(actual.preferences->subtitleTrackLang.value(), "eng");
    TEST_ASSERT_EQ(actual.preferences->audioDelayMs.value(), -150);
    TEST_ASSERT_EQ(actual.preferences->subDelayMs.value(), 250);
    TEST_ASSERT(!actual.preferences->subtitlesVisible.value());
    TEST_ASSERT_EQ(actual.preferences->extraFields.get("extension").get("values")[1].as_string(), "two");
    TEST_ASSERT_EQ(actual.extraFields.get("customMediaRating").as_int(), 5);
    TEST_ASSERT_EQ(actual.bookmarks.at(0).timeSec, 125.5);
    TEST_ASSERT_EQ(actual.bookmarks.at(0).id, "b");
    TEST_ASSERT_EQ(actual.bookmarks.at(0).createdTimestamp, int64_t(1690000000));
    TEST_ASSERT_EQ(actual.bookmarks.at(0).label, L"Changed 日本語");
    TEST_ASSERT_EQ(actual.bookmarks.at(0).extraFields.get("nested").get("value").as_int(), 42);
    TEST_ASSERT_EQ(actual.bookmarks.at(0).extraFields.get("color").as_string(), "#00FF88");
    TEST_ASSERT_EQ(actual.overrides->title.value(), L"Override");
    TEST_ASSERT_EQ(actual.overrides->year.value(), 2024);
    TEST_ASSERT_EQ(actual.overrides->series.value(), L"Series");
    TEST_ASSERT_EQ(actual.overrides->extraFields.get("plugin").as_string(), "override-data");
    TEST_ASSERT_EQ(restarted.playlists.at(0).extraFields.get("curator").as_string(), "retained");
    TEST_ASSERT_EQ(restarted.history.at(0).extraFields.get("device").as_string(), "portable");
    TEST_ASSERT(restarted.Save().is_ok());
}

TEST_CASE(BackendStores, HistoryKeepsMostRecentHundredValidUniqueEntries) {
    const auto root = Root(L"history");
    JsonValue records = JsonValue::Array();
    for (int i = 0; i < 3000; ++i) {
        JsonValue record = JsonValue::Object();
        record["filePath"] = i % 9 == 0 ? "https://invalid.test/video" : ".\\media\\" + std::to_string(i / 2) + ".mkv";
        record["title"] = std::to_string(i / 2);
        record["watchedTimestamp"] = i / 2;
        records.push_back(std::move(record));
    }
    Write(root + L"\\data\\history.json", records.stringify());
    MediaLibrary library;
    TEST_ASSERT(library.LoadFromRoot(root).is_ok());
    TEST_ASSERT_EQ(library.history.size(), size_t(100));
    for (int i = 0; i < 100; ++i) TEST_ASSERT_EQ(library.history.at(i).title, std::to_wstring(1499 - i));
    TEST_ASSERT(library.Save().is_ok());
    TEST_ASSERT_EQ(Read(root + L"\\data\\history.json").as_array().size(), size_t(100));
}

TEST_CASE(BackendJson, NumericFormattingMatchesClassicStoreFormat) {
    std::mt19937_64 random(20261015);
    for (int i = 0; i < 10000; ++i) {
        const double value = std::ldexp(static_cast<double>(static_cast<int64_t>(random())), -(i % 110));
        std::ostringstream oracle;
        oracle.imbue(std::locale::classic());
        if (std::floor(value) == value && value >= -std::ldexp(1.0, 63) && value < std::ldexp(1.0, 63))
            oracle << static_cast<int64_t>(value);
        else oracle << value;
        TEST_ASSERT_EQ(JsonValue(value).stringify(), oracle.str());
    }
    TEST_ASSERT_EQ(JsonValue(std::ldexp(1.0, 63)).stringify(), "9.22337e+18");
    TEST_ASSERT_EQ(JsonValue(-std::ldexp(1.0, 63)).stringify(), "-9223372036854775808");
    TEST_ASSERT_EQ(JsonValue(std::numeric_limits<double>::infinity()).stringify(), "null");
    TEST_ASSERT_EQ(JsonValue(std::numeric_limits<double>::denorm_min()).stringify(), "4.94066e-324");
}

TEST_CASE(BackendJson, LocaleCannotCorruptNumbersAndFormattingNeedsNoHeap) {
    struct Comma : std::numpunct<char> { char do_decimal_point() const override { return ','; } };
    struct RestoreLocale { std::locale saved = std::locale(); ~RestoreLocale() { std::locale::global(saved); } } restore;
    std::locale::global(std::locale(std::locale::classic(), new Comma));
    TEST_ASSERT_EQ(JsonValue(123.125).stringify(), "123.125");
    std::string output;
    output.reserve(32);
    stats = {};
    measuring = true;
    for (int i = 0; i < 10000; ++i) {
        output.clear();
        JsonValue(i + 0.125).append_to(output);
    }
    measuring = false;
    const auto allocations = stats.calls;
    TEST_ASSERT_EQ(output, "9999.12");
    TEST_ASSERT_EQ(allocations, size_t(0));
}

TEST_CASE(BackendCache, ScanClassifiesNestedFilesAndHonorsLimits) {
    const auto root = Root(L"scan");
    const std::vector<std::pair<std::wstring, CacheClass>> directories = {
        {L"thumbnails", CacheClass::Thumbnails}, {L"SHADERS\\nested", CacheClass::Shaders},
        {L"subtitles\\日本語", CacheClass::Subtitles}, {L"generic", CacheClass::Generic},
        {L"thumbnails-extra", CacheClass::Generic}, {L"loose", CacheClass::Generic}};
    uint64_t expectedBytes = 0;
    for (size_t i = 0; i < directories.size(); ++i) for (int n = 0; n < 20; ++n) {
        Write(root + L"\\cache\\" + directories[i].first + L"\\" + std::to_wstring(n) + L".bin", std::string(i + 1, 'x'));
        expectedBytes += i + 1;
    }
    Write(root + L"\\data\\protected.json", "protected");
    const auto files = ScanCacheFiles(root);
    TEST_ASSERT_EQ(files.size(), size_t(120));
    for (const auto& file : files) {
        bool found = false;
        for (const auto& dir : directories) if (IsPathUnderRoot(file.path, root + L"\\cache\\" + dir.first)) {
            TEST_ASSERT_EQ(file.cacheClass, dir.second); found = true; break;
        }
        TEST_ASSERT(found);
    }
    const auto usage = QueryCacheStats(root);
    TEST_ASSERT_EQ(usage.totalBytes, expectedBytes);
    TEST_ASSERT_EQ(usage.totalFiles, size_t(120));
    TEST_ASSERT_EQ(ScanCacheFiles(root, CacheClass::Shaders).size(), size_t(20));
    TEST_ASSERT_EQ(ScanCacheFiles(root, std::nullopt, 7).size(), size_t(7));
    TEST_ASSERT(ScanCacheFiles(root, std::nullopt, 0).empty());
}

TEST_CASE(BackendCache, ScanningAndCleanupNeverFollowDirectoryLinks) {
    const auto root = GetExeDir();
    TEST_ASSERT(ScanCacheFiles(root + L"\\root-link").empty());
    TEST_ASSERT(ScanCacheFiles(root + L"\\root-link", CacheClass::Shaders).empty());
    TEST_ASSERT(ScanCacheFiles(root + L"\\class-link", CacheClass::Shaders).empty());
    TEST_ASSERT(ScanCacheFiles(root + L"\\nested-link").empty());
    TEST_ASSERT(!ClearDisposableCache(root + L"\\root-link").is_ok());
    TEST_ASSERT(ClearDisposableCache(root + L"\\class-link", CacheClass::Shaders).is_ok());
    TEST_ASSERT(ClearDisposableCache(root + L"\\nested-link").is_ok());
    std::string kept;
    TEST_ASSERT(ReadTextFile(root + L"\\link-target\\keep.bin", kept).is_ok());
    TEST_ASSERT_EQ(kept, "untouched");
}

TEST_CASE(BackendCache, RealWritesRespectBudgetsReplacementAndLockedFiles) {
    const auto root = Root(L"eviction");
    CacheBudgetConfig budget;
    budget.totalCeilingBytes = 512;
    for (auto c : {CacheClass::Thumbnails, CacheClass::Shaders, CacheClass::Subtitles, CacheClass::Generic}) budget.SetBudget(c, 512);
    TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, L"locked.bin", std::string(128, 'l'), budget, root).is_ok());
    struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } locked{
        CreateFileW((root + L"\\cache\\generic\\locked.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    TEST_ASSERT(locked.value != INVALID_HANDLE_VALUE);
    for (int i = 0; i < 30; ++i) {
        const auto name = std::to_wstring(i % 5) + L".bin";
        const std::string payload(64 + (i % 3) * 32, static_cast<char>('A' + i % 26));
        TEST_ASSERT(AtomicWriteCacheFile(CacheClass::Generic, name, payload, budget, root).is_ok());
        std::string actual;
        TEST_ASSERT(ReadCacheFile(CacheClass::Generic, name, actual, root).is_ok());
        TEST_ASSERT_EQ(actual, payload);
        uint64_t diskBytes = 0;
        for (const auto& file : fs::recursive_directory_iterator(root + L"\\cache")) if (file.is_regular_file()) diskBytes += file.file_size();
        TEST_ASSERT(diskBytes <= budget.totalCeilingBytes);
        TEST_ASSERT_EQ(QueryCacheStats(root).totalBytes, diskBytes);
    }
    TEST_ASSERT(!AtomicWriteCacheFile(CacheClass::Generic, L"oversized.bin", std::string(513, 'x'), budget, root).is_ok());
    TEST_ASSERT(!AtomicWriteCacheFile(CacheClass::Generic, L"..\\..\\data\\escape.json", "escape", budget, root).is_ok());
    TEST_ASSERT(!FileExists(root + L"\\data\\escape.json"));
    TEST_ASSERT(FileExists(root + L"\\cache\\generic\\locked.bin"));
}

TEST_CASE(BackendStores, CorruptStoreLeavesIndependentStoresAvailable) {
    const wchar_t* roots[] = {L"partial-library", L"partial-playlists", L"partial-history"};
    const wchar_t* files[] = {L"library.json", L"playlists.json", L"history.json"};
    const char* contents[] = {
        R"([{"id":"media","filePath":".\\movie.mp4","title":"Movie","durationSec":120}])",
        R"([{"id":"playlist","name":"Favorites","itemPaths":[".\\movie.mp4"]}])",
        R"([{"filePath":".\\movie.mp4","title":"Movie","lastPosSec":10,"durationSec":120,"watchedTimestamp":1788001000}])"
    };
    for (size_t corrupt = 0; corrupt < 3; ++corrupt) {
        const auto root = Root(roots[corrupt]);
        for (size_t store = 0; store < 3; ++store)
            Write(root + L"\\data\\" + files[store], store == corrupt ? "{corrupt" : contents[store]);
        MediaLibrary library;
        TEST_ASSERT(!library.LoadFromRoot(root).is_ok());
        TEST_ASSERT_EQ(library.mediaItems.size(), corrupt == 0 ? size_t(0) : size_t(1));
        TEST_ASSERT_EQ(library.playlists.size(), size_t(1));
        TEST_ASSERT_EQ(library.playlists[0].id, corrupt == 1 ? "pl_favorites" : "playlist");
        TEST_ASSERT_EQ(library.history.size(), corrupt == 2 ? size_t(0) : size_t(1));
        const auto status = library.GetLastLoadResult();
        TEST_ASSERT_EQ(status.libraryStatus.is_ok(), corrupt != 0);
        TEST_ASSERT_EQ(status.playlistsStatus.is_ok(), corrupt != 1);
        TEST_ASSERT_EQ(status.historyStatus.is_ok(), corrupt != 2);
    }
}

int main(int argc, char** argv) {
    if (!FileExists(GetExeDir() + L"\\.backend-test-root")) {
        std::cerr << "Refusing to write outside an isolated backend test installation.\n";
        return 2;
    }
    return Aperture::Test::RunRegisteredTests(argc, argv, "Aperture backend behavior and resource contracts");
}
