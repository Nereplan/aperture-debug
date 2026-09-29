#pragma once

// Pure-logic coverage for the touch gesture engine (src/TouchInput.h): tap vs
// pan resolution, axis lock, long-press timing, double-tap windows, fling
// velocity windows, and the rubber-band helpers.

#include "../support/TestFramework.h"
#include "../../src/TouchInput.h"

namespace Aperture {
namespace Test {

namespace {
using Touch::Engine;

// Feed a straight-line drag: one sample every `stepSec` seconds.
void Drag(Engine& e, float fromX, float fromY, float toX, float toY, int steps, double t0, double stepSec) {
    for (int i = 1; i <= steps; ++i) {
        const double t = t0 + stepSec * i;
        const float f = static_cast<float>(i) / static_cast<float>(steps);
        e.Move(fromX + (toX - fromX) * f, fromY + (toY - fromY) * f, t);
    }
}
} // namespace

TEST_CASE(TouchInput, QuickStillPressResolvesAsTap) {
    Engine e;
    e.Begin(1, 100.0f, 200.0f, 0.0);
    const auto outcome = e.Finish(0.10);
    TEST_ASSERT(e.phase() == Touch::Phase::Pressing);
    TEST_ASSERT(outcome == Engine::Outcome::Tap);
}

TEST_CASE(TouchInput, MovementPastSlopBecomesPanWithVerticalAxisLock) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    const bool began = e.Move(0.0f, -8.0f, 0.05); // 8 DIP up > 6 DIP slop
    TEST_ASSERT(began);
    TEST_ASSERT(e.phase() == Touch::Phase::Panning);
    TEST_ASSERT(e.axis() == Touch::Axis::Vertical);
    TEST_ASSERT(e.totalDy() < 0.0f);
    TEST_ASSERT(e.Finish(0.10) == Engine::Outcome::Pan);
}

TEST_CASE(TouchInput, HorizontalDragLocksHorizontalAxis) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    e.Move(9.0f, 3.0f, 0.05);
    TEST_ASSERT(e.axis() == Touch::Axis::Horizontal);
}

TEST_CASE(TouchInput, SmallDriftInsideTapSlopStaysATap) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    e.Move(4.0f, 4.0f, 0.03); // hypot ~5.7 < 9 DIP tap slop
    TEST_ASSERT(e.phase() == Touch::Phase::Pressing);
    TEST_ASSERT(e.Finish(0.08) == Engine::Outcome::Tap);
}

TEST_CASE(TouchInput, LongPressDueOnlyAfterHoldWithoutMovement) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    TEST_ASSERT(!e.LongPressDue(0.25));
    TEST_ASSERT(e.LongPressDue(0.50));
    e.MarkLongPress();
    TEST_ASSERT(e.phase() == Touch::Phase::LongPress);
    TEST_ASSERT(!e.LongPressDue(1.0));
}

TEST_CASE(TouchInput, HeldPressPastLongPressIsNotATap) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    TEST_ASSERT(e.Finish(0.80) == Engine::Outcome::None);
}

TEST_CASE(TouchInput, DoubleTapWindowAndSlop) {
    Engine e;
    e.RegisterTap(100.0f, 100.0f, 1.0);
    TEST_ASSERT(e.IsDoubleTapCandidate(105.0f, 104.0f, 1.20));          // close, soon
    TEST_ASSERT(!e.IsDoubleTapCandidate(100.0f, 100.0f, 1.45));         // too late
    TEST_ASSERT(!e.IsDoubleTapCandidate(200.0f, 200.0f, 1.10));         // too far
    e.ClearTapMemory();
    TEST_ASSERT(!e.IsDoubleTapCandidate(100.0f, 100.0f, 1.05));
}

TEST_CASE(TouchInput, FlingVelocityTracksRecentMotion) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    // 2400 DIP/s downward fling sampled at 120 Hz.
    Drag(e, 0.0f, 0.0f, 0.0f, 360.0f, 18, 0.0, 1.0 / 120.0);
    TEST_ASSERT(e.velocityY() > 2000.0f);
    TEST_ASSERT(e.velocityX() == 0.0f);
}

TEST_CASE(TouchInput, PauseBeforeLiftSuppressesFling) {
    Engine e;
    e.Begin(1, 0.0f, 0.0f, 0.0);
    Drag(e, 0.0f, 0.0f, 0.0f, 300.0f, 24, 0.0, 1.0 / 120.0);
    TEST_ASSERT(e.velocityY() > 1000.0f);
    // Finger stops; the lift happens 120 ms later with no further samples.
    TEST_ASSERT(e.Finish(0.20 + 0.12) == Engine::Outcome::Pan);
    TEST_ASSERT(e.velocityY() == 0.0f);
}

TEST_CASE(TouchInput, SecondPointerBeginRestartsWithoutTapMemoryLoss) {
    Engine e;
    e.RegisterTap(52.0f, 48.0f, 1.0);
    e.Begin(2, 50.0f, 50.0f, 1.05);
    TEST_ASSERT(e.active());
    TEST_ASSERT(e.pointerId() == 2);
    TEST_ASSERT(e.IsDoubleTapCandidate(52.0f, 48.0f, 1.15)); // memory survives Begin
    e.Reset();
    TEST_ASSERT(!e.active());
}

TEST_CASE(TouchInput, OverscrollResistanceOnlyAppliesPastBounds) {
    const float step = 10.0f;
    TEST_ASSERT(Touch::ResistedScrollStep(50.0f, 0.0f, 200.0f, step) == step);  // mid-range
    TEST_ASSERT(Touch::ResistedScrollStep(0.0f, 0.0f, 200.0f, -step) == -step * Touch::kOverscrollResistance);  // at top, dragging up
    TEST_ASSERT(Touch::ResistedScrollStep(200.0f, 0.0f, 200.0f, step) == step * Touch::kOverscrollResistance);  // at bottom, dragging down
    TEST_ASSERT(Touch::ResistedScrollStep(-12.0f, 0.0f, 200.0f, -step) == -step * Touch::kOverscrollResistance); // already overscrolled
    TEST_ASSERT(Touch::ResistedScrollStep(-12.0f, 0.0f, 200.0f, step) == step); // springing back is unresisted
}

TEST_CASE(TouchInput, SpringTowardApproachesTargetWithoutOvershooting) {
    const float a = Touch::SpringToward(0.0f, 100.0f, 0.0f, 16.0f);
    TEST_ASSERT(a == 0.0f);
    const float b = Touch::SpringToward(0.0f, 100.0f, 1.0f / 60.0f, 16.0f);
    TEST_ASSERT(b > 0.0f && b < 100.0f);
    const float c = Touch::SpringToward(0.0f, 100.0f, 10.0f, 16.0f);
    TEST_ASSERT(c == 100.0f);
}

} // namespace Test
} // namespace Aperture
