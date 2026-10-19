#pragma once
#include "../support/TestFramework.h"
#include "../../src/AnimationCurves.h"
#include "../../src/AppLayout.h"

namespace Aperture::Test {
TEST_CASE(AnimationPerformance, CurvesAndVirtualLayoutReuse) {
    AnimatedFloat value;
    value.AnimateTo(160.0f, 0.32f);
    TEST_ASSERT(value.Tick(value.startTime + value.duration + 0.01f));
    TEST_ASSERT_EQ(value.current, 160.0f);
    TEST_ASSERT(!value.animating);
    TEST_ASSERT(!value.Tick());

    value.SnapTo(160.0f);
    for (int frame = 0; frame < 120; ++frame) {
        value.AnimateTo(160.0f, 0.32f);
        TEST_ASSERT(!value.animating);
        TEST_ASSERT(!value.Tick());
    }
    value.AnimateTo(76.0f, 0.32f);
    TEST_ASSERT(value.animating);
    TEST_ASSERT(value.Tick(value.startTime + 0.1f));
    const float intermediate = value.current;
    value.AnimateTo(160.0f, 0.32f);
    TEST_ASSERT_EQ(value.from, intermediate);
    TEST_ASSERT(value.Tick(value.startTime + value.duration + 0.01f));
    TEST_ASSERT_EQ(value.current, 160.0f);

    std::vector<float> heights(25000, kMediaRestHeight);
    heights[0] = kMediaPinnedRestHeight;
    std::vector<float> partial, full;
    UpdateFlatMediaRowOffsets(partial, heights.size(), 0, [&](size_t i) { return heights[i]; });
    for (int frame = 0; frame < 100; ++frame) {
        heights[24000] = kMediaRestHeight + (frame % 2 ? 0.0f : 61.375f);
        size_t visits = 0;
        UpdateFlatMediaRowOffsets(partial, heights.size(), 24000, [&](size_t i) { ++visits; return heights[i]; });
        UpdateFlatMediaRowOffsets(full, heights.size(), 0, [&](size_t i) { return heights[i]; });
        TEST_ASSERT_EQ(visits, size_t(1000));
        TEST_ASSERT(partial == full);
    }
    heights.resize(10);
    UpdateFlatMediaRowOffsets(partial, heights.size(), 24000, [&](size_t i) { return heights[i]; });
    UpdateFlatMediaRowOffsets(full, heights.size(), 0, [&](size_t i) { return heights[i]; });
    TEST_ASSERT(partial == full);
    TEST_ASSERT_EQ(UpdateFlatMediaRowOffsets(partial, 0, 0, [](size_t) { return 76.0f; }), 0.0f);
}
}
