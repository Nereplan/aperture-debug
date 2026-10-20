#include "tests/support/TestRunner.h"
#include "tests/unit/TestLibraryQuery.h"
#include <numeric>
namespace Aperture::Test {
TEST_CASE(LibraryQuery, BoundedReplacementMatchesIndependentNumericPrefix) {
    for (int order = 0; order < 4; ++order) {
        MediaLibrary library;
        for (size_t i = 0; i < 512; ++i) {
            MediaItem item;
            // Equal secondary fields deliberately reach the original-index tie break.
            item.id = "same"; item.filePath = L"C:\\Media\\same.mkv"; item.title = L"same";
            item.durationSec = order == 0 ? double(i) : order == 1 ? double(512 - i) :
                order == 2 ? double((i * 137) % 512) : double((i * 137) % 13);
            library.mediaItems.push_back(std::move(item));
        }
        for (bool descending : {false, true}) {
            std::vector<size_t> expected(512);
            std::iota(expected.begin(), expected.end(), 0);
            std::stable_sort(expected.begin(), expected.end(), [&](size_t a, size_t b) {
                return descending ? library.mediaItems[a].durationSec > library.mediaItems[b].durationSec :
                    library.mediaItems[a].durationSec < library.mediaItems[b].durationSec;
            });
            for (size_t cap : {size_t(1),size_t(2),size_t(17),size_t(50),size_t(64),size_t(127),size_t(128),size_t(513)}) {
                LibraryQueryOptions options;
                options.sortField = LibrarySortField::Duration;
                options.sortDirection = descending ? SortDirection::Descending : SortDirection::Ascending;
                options.pinLastWatched = false; options.checkFileExists = false; options.maxItems = cap;
                LibraryQueryEngine engine;
                const auto& result = engine.QueryView(library, options);
                TEST_ASSERT_EQ(result.filteredCount, size_t(512));
                TEST_ASSERT_EQ(result.indices.size(), (std::min)(cap, size_t(512)));
                for (size_t i = 0; i < result.indices.size(); ++i) TEST_ASSERT_EQ(result.indices[i], expected[i]);
            }
        }
    }
}
}
int main(int argc, char** argv) { return Aperture::Test::RunRegisteredTests(argc, argv, "Bounded query selection"); }
