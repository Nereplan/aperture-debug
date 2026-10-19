#pragma once
#include "../support/TestFramework.h"
#include "../../src/OverlayFramePacer.h"
#include <limits>

namespace Aperture::Test {

TEST_CASE(OverlayPacing, CadenceAndRedrawSuppression) {
    OverlayFramePacer pacer;
    pacer.SetRefreshRate(60.0);
    pacer.Request();
    TEST_ASSERT(pacer.IsDue(1.0));
    pacer.FrameStarted(1.0);
    for (int request = 1; request <= 15; ++request) {
        pacer.Request();
        TEST_ASSERT(!pacer.IsDue(1.0 + request / 1000.0));
    }
    TEST_ASSERT(pacer.HasPendingFrame());
    TEST_ASSERT(pacer.IsDue(1.017));
    pacer.FrameStarted(1.017);
    TEST_ASSERT(!pacer.HasPendingFrame());
    TEST_ASSERT(!pacer.IsDue(10.0));

    pacer.Request();
    TEST_ASSERT(pacer.IsDue(14.0));
    pacer.FrameStarted(14.0);
    pacer.Request();
    TEST_ASSERT(!pacer.IsDue(14.001));
    pacer.SetRefreshRate(120.0);
    TEST_ASSERT(!pacer.IsDue(14.008));
    TEST_ASSERT(pacer.IsDue(14.009));
    pacer.SetRefreshRate(30.0);
    TEST_ASSERT(!pacer.IsDue(14.02));
    TEST_ASSERT(pacer.IsDue(14.034));
    pacer.SetRefreshRate(std::numeric_limits<double>::quiet_NaN());
    TEST_ASSERT_EQ(pacer.GetRefreshRate(), 60.0);
    pacer.SetRefreshRate(0.0);
    TEST_ASSERT_EQ(pacer.GetRefreshRate(), 60.0);

    TEST_ASSERT(!pacer.PlaybackContentChanged(false, L""));
    TEST_ASSERT(pacer.PlaybackContentChanged(false, L"A cue"));
    for (int frame = 0; frame < 240; ++frame) TEST_ASSERT(!pacer.PlaybackContentChanged(false, L"A cue"));
    TEST_ASSERT(pacer.PlaybackContentChanged(false, L"Next cue"));
    TEST_ASSERT(pacer.PlaybackContentChanged(false, L""));
    TEST_ASSERT(!pacer.PlaybackContentChanged(false, L""));
    TEST_ASSERT(pacer.PlaybackContentChanged(true, L""));
    pacer.Reset();
    TEST_ASSERT(pacer.PlaybackContentChanged(false, L"A cue"));
}

} // namespace Aperture::Test
