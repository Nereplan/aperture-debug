#pragma once

#include "../support/TestFramework.h"
#include "../../src/ShuffleSequence.h"

namespace Aperture::Test {

static void step_Shuffle_SingleItemQueue() {
    ShuffleSequence seq(7);
    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT_EQ(seq.Next(0, 1), 0ull);
    }
    // Empty queue falls back to index 0.
    TEST_ASSERT_EQ(seq.Next(0, 0), 0ull);
}

static void step_Shuffle_SeededSequenceCoversAllBeforeRepeat() {
    ShuffleSequence seq(42);
    bool seen[5] = { false, false, false, false, false };
    // The four draws after index 0 must be exactly {1,2,3,4} in some order.
    size_t fourthDraw = 0;
    for (int i = 0; i < 4; ++i) {
        size_t drawn = seq.Next(0, 5);
        TEST_ASSERT(drawn >= 1 && drawn <= 4);
        TEST_ASSERT(!seen[drawn]);
        seen[drawn] = true;
        fourthDraw = drawn;
    }
    for (size_t v = 1; v <= 4; ++v) TEST_ASSERT(seen[v]);
    // Pool exhausted: the refill excludes the current index only.
    size_t afterRefill = seq.Next(fourthDraw, 5);
    TEST_ASSERT(afterRefill != fourthDraw);
    TEST_ASSERT(afterRefill <= 4);
}

static void step_Shuffle_ResetForgetsVisitState() {
    ShuffleSequence seq(1234);
    (void)seq.Next(0, 5);
    (void)seq.Next(1, 5);
    seq.Reset();
    // After Reset the visit pool is refilled, so a draw from index 0 lands in 1..4.
    size_t drawn = seq.Next(0, 5);
    TEST_ASSERT(drawn >= 1 && drawn <= 4);
}

static void step_Shuffle_TwoItemAlternation() {
    ShuffleSequence seq(99);
    size_t current = 0;
    for (int i = 0; i < 32; ++i) {
        size_t drawn = seq.Next(current, 2);
        TEST_ASSERT(drawn != current);
        TEST_ASSERT(drawn <= 1);
        current = drawn;
    }
}

TEST_CASE(Shuffle, SequenceBehavior) {
    step_Shuffle_SingleItemQueue();
    step_Shuffle_SeededSequenceCoversAllBeforeRepeat();
    step_Shuffle_ResetForgetsVisitState();
    step_Shuffle_TwoItemAlternation();
}

} // namespace Aperture::Test
