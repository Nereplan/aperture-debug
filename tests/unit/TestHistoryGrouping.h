#pragma once

#include "../support/TestFramework.h"
#include "../../src/RenderViewModel.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Common.h"

#include <ctime>
#include <vector>
#include <string>

namespace Aperture::Test {

TEST_CASE(HistoryGrouping, EmptyHistoryProjection) {
    std::vector<HistoryRecord> emptyHistory;
    HistoryTimelineProjection proj = ProjectHistoryTimeline(emptyHistory);

    TEST_ASSERT(proj.IsEmpty());
    TEST_ASSERT_EQ(proj.totalCount, 0ull);
    TEST_ASSERT_EQ(proj.GroupCount(), 0ull);
    TEST_ASSERT(proj.groups.empty());
    TEST_ASSERT(proj.items.empty());
    TEST_ASSERT(proj.itemIndices.empty());
}

TEST_CASE(HistoryGrouping, DeterministicTimeGrouping) {
    // Choose a fixed reference time: 2026-08-12 15:30:00
    std::tm tmRef{};
    tmRef.tm_year = 2026 - 1900;
    tmRef.tm_mon = 7; // August
    tmRef.tm_mday = 12;
    tmRef.tm_hour = 15;
    tmRef.tm_min = 30;
    tmRef.tm_sec = 0;
    int64_t refTime = static_cast<int64_t>(mktime(&tmRef));

    std::tm tmToday = tmRef;
    tmToday.tm_hour = 0;
    tmToday.tm_min = 0;
    tmToday.tm_sec = 0;
    int64_t todayStart = static_cast<int64_t>(mktime(&tmToday));
    int64_t yesterdayStart = todayStart - 86400;
    int64_t twoDaysAgo = todayStart - 2 * 86400;
    int64_t fourDaysAgo = todayStart - 4 * 86400;
    int64_t tenDaysAgo = todayStart - 10 * 86400;

    std::vector<HistoryRecord> history;

    // Item 0: Today (1 hour ago)
    HistoryRecord h0;
    h0.filePath = L"C:\\videos\\today_recent.mp4";
    h0.title = L"Today Recent";
    h0.watchedTimestamp = refTime - 3600;
    h0.lastPosSec = 120.0;
    h0.durationSec = 600.0;
    history.push_back(h0);

    // Item 1: Yesterday (afternoon)
    HistoryRecord h1;
    h1.filePath = L"C:\\videos\\yesterday_video.mp4";
    h1.title = L"Yesterday Video";
    h1.watchedTimestamp = yesterdayStart + 14 * 3600;
    h1.lastPosSec = 45.0;
    h1.durationSec = 300.0;
    history.push_back(h1);

    // Item 2: This week (2 days ago)
    HistoryRecord h2;
    h2.filePath = L"C:\\videos\\this_week_video1.mp4";
    h2.title = L"This Week Video 1";
    h2.watchedTimestamp = twoDaysAgo + 10 * 3600;
    h2.lastPosSec = 90.0;
    h2.durationSec = 400.0;
    history.push_back(h2);

    // Item 3: Older (10 days ago)
    HistoryRecord h3;
    h3.filePath = L"C:\\videos\\older_video.mp4";
    h3.title = L"Older Video";
    h3.watchedTimestamp = tenDaysAgo;
    h3.lastPosSec = 15.0;
    h3.durationSec = 200.0;
    history.push_back(h3);

    // Item 4: This week (4 days ago)
    HistoryRecord h4;
    h4.filePath = L"C:\\videos\\this_week_video2.mp4";
    h4.title = L"This Week Video 2";
    h4.watchedTimestamp = fourDaysAgo + 8 * 3600;
    h4.lastPosSec = 180.0;
    h4.durationSec = 500.0;
    history.push_back(h4);

    // Item 5: Undated (timestamp = 0) -> should be Older
    HistoryRecord h5;
    h5.filePath = L"C:\\videos\\undated_video.mp4";
    h5.title = L"Undated Video";
    h5.watchedTimestamp = 0;
    h5.lastPosSec = 0.0;
    h5.durationSec = 100.0;
    history.push_back(h5);

    // Item 6: Today (earlier today at 09:00)
    HistoryRecord h6;
    h6.filePath = L"C:\\videos\\today_morning.mp4";
    h6.title = L"Today Morning";
    h6.watchedTimestamp = todayStart + 9 * 3600;
    h6.lastPosSec = 30.0;
    h6.durationSec = 150.0;
    history.push_back(h6);

    HistoryTimelineProjectionOptions options;
    options.nowTimestamp = refTime;
    HistoryTimelineProjection proj = ProjectHistoryTimeline(history, options);

    TEST_ASSERT(!proj.IsEmpty());
    TEST_ASSERT_EQ(proj.totalCount, 7ull);
    TEST_ASSERT_EQ(proj.GroupCount(), 4ull);

    // Group 0: Today
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Today"));
    TEST_ASSERT_EQ(proj.groups[0].count, 2ull);
    TEST_ASSERT_EQ(proj.groups[0].countText, std::wstring(L"2 items"));
    TEST_ASSERT_EQ(proj.groups[0].startIndex, 0ull);
    TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Today Recent"));
    TEST_ASSERT_EQ(proj.itemIndices[0], 0ull);
    TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Today Morning"));
    TEST_ASSERT_EQ(proj.itemIndices[1], 6ull);

    // Group 1: Yesterday
    TEST_ASSERT_EQ(proj.groups[1].title, std::wstring(L"Yesterday"));
    TEST_ASSERT_EQ(proj.groups[1].count, 1ull);
    TEST_ASSERT_EQ(proj.groups[1].countText, std::wstring(L"1 item"));
    TEST_ASSERT_EQ(proj.groups[1].startIndex, 2ull);
    TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Yesterday Video"));
    TEST_ASSERT_EQ(proj.itemIndices[2], 1ull);

    // Group 2: This week
    TEST_ASSERT_EQ(proj.groups[2].title, std::wstring(L"This week"));
    TEST_ASSERT_EQ(proj.groups[2].count, 2ull);
    TEST_ASSERT_EQ(proj.groups[2].countText, std::wstring(L"2 items"));
    TEST_ASSERT_EQ(proj.groups[2].startIndex, 3ull);
    TEST_ASSERT_EQ(proj.items[3].title, std::wstring(L"This Week Video 1"));
    TEST_ASSERT_EQ(proj.itemIndices[3], 2ull);
    TEST_ASSERT_EQ(proj.items[4].title, std::wstring(L"This Week Video 2"));
    TEST_ASSERT_EQ(proj.itemIndices[4], 4ull);

    // Group 3: Older
    TEST_ASSERT_EQ(proj.groups[3].title, std::wstring(L"Older"));
    TEST_ASSERT_EQ(proj.groups[3].count, 2ull);
    TEST_ASSERT_EQ(proj.groups[3].countText, std::wstring(L"2 items"));
    TEST_ASSERT_EQ(proj.groups[3].startIndex, 5ull);
    TEST_ASSERT_EQ(proj.items[5].title, std::wstring(L"Older Video"));
    TEST_ASSERT_EQ(proj.itemIndices[5], 3ull);
    TEST_ASSERT_EQ(proj.items[6].title, std::wstring(L"Undated Video"));
    TEST_ASSERT_EQ(proj.itemIndices[6], 5ull);
}

TEST_CASE(HistoryGrouping, StableRecencyOrderingAndEqualTimestamps) {
    int64_t refTime = 1750000000;
    std::vector<HistoryRecord> history;

    // Three records with the exact same timestamp
    for (int i = 0; i < 3; ++i) {
        HistoryRecord h;
        h.filePath = L"C:\\videos\\same_time_" + std::to_wstring(i) + L".mp4";
        h.title = L"Item " + std::to_wstring(i);
        h.watchedTimestamp = refTime;
        h.lastPosSec = 10.0 * i;
        history.push_back(h);
    }

    HistoryTimelineProjectionOptions options;
    options.nowTimestamp = refTime;
    HistoryTimelineProjection proj = ProjectHistoryTimeline(history, options);

    TEST_ASSERT_EQ(proj.totalCount, 3ull);
    TEST_ASSERT_EQ(proj.GroupCount(), 1ull);
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Today"));
    TEST_ASSERT_EQ(proj.groups[0].count, 3ull);

    // Original insertion order must be preserved when timestamps match
    TEST_ASSERT_EQ(proj.itemIndices[0], 0ull);
    TEST_ASSERT_EQ(proj.itemIndices[1], 1ull);
    TEST_ASSERT_EQ(proj.itemIndices[2], 2ull);
    TEST_ASSERT_EQ(proj.items[0].title, std::wstring(L"Item 0"));
    TEST_ASSERT_EQ(proj.items[1].title, std::wstring(L"Item 1"));
    TEST_ASSERT_EQ(proj.items[2].title, std::wstring(L"Item 2"));
}

TEST_CASE(HistoryGrouping, BoundaryTimestampsAccuracy) {
    std::tm tmRef{};
    tmRef.tm_year = 2026 - 1900;
    tmRef.tm_mon = 5; // June
    tmRef.tm_mday = 10;
    tmRef.tm_hour = 12;
    tmRef.tm_min = 0;
    tmRef.tm_sec = 0;
    int64_t refTime = static_cast<int64_t>(mktime(&tmRef));

    std::tm tmToday = tmRef;
    tmToday.tm_hour = 0;
    tmToday.tm_min = 0;
    tmToday.tm_sec = 0;
    int64_t todayStart = static_cast<int64_t>(mktime(&tmToday));
    int64_t yesterdayStart = todayStart - 86400;
    int64_t thisWeekStart = todayStart - 6 * 86400;

    std::vector<HistoryRecord> history;

    // Exact boundary at midnight today -> Today
    HistoryRecord hTodayBoundary;
    hTodayBoundary.filePath = L"C:\\today_boundary.mp4";
    hTodayBoundary.title = L"Today Boundary";
    hTodayBoundary.watchedTimestamp = todayStart;
    history.push_back(hTodayBoundary);

    // 1 second before midnight today -> Yesterday
    HistoryRecord hYesterdayBoundary;
    hYesterdayBoundary.filePath = L"C:\\yesterday_boundary.mp4";
    hYesterdayBoundary.title = L"Yesterday Boundary";
    hYesterdayBoundary.watchedTimestamp = todayStart - 1;
    history.push_back(hYesterdayBoundary);

    // Exact boundary at midnight yesterday -> Yesterday
    HistoryRecord hYesterdayStartBoundary;
    hYesterdayStartBoundary.filePath = L"C:\\yesterday_start.mp4";
    hYesterdayStartBoundary.title = L"Yesterday Start";
    hYesterdayStartBoundary.watchedTimestamp = yesterdayStart;
    history.push_back(hYesterdayStartBoundary);

    // 1 second before midnight yesterday -> This week
    HistoryRecord hThisWeekBoundary;
    hThisWeekBoundary.filePath = L"C:\\this_week_boundary.mp4";
    hThisWeekBoundary.title = L"This Week Boundary";
    hThisWeekBoundary.watchedTimestamp = yesterdayStart - 1;
    history.push_back(hThisWeekBoundary);

    // Exact boundary at this week start -> This week
    HistoryRecord hThisWeekStartBoundary;
    hThisWeekStartBoundary.filePath = L"C:\\this_week_start.mp4";
    hThisWeekStartBoundary.title = L"This Week Start";
    hThisWeekStartBoundary.watchedTimestamp = thisWeekStart;
    history.push_back(hThisWeekStartBoundary);

    // 1 second before this week start -> Older
    HistoryRecord hOlderBoundary;
    hOlderBoundary.filePath = L"C:\\older_boundary.mp4";
    hOlderBoundary.title = L"Older Boundary";
    hOlderBoundary.watchedTimestamp = thisWeekStart - 1;
    history.push_back(hOlderBoundary);

    HistoryTimelineProjectionOptions options;
    options.nowTimestamp = refTime;
    HistoryTimelineProjection proj = ProjectHistoryTimeline(history, options);

    TEST_ASSERT_EQ(proj.GroupCount(), 4ull);
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Today"));
    TEST_ASSERT_EQ(proj.groups[0].count, 1ull);

    TEST_ASSERT_EQ(proj.groups[1].title, std::wstring(L"Yesterday"));
    TEST_ASSERT_EQ(proj.groups[1].count, 2ull);

    TEST_ASSERT_EQ(proj.groups[2].title, std::wstring(L"This week"));
    TEST_ASSERT_EQ(proj.groups[2].count, 2ull);

    TEST_ASSERT_EQ(proj.groups[3].title, std::wstring(L"Older"));
    TEST_ASSERT_EQ(proj.groups[3].count, 1ull);
}

TEST_CASE(HistoryGrouping, MissingFileToleranceAndTitleFallback) {
    std::vector<HistoryRecord> history;

    // Record without title -> should fallback to file name
    HistoryRecord hMissingTitle;
    hMissingTitle.filePath = L"C:\\nonexistent_folder\\awesome_clip.mkv";
    hMissingTitle.title = L"";
    hMissingTitle.watchedTimestamp = 1000;
    history.push_back(hMissingTitle);

    HistoryTimelineProjection proj = ProjectHistoryTimeline(history);
    TEST_ASSERT_EQ(proj.totalCount, 1ull);
    TEST_ASSERT_EQ(proj.items[0].filePath, std::wstring(L"C:\\nonexistent_folder\\awesome_clip.mkv"));

    std::wstring displayTitle = proj.items[0].title.empty() ? GetFileNameFromPath(proj.items[0].filePath) : proj.items[0].title;
    TEST_ASSERT_EQ(displayTitle, std::wstring(L"awesome_clip.mkv"));
}

TEST_CASE(HistoryGrouping, HistoryTabViewModelInvariants) {
    HistoryTabViewModel vm;
    TEST_ASSERT_EQ(vm.bounds.left, 0.0f);
    TEST_ASSERT_EQ(vm.bounds.top, 0.0f);
    TEST_ASSERT_EQ(vm.bounds.right, 0.0f);
    TEST_ASSERT_EQ(vm.bounds.bottom, 0.0f);
    // The view model borrows history records instead of copying them, so the
    // test keeps the owned records alive in a local vector.
    std::vector<HistoryRecord> ownedHistory;
    TEST_ASSERT(vm.pHistory == nullptr);
    TEST_ASSERT(vm.groups.empty());
    TEST_ASSERT(vm.visibleItems.empty());
    TEST_ASSERT_EQ(vm.hoveredIndex, -1);
    TEST_ASSERT_EQ(vm.hoveredDeleteIndex, -1);
    TEST_ASSERT(!vm.isClearHovered);
    TEST_ASSERT(!vm.isClearPressed);
    TEST_ASSERT_EQ(vm.scrollY, 0.0f);
    TEST_ASSERT(vm.selectedIndices.empty());

    // Populate with a grouped projection
    HistoryRecord r;
    r.filePath = L"C:\\media\\demo.mp4";
    r.title = L"Demo Video";
    r.watchedTimestamp = GetCurrentTimestampSec();
    r.lastPosSec = 60.0;
    r.durationSec = 120.0;
    ownedHistory.push_back(r);
    vm.pHistory = &ownedHistory;

    auto proj = ProjectHistoryTimeline(ownedHistory);
    TEST_ASSERT_EQ(proj.GroupCount(), 1ull);
    TEST_ASSERT_EQ(proj.groups[0].title, std::wstring(L"Today"));

    vm.groups = proj.groups;

    VisibleHistoryItemViewModel itemVm;
    itemVm.originalIndex = proj.itemIndices[0];
    itemVm.pRecord = &ownedHistory[proj.itemIndices[0]];
    itemVm.itemRect = D2D1::RectF(10.0f, 100.0f, 800.0f, 176.0f);
    itemVm.isHovered = true;
    itemVm.isSelected = false;
    itemVm.displayTitle = itemVm.pRecord->title;
    itemVm.formattedDuration = FormatDuration(itemVm.pRecord->lastPosSec);
    vm.visibleItems.push_back(itemVm);

    TEST_ASSERT_EQ(vm.groups.size(), 1ull);
    TEST_ASSERT_EQ(vm.visibleItems.size(), 1ull);
    TEST_ASSERT_EQ(vm.visibleItems[0].originalIndex, 0ull);
    TEST_ASSERT_EQ(vm.visibleItems[0].displayTitle, std::wstring(L"Demo Video"));
    TEST_ASSERT(vm.visibleItems[0].isHovered);
    TEST_ASSERT(!vm.visibleItems[0].isSelected);
}

} // namespace Aperture::Test
