#pragma once

#include "../support/TestFramework.h"
#include "../../src/RenderViewModel.h"
#include "../../src/ChapterMarkerCache.h"

namespace Aperture::Test {

TEST_CASE(RenderViewModel, ChapterSnapshotsChangeOnlyAtExactBoundaries) {
    ChapterMarkerCache cache;
    std::vector<MpvChapterInfo> chapters(4);
    // Unsorted, duplicate times and a sub-quarter-second boundary.
    chapters[0].time = 10.125; chapters[0].title = L"Later";
    chapters[1].time = 0; chapters[1].title.assign(90, L'x');
    chapters[2].time = 10.125; chapters[2].title = L"Same time";
    chapters[3].time = 50; chapters[3].title = L"Last";
    const auto initial = cache.Get(chapters, 1, 100, 1);
    TEST_ASSERT(initial == cache.Get(chapters, 1, 100, 10.1));
    TEST_ASSERT_EQ((*initial)[1].title, std::wstring(77, L'x') + L"...");
    TEST_ASSERT(!(*initial)[0].isPassed);
    TEST_ASSERT((*initial)[1].isPassed);
    const auto boundary = cache.Get(chapters, 1, 100, 10.125);
    TEST_ASSERT(boundary != initial);
    TEST_ASSERT((*boundary)[0].isPassed && (*boundary)[2].isPassed);
    TEST_ASSERT(!(*initial)[0].isPassed); // Previous frame remains immutable.
    TEST_ASSERT(boundary == cache.Get(chapters, 1, 100, 49.999));
    const auto back = cache.Get(chapters, 1, 100, std::nextafter(10.125, 0.0));
    TEST_ASSERT(!(*back)[0].isPassed && !(*back)[2].isPassed);
    TEST_ASSERT_EQ((*back)[3].normalizedPosition, 0.5f);
    const auto resized = cache.Get(chapters, 1, 200, 10);
    TEST_ASSERT_EQ((*resized)[3].normalizedPosition, 0.25f);
    chapters[0].title = L"Renamed";
    TEST_ASSERT_EQ((*cache.Get(chapters, 2, 200, 10))[0].title, std::wstring(L"Renamed"));
}

TEST_CASE(RenderViewModel, ChapterSnapshotHandlesEmptyCappedAndInvalidTimes) {
    ChapterMarkerCache cache;
    std::vector<MpvChapterInfo> chapters(501);
    for (size_t i = 0; i < chapters.size(); ++i) chapters[i].time = double(i * 10);
    const auto unknownDuration = cache.Get(chapters, 1, 0, 2);
    TEST_ASSERT(unknownDuration->empty());
    TEST_ASSERT(unknownDuration == cache.Get(chapters, 1, 0, 100));
    const auto capped = cache.Get(chapters, 1, 10000, 4995);
    TEST_ASSERT_EQ(capped->size(), size_t(500));
    TEST_ASSERT(capped == cache.Get(chapters, 1, 10000, 5005)); // Hidden 501st chapter is irrelevant.
    const auto invalid = cache.Get(chapters, 1, 10000, std::numeric_limits<double>::quiet_NaN());
    TEST_ASSERT(std::none_of(invalid->begin(), invalid->end(), [](const auto& marker) { return marker.isPassed; }));
    TEST_ASSERT((*cache.Get(chapters, 1, 10000, 5))[0].isPassed);
    const auto empty = cache.Get({}, 2, 100, 5);
    TEST_ASSERT(empty->empty());
    TEST_ASSERT(empty == cache.Get({}, 2, 100, 95));
}

TEST_CASE(RenderViewModel, FlyoutBorrowsLargeQueueForTheFrame) {
    SettingsFlyoutViewModel vm;
    TEST_ASSERT(vm.QueueItems().empty());
    std::vector<std::wstring> queue(25000, L"C:\\Media\\episode.mkv");
    vm.pQueueItems = &queue;
    SettingsFlyoutViewModel nextFrame = vm;
    TEST_ASSERT(nextFrame.QueueItems().data() == queue.data());
    TEST_ASSERT_EQ(nextFrame.QueueItems().size(), size_t(25000));
}

TEST_CASE(RenderViewModel, SettingsTabSystemActionMatching) {
    SettingsTabViewModel vm;
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(2));
    TEST_ASSERT(!vm.updateBusy);
    TEST_ASSERT(!vm.updateCanCancel);
    TEST_ASSERT(!vm.updateHasAvailable);
    TEST_ASSERT(!vm.updateIsError);
    TEST_ASSERT(!vm.updateIsUpToDate);
    TEST_ASSERT(vm.updateProgress < 0.0);
    vm.pressState = { SettingsPressState::Target::SystemAction, 2, -1 };
    TEST_ASSERT(vm.pressState.IsSystemActionPressed(2));
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(0));
    vm.pressState = { SettingsPressState::Target::OptionRow, 2, -1 };
    TEST_ASSERT(!vm.pressState.IsSystemActionPressed(2));
}

} // namespace Aperture::Test
