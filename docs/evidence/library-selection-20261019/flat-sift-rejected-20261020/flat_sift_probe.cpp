// Repeated cold bounded queries. Fixture creation, validation, destruction and hashing are outside each timed Query call.
#include "LibraryQuery.h"
#include "AllocationProbe.h"
#include <chrono>
#include <cstdio>
#include <numeric>
#include <stdexcept>

using namespace Aperture;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
uint64_t HashIndices(const std::vector<size_t>& indices) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t index : indices) { hash ^= index; hash *= 1099511628211ull; }
    return hash;
}
}
int main(int argc, char** argv) {
    try {
        const int rotation = argc == 2 ? std::atoi(argv[1]) : 0;
        Require(rotation >= 0 && rotation < 5, "Invalid mode rotation");
        DWORD_PTR processMask = 0, systemMask = 0;
        Require(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != FALSE && processMask, "Get affinity");
        const DWORD_PTR chosen = processMask & (~processMask + 1);
        Require(SetProcessAffinityMask(GetCurrentProcess(), chosen) != FALSE, "Set verified one-processor affinity");
        DWORD_PTR verified = 0;
        Require(GetProcessAffinityMask(GetCurrentProcess(), &verified, &systemMask) != FALSE && verified == chosen, "Verify affinity");
        std::fprintf(stderr, "verified_affinity_mask=%llu;mode_rotation=%d;queries_per_sample=64;warmup_samples=1;measured_samples=7\n",
            static_cast<unsigned long long>(verified), rotation);
        MediaLibrary library;
        constexpr size_t itemCount = 25000, queries = 64;
        library.mediaItems.reserve(itemCount);
        for (size_t i = 0; i < itemCount; ++i) {
            MediaItem item;
            item.id = std::to_string(i);
            item.filePath = L"C:\\Benchmark\\Series" + std::to_wstring(i / 50) + L"\\Series" +
                std::to_wstring(i / 50) + L" S01E" + std::to_wstring(i % 50 + 1) + L".mkv";
            item.title = GetFileTitleFromPath(item.filePath);
            item.durationSec = 1200;
            library.mediaItems.push_back(std::move(item));
        }
        std::puts("mode,sample,queries,sum_query_ms,mean_query_ms,allocation_calls,allocated_bytes,peak_live_cpp_bytes,items,filtered_count,order_hash");
        for (int step = 0; step < 5; ++step) {
            const int mode = (step + rotation) % 5;
            LibraryQueryOptions options;
            options.maxItems = mode == 3 ? 1023 : 50;
            options.pinLastWatched = false;
            options.checkFileExists = false;
            options.sortField = mode == 0 ? LibrarySortField::Title : LibrarySortField::Duration;
            for (size_t i = 0; i < itemCount; ++i) {
                library.mediaItems[i].durationSec = mode == 1 ? double((i * 193) % itemCount) + 0.5 :
                    (mode == 2 || mode == 3 ? double(itemCount - i) + 0.5 : double(i) + 0.5);
            }
            std::vector<size_t> expected(itemCount);
            std::iota(expected.begin(), expected.end(), 0);
            std::sort(expected.begin(), expected.end(), [&](size_t a, size_t b) {
                if (mode == 0) {
                    const int comparison = CompareStringOrdinal(library.mediaItems[a].title.c_str(), -1,
                        library.mediaItems[b].title.c_str(), -1, TRUE);
                    return comparison == CSTR_LESS_THAN || (comparison == CSTR_EQUAL && a < b);
                }
                const double left = library.mediaItems[a].durationSec, right = library.mediaItems[b].durationSec;
                return left < right || (left == right && a < b);
            });
            expected.resize(options.maxItems);
            const uint64_t expectedHash = HashIndices(expected);
            for (int sample = 0; sample < 8; ++sample) {
                long long elapsedNs = 0;
                size_t calls = 0, bytes = 0, peak = 0, firstCalls = 0, firstBytes = 0;
                for (size_t query = 0; query < queries; ++query) {
                    Require(stats.live == 0, "Previous cold query allocations were released");
                    stats = {};
                    {
                        LibraryQueryEngine engine;
                        measuring = sample > 0;
                        const auto start = std::chrono::steady_clock::now();
                        if (mode == 0) {
                            const auto& result = engine.Query(library, options);
                            const auto end = std::chrono::steady_clock::now();
                            measuring = false;
                            elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            Require(result.items.size() == expected.size() && result.filteredCount == itemCount, "Projection result count");
                            for (size_t i = 0; i < expected.size(); ++i)
                                Require(result.items[i].originalLibraryIndex == expected[i], "Projection independent order oracle");
                        } else {
                            const auto& result = engine.QueryView(library, options);
                            const auto end = std::chrono::steady_clock::now();
                            measuring = false;
                            elapsedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                            Require(result.indices == expected && result.filteredCount == itemCount && result.isCapped, "Compact-view independent order oracle");
                            Require(HashIndices(result.indices) == expectedHash, "Compact-view hash");
                        }
                        if (!query) { firstCalls = stats.calls; firstBytes = stats.bytes; }
                        Require(stats.calls == firstCalls && stats.bytes == firstBytes, "Stable per-query allocation counts");
                        calls += stats.calls; bytes += stats.bytes; peak = (std::max)(peak, stats.peak);
                    }
                    Require(stats.live == 0, "Cold query owns no surviving measured allocation");
                }
                if (sample) std::printf("%d,%d,%zu,%.6f,%.9f,%zu,%zu,%zu,%zu,%zu,%llu\n", mode, sample, queries,
                    double(elapsedNs) / 1e6, double(elapsedNs) / (1e6 * queries), calls, bytes, peak, expected.size(), itemCount,
                    static_cast<unsigned long long>(expectedHash));
            }
        }
        return 0;
    } catch (const std::exception& error) {
        measuring = false;
        std::fprintf(stderr, "probe_error=%s\n", error.what());
        return 2;
    }
}
