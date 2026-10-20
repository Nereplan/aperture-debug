#include "MpvPlayer.h"
#include <filesystem>
#include <stdexcept>

using namespace Aperture;
namespace fs = std::filesystem;

size_t TestThumbnailPurge(const fs::path& root) {
    size_t checks = 0;
    auto check = [&](bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); };
    for (const auto& path : std::vector<std::wstring>{L"", L"C:\\Media\\movie.mkv",
        L"C:\\Medya\\\u0130stanbul \u65e5\u672c\U0001f3ac.mkv", std::wstring(1024, L'x') + L".mkv"}) {
        const std::string input = Utf16ToUtf8(path) + "|hub";
        std::wostringstream expected;
        expected << L"hub_" << std::hex << std::setfill(L'0') << std::setw(16)
            << Fnv1a64(input.data(), input.size()) << L".bin";
        check(MakeHubThumbnailCacheKey(path, 320, 180) == expected.str(), "Existing hub key changed");
    }
    auto create = [&](const fs::path& file) {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Cannot create empty cache fixture");
        CloseHandle(handle);
    };
    const std::wstring path = L"C:\\Media\\removed.mkv", live = L"C:\\Media\\retained.mkv";
    const auto profile = root.wstring();
    const fs::path dir = GetCacheDir(CacheClass::Thumbnails, profile);
    const auto target = MakeHubThumbnailCacheKey(path, 0, 0), retained = MakeHubThumbnailCacheKey(live, 0, 0);
    check(PurgeThumbnailDiskCacheForFile(path, profile) == 0, "Empty cache removed count");
    auto upperTarget = target;
    CharUpperBuffW(upperTarget.data(), static_cast<DWORD>(upperTarget.size()));
    create(dir / upperTarget); create(dir / retained); create(dir / L"unrelated.bin");
    struct Handle { HANDLE value; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } locked{
        CreateFileW((dir / target).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    check(locked.value != INVALID_HANDLE_VALUE, "Cannot lock purge fixture");
    check(PurgeThumbnailDiskCacheForFile(path, profile) == 0 && fs::exists(dir / target), "Locked thumbnail was removed");
    CloseHandle(locked.value); locked.value = INVALID_HANDLE_VALUE;
    check(PurgeThumbnailDiskCacheForFile(path, profile) == 1, "Unlocked thumbnail cannot be retried");
    check(!fs::exists(dir / target) && fs::exists(dir / retained) && fs::exists(dir / L"unrelated.bin"), "Scoped purge affected unrelated files");
    fs::create_directory(dir / target);
    check(PurgeThumbnailDiskCacheForFile(path, profile) == 0 && fs::is_directory(dir / target), "Purge changed directory entry");
    fs::remove(dir / target);
    create(dir / L"hub_0123456789abcdef.bin");
    check(PruneOrphanedHubThumbnailDiskCache({live}, profile) == 1, "Orphan hub entry remains");
    check(fs::exists(dir / retained) && fs::exists(dir / L"unrelated.bin"), "Pruning affected live or unrelated files");
    check(PurgeThumbnailDiskCacheForFile(L"", profile) == 0, "Empty path changed cache");
    PreviewThumbnailFrame frame;
    frame.filePath = path; frame.timestampSec = 65.25;
    frame.width = 32; frame.height = 18; frame.bgraPixels.assign(32 * 18 * 4, 255);
    check(SaveHubThumbnailToDiskCache(frame, CacheBudgetConfig{}, profile), "Hub writer rejected fixture");
    PreviewThumbnailFrame restored;
    check(LoadHubThumbnailFromDiskCache(path, 320, 180, restored, profile, false), "Hub reader lost saved key");
    check(restored.width == 32 && restored.height == 18 && restored.timestampSec == 65.25, "Hub roundtrip metadata changed");
    check(PurgeThumbnailDiskCacheForFile(path, profile) == 1, "Real encoded cache purge count");
    check(!LoadHubThumbnailFromDiskCache(path, 320, 180, restored, profile, false), "Hub reader resurrected removed entry");
    return checks;
}
