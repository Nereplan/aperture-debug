// Measures the production purge with warm filesystem metadata. Fixture creation,
// verification and process startup are outside the measured interval.
#include "MpvPlayer.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

using namespace Aperture;

struct Scenario {
    const wchar_t* name;
    double duration;
    size_t unrelated;
    std::vector<double> times;
};

static void CreateEntry(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create benchmark cache entry");
    CloseHandle(file);
}

static size_t CountEntries(const std::wstring& dir) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((dir + L"*.bin").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return 0;
        throw std::runtime_error("Cannot enumerate benchmark cache");
    }
    size_t count = 0;
    do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++count; } while (FindNextFileW(find, &data));
    const auto error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) throw std::runtime_error("Incomplete benchmark enumeration");
    return count;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::wstring profileRoot = argv[1];
    const int sample = _wtoi(argv[2]);
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        SetProcessAffinityMask(GetCurrentProcess(), processMask & (~processMask + 1));
    std::vector<Scenario> cases = {
        {L"empty_2h", 7200.6, 0, {}},
        {L"sparse_2h", 7200.6, 128, {0, 10, 60, 1800, 3600, 7200, 7201, 7200.6}},
        {L"populated_2h", 7200.6, 4096, {}},
        {L"dense_30m", 1800.6, 0, {}},
        {L"overflow_65s", 65.25, 128, {0, 32, 65.25, 66}},
        {L"short_20s", 20.25, 4096, {0, 10, 20.25, 21}},
    };
    for (int t = 0; t < 64; ++t) cases[2].times.push_back(t * 100.0);
    cases[2].times.insert(cases[2].times.end(), {7200.6, 7201.0});
    for (int t = 0; t < 1600; ++t) cases[3].times.push_back(double(t));
    cases[3].times.insert(cases[3].times.end(), {1800.6, 1801.0});
    const std::wstring target = L"C:\\Benchmark\\Medya\\\u0130stanbul \u65e5\u672c.mkv";
    const std::wstring other = L"C:\\Benchmark\\other.mkv";
    for (const auto& scenario : cases) {
        const auto root = profileRoot + L"\\" + scenario.name;
        const auto dir = GetCacheDir(CacheClass::Thumbnails, root) + L"\\";
        for (size_t i = 0; i < scenario.unrelated; ++i) CreateEntry(dir + MakeThumbnailCacheKey(other, double(i)));
        for (double t : scenario.times) CreateEntry(dir + MakeThumbnailCacheKey(target, t));
        if (!scenario.times.empty()) CreateEntry(dir + MakeHubThumbnailCacheKey(target, 0, 0));
        const size_t expected = scenario.times.size() + (scenario.times.empty() ? 0 : 1);
        if (CountEntries(dir) != expected + scenario.unrelated) throw std::runtime_error("Unexpected benchmark fixture contents");
        // Initialize process-local cache helpers before tracking transient heap.
        PurgeThumbnailDiskCacheForFile(L"C:\\Benchmark\\not-cached.mkv", 0, root);
        if (stats.live != 0) throw std::runtime_error("Allocation probe scope leaked");
        stats = {};
        measuring = true;
        const auto start = std::chrono::steady_clock::now();
        const size_t removed = PurgeThumbnailDiskCacheForFile(target, scenario.duration, root);
        const auto end = std::chrono::steady_clock::now();
        measuring = false;
        const auto measured = stats;
        if (removed != expected || CountEntries(dir) != scenario.unrelated)
            throw std::runtime_error("Purge count or retained cache contents differ");
        for (double t : scenario.times)
            if (FileExists(dir + MakeThumbnailCacheKey(target, t))) throw std::runtime_error("Target cache entry remains");
        for (size_t i = 0; i < scenario.unrelated; ++i)
            if (!FileExists(dir + MakeThumbnailCacheKey(other, double(i)))) throw std::runtime_error("Unrelated entry removed");
        std::printf("%ls,%d,%.6f,%zu,%zu,%zu,%zu,%zu\n", scenario.name, sample,
            std::chrono::duration<double, std::milli>(end - start).count(), measured.calls,
            measured.bytes, measured.peak, measured.live, removed);
    }
}
