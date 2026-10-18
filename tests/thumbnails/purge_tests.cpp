#include "MpvPlayer.h"
#include <filesystem>
#include <stdexcept>

using namespace Aperture;
namespace fs = std::filesystem;

size_t TestThumbnailPurge(const fs::path& root) {
    size_t checks = 0;
    auto check = [&](bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); };
    auto legacyKey = [](const std::wstring& path, const std::string& suffix, const wchar_t* prefix) {
        const std::string input = Utf16ToUtf8(path) + "|" + suffix;
        std::wostringstream out;
        out << prefix << std::hex << std::setfill(L'0') << std::setw(16)
            << Fnv1a64(input.data(), input.size()) << L".bin";
        return out.str();
    };
    for (const auto& path : std::vector<std::wstring>{L"", L"C:\\Media\\movie.mkv",
        L"C:\\Medya\\\u0130stanbul \u65e5\u672c\U0001f3ac.mkv", std::wstring(1024, L'x') + L".mkv"}) {
        for (double time : {0.0, 0.00049, 0.0005, 1.25, 65.6, 7200.0, 2147483646.0, -1.0})
            check(MakeThumbnailCacheKey(path, time) == legacyKey(path,
                std::to_string(static_cast<uint64_t>(std::llround(time * 1000.0))), L"thumb_"), "Legacy seek key changed");
        check(MakeHubThumbnailCacheKey(path, 320, 180) == legacyKey(path, "hub", L"hub_"), "Legacy hub key changed");
    }
    auto create = [&](const fs::path& file) {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Cannot create empty cache fixture");
        CloseHandle(handle);
    };
    const std::wstring path = L"C:\\Media\\removed.mkv", live = L"C:\\Media\\retained.mkv";
    // The real file operations cover both inventory and direct-deletion paths.
    for (double duration : {2.6, 7200.6}) {
        const auto profile = (root / (duration < 64 ? L"short" : L"long")).wstring();
        const fs::path dir = GetCacheDir(CacheClass::Thumbnails, profile);
        std::vector<std::wstring> keys{MakeHubThumbnailCacheKey(path, 0, 0),
            MakeThumbnailCacheKey(path, 0), MakeThumbnailCacheKey(path, 1),
            MakeThumbnailCacheKey(path, std::ceil(duration)), MakeThumbnailCacheKey(path, duration)};
        for (auto& key : keys) { CharUpperBuffW(key.data(), static_cast<DWORD>(key.size())); create(dir / key); }
        const auto lockedKey = MakeThumbnailCacheKey(path, 2);
        create(dir / lockedKey);
        struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } locked{
            CreateFileW((dir / lockedKey).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        check(locked.value != INVALID_HANDLE_VALUE, "Cannot lock purge fixture");
        std::vector<std::wstring> preserved{MakeHubThumbnailCacheKey(live, 0, 0), MakeThumbnailCacheKey(live, 1),
            MakeThumbnailCacheKey(path, std::ceil(duration) + 1), L"thumb_invalid.bin", L"thumb_0123456789abcdeg.bin"};
        for (const auto& key : preserved) create(dir / key);
        const auto directoryKey = MakeThumbnailCacheKey(path, duration > 64 ? 15 : 0.5);
        fs::create_directory(dir / directoryKey);
        check(PurgeThumbnailDiskCacheForFile(path, duration, profile) == keys.size(), "Incorrect scoped purge count");
        for (const auto& key : keys) check(!fs::exists(dir / key), "Target thumbnail remains");
        for (const auto& key : preserved) check(fs::exists(dir / key), "Unrelated or invalid name removed");
        check(fs::is_directory(dir / directoryKey), "Purge changed directory entry");
        check(fs::exists(dir / lockedKey), "Locked thumbnail was removed");
        CloseHandle(locked.value); locked.value = INVALID_HANDLE_VALUE;
        check(PurgeThumbnailDiskCacheForFile(path, duration, profile) == 1, "Locked thumbnail cannot be retried");
        check(!fs::exists(dir / lockedKey), "Retried thumbnail remains");
    }
    {
        const auto profile = (root / L"overflow").wstring();
        const fs::path dir = GetCacheDir(CacheClass::Thumbnails, profile);
        for (int t = 0; t < 128; ++t) create(dir / MakeThumbnailCacheKey(live, t));
        for (double t : {0.0, 65.25, 66.0}) create(dir / MakeThumbnailCacheKey(path, t));
        check(PurgeThumbnailDiskCacheForFile(path, 65.25, profile) == 3, "Bound overflow lost target entries");
        for (double t : {0.0, 65.25, 66.0}) check(!fs::exists(dir / MakeThumbnailCacheKey(path, t)), "Overflow target remains");
        for (int t = 0; t < 128; ++t) check(fs::exists(dir / MakeThumbnailCacheKey(live, t)), "Overflow removed unrelated entry");
    }
    {
        const auto profile = (root / L"empty").wstring();
        check(PurgeThumbnailDiskCacheForFile(path, 7200, profile) == 0, "Empty cache removed count");
        const fs::path dir = GetCacheDir(CacheClass::Thumbnails, profile);
        const auto seekKey = MakeThumbnailCacheKey(path, 0);
        create(dir / seekKey); create(dir / MakeHubThumbnailCacheKey(path, 0, 0));
        check(PurgeThumbnailDiskCacheForFile(path, std::nan(""), profile) == 1, "Unknown duration must remove only hub");
        for (double duration : {0.0, -1.0, std::numeric_limits<double>::infinity(), 2147483647.0})
            check(PurgeThumbnailDiskCacheForFile(path, duration, profile) == 0, "Invalid duration touched seek cache");
        check(fs::exists(dir / seekKey), "Unknown duration removed seek entry");
    }
    {
        const auto profile = (root / L"roundtrip").wstring();
        PreviewThumbnailFrame frame;
        frame.filePath = path; frame.timestampSec = 65.25;
        frame.width = 32; frame.height = 18; frame.bgraPixels.assign(32 * 18 * 4, 255);
        check(SaveThumbnailToDiskCache(frame, CacheBudgetConfig{}, profile), "Seek writer rejected fixture");
        check(SaveHubThumbnailToDiskCache(frame, CacheBudgetConfig{}, profile), "Hub writer rejected fixture");
        PreviewThumbnailFrame restored;
        check(LoadThumbnailFromDiskCache(path, 65.25, restored, profile, false), "Seek reader lost saved key");
        check(restored.width == 32 && restored.height == 18 && restored.timestampSec == 65.25, "Seek roundtrip metadata changed");
        check(LoadHubThumbnailFromDiskCache(path, 320, 180, restored, profile, false), "Hub reader lost saved key");
        check(PurgeThumbnailDiskCacheForFile(path, 65.25, profile) == 2, "Real encoded cache purge count");
        check(!LoadThumbnailFromDiskCache(path, 65.25, restored, profile, false), "Seek reader resurrected removed entry");
        check(!LoadHubThumbnailFromDiskCache(path, 320, 180, restored, profile, false), "Hub reader resurrected removed entry");
    }
    return checks;
}
