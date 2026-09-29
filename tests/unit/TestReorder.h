#pragma once
#include "../support/TestFramework.h"
#include "../support/TestTempDir.h"
#include "../../src/Reorder.h"
#include "../../src/LibraryQuery.h"
#include <numeric>

namespace Aperture::Test {
TEST_CASE(Reorder, EveryMoveMatchesTheDisplayedSlots) {
    for (int count = 1; count <= 20; ++count) {
        for (int from = 0; from < count; ++from) for (int to = 0; to < count; ++to) {
            std::vector<int> items(count);
            std::iota(items.begin(), items.end(), 0);
            Reorder::Move(items, from, to);
            for (int i = 0; i < count; ++i) TEST_ASSERT_EQ(items[Reorder::Slot(i, from, to)], i);
        }
    }
    std::vector<int> items{0, 1, 2};
    TEST_ASSERT(!Reorder::Move(items, -1, 1));
    TEST_ASSERT(!Reorder::Move(items, 0, 3));
    TEST_ASSERT_EQ(items[0], 0);
}
TEST_CASE(Reorder, GridWrapsAndClampsAtEveryDpi) {
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        TEST_ASSERT_EQ(Reorder::GridTarget(210*scale, 20*scale, 200*scale, 176*scale, 3, 8), 1);
        TEST_ASSERT_EQ(Reorder::GridTarget(20*scale, 180*scale, 200*scale, 176*scale, 3, 8), 3);
        TEST_ASSERT_EQ(Reorder::GridTarget(590*scale, 400*scale, 200*scale, 176*scale, 3, 8), 7);
        TEST_ASSERT_EQ(Reorder::GridTarget(-20*scale, -20*scale, 200*scale, 176*scale, 3, 8), 0);
        TEST_ASSERT_EQ(Reorder::GridTarget(0, 0, 200*scale, 176*scale, 3, 0), -1);
    }
}
TEST_CASE(Reorder, CenterTargetsChangeAtSlotMidpoints) {
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const float pitch = 68.0f * scale;
        const float firstCenter = 31.0f * scale;
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(firstCenter, firstCenter, pitch, 5), 0);
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(firstCenter + pitch * 0.49f, firstCenter, pitch, 5), 0);
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(firstCenter + pitch * 0.51f, firstCenter, pitch, 5), 1);
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(-1000.0f, firstCenter, pitch, 5), 0);
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(10000.0f, firstCenter, pitch, 5), 4);
        TEST_ASSERT_EQ(Reorder::LinearTargetFromCenter(firstCenter, firstCenter, pitch, 0), -1);
    }
}
TEST_CASE(Reorder, GridCenterTargetIgnoresGrabOffsetAndGaps) {
    const float pitchX = 200.0f;
    const float pitchY = 176.0f;
    const float firstCenterX = 95.0f;
    const float firstCenterY = 83.0f;
    TEST_ASSERT_EQ(Reorder::GridTargetFromCenter(firstCenterX + pitchX * 0.49f, firstCenterY,
        firstCenterX, firstCenterY, pitchX, pitchY, 3, 8), 0);
    TEST_ASSERT_EQ(Reorder::GridTargetFromCenter(firstCenterX + pitchX * 0.51f, firstCenterY,
        firstCenterX, firstCenterY, pitchX, pitchY, 3, 8), 1);
    TEST_ASSERT_EQ(Reorder::GridTargetFromCenter(firstCenterX, firstCenterY + pitchY * 0.51f,
        firstCenterX, firstCenterY, pitchX, pitchY, 3, 8), 3);
    TEST_ASSERT_EQ(Reorder::GridTargetFromCenter(10000.0f, 10000.0f,
        firstCenterX, firstCenterY, pitchX, pitchY, 3, 8), 7);
    TEST_ASSERT_EQ(Reorder::GridTargetFromCenter(firstCenterX, firstCenterY,
        firstCenterX, firstCenterY, pitchX, pitchY, 3, 0), -1);
}
TEST_CASE(Reorder, EdgeScrollHasDeadZoneAndAccelerates) {
    TEST_ASSERT_EQ(Reorder::EdgeSpeed(300, 100, 600, 1), 0.0f);
    TEST_ASSERT(Reorder::EdgeSpeed(105, 100, 600, 1) < Reorder::EdgeSpeed(130, 100, 600, 1));
    TEST_ASSERT(Reorder::EdgeSpeed(595, 100, 600, 1) > Reorder::EdgeSpeed(570, 100, 600, 1));
    TEST_ASSERT_EQ(Reorder::EdgeSpeed(800, 100, 600, 1), 0.0f);
}
TEST_CASE(Reorder, ManualProjectionKeepsSavedOrderWithAndWithoutPinnedItem) {
    ScopedTempDir stage(L"Aperture_Reorder_");
    stage.CreateSubDir(L"data");
    stage.WriteFile(L"data\\library.json", R"([{"id":"z","filePath":"z.mp4","title":"Zeta"},{"id":"a","filePath":"a.mp4","title":"Alpha"},{"id":"b","filePath":"b.mp4","title":"Beta","lastWatchedTimestamp":10}])");
    MediaLibrary library;
    library.LoadFromRoot(stage.GetPath());
    LibraryQueryEngine engine;
    LibraryQueryOptions options;
    options.sortField = LibrarySortField::Manual;
    auto manual = engine.Query(library, options);
    TEST_ASSERT_EQ(manual.items.size(), size_t(3));
    TEST_ASSERT_EQ(manual.items[0].id, std::string("z"));
    TEST_ASSERT_EQ(manual.items[1].id, std::string("a"));
    options.pinLastWatched = true;
    auto pinned = engine.Query(library, options);
    TEST_ASSERT_EQ(pinned.items[0].id, std::string("b"));
    TEST_ASSERT_EQ(pinned.items[1].id, std::string("z"));
    TEST_ASSERT_EQ(pinned.items[2].id, std::string("a"));
    options.pinLastWatched = false;
    options.sortField = LibrarySortField::Title;
    auto alphabetical = engine.Query(library, options);
    TEST_ASSERT_EQ(alphabetical.items[0].id, std::string("a"));
}
}
