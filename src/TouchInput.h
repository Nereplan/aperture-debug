#pragma once

// Pure-logic touch gesture recognition shared by the main window (hub) and the
// player overlay windows. No Win32 dependencies: the App feeds it pointer
// positions in DIPs and timestamps in seconds, and reads back the recognized
// phase, axis lock, and fling velocity. Zone semantics (what a tap, double-tap,
// long-press, or pan means in each window) live in AppTouch.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Aperture {
namespace Touch {

// Timing windows, matching platform conventions.
inline constexpr double kLongPressSec = 0.50;        // hold-to-open context menu
inline constexpr double kDoubleTapWindowSec = 0.30;  // tap-to-tap interval
// Distances in DIPs.
inline constexpr float kTapSlopDIP = 9.0f;           // movement tolerated inside a tap
inline constexpr float kPanSlopDIP = 6.0f;           // movement that claims a pan
inline constexpr float kDoubleTapSlopDIP = 24.0f;    // second tap may land this far away
// Fling and rubber-band dynamics.
inline constexpr float kFlingStopDIPPerSec = 50.0f;  // below this the glide stops
inline constexpr float kMaxFlingDIPPerSec = 5000.0f;
inline constexpr float kInertiaTauSec = 0.34f;       // exponential fling decay
inline constexpr float kOverscrollResistance = 0.28f;// drag past a bound scales by this
inline constexpr float kOverscrollSpringRate = 16.0f;// rubber-band settle rate (1/s)

enum class Phase : std::uint8_t { Idle, Pressing, Panning, LongPress, Consumed };
enum class Axis : std::uint8_t { Undecided, Horizontal, Vertical };

// Rubber-band resistance: a drag step that pushes the offset at-or-past a
// bound further out is scaled down so overscroll visibly fights the finger.
// Steps moving back toward the bounds stay unresisted.
inline float ResistedScrollStep(float offset, float lowerBound, float upperBound, float step) {
    const bool pushingFurtherPastUpper = offset > upperBound - 0.01f && step > 0.0f;
    const bool pushingFurtherPastLower = offset < lowerBound + 0.01f && step < 0.0f;
    if (pushingFurtherPastUpper || pushingFurtherPastLower) return step * kOverscrollResistance;
    return step;
}

// Critically-damped-ish approach used to settle overscroll back inside bounds.
inline float SpringToward(float value, float target, float dt, float rate) {
    const float t = std::clamp(dt * rate, 0.0f, 1.0f);
    return value + (target - value) * t;
}

class Engine {
public:
    // Outcome of Finish(): the gesture resolved as a completed pan or a tap.
    enum class Outcome : std::uint8_t { None, Tap, Pan };

    void Begin(std::uint32_t pointerId, float x, float y, double nowSec) {
        pointerId_ = pointerId;
        phase_ = Phase::Pressing;
        axis_ = Axis::Undecided;
        downX_ = x_ = lastX_ = x;
        downY_ = y_ = lastY_ = y;
        downTime_ = lastTime_ = nowSec;
        movedBeyondSlop_ = false;
        panNotified_ = false;
        ClearSamples();
        PushSample(nowSec, x, y);
        velocityX_ = velocityY_ = 0.0f;
    }

    // Returns true exactly once, when the pointer crosses the pan slop and the
    // axis lock is chosen. After that the engine stays in Phase::Panning.
    bool Move(float x, float y, double nowSec) {
        if (phase_ != Phase::Pressing && phase_ != Phase::Panning) return false;
        const float dt = static_cast<float>(std::max(1e-4, nowSec - lastTime_));
        lastX_ = x_;
        lastY_ = y_;
        lastTime_ = nowSec;
        x_ = x;
        y_ = y;
        PushSample(nowSec, x, y);
        const float dx = x - downX_;
        const float dy = y - downY_;
        if (!movedBeyondSlop_) {
            const float panSlop = kPanSlopDIP;
            if (std::abs(dx) >= panSlop || std::abs(dy) >= panSlop) {
                movedBeyondSlop_ = true;
                axis_ = std::abs(dx) >= std::abs(dy) ? Axis::Horizontal : Axis::Vertical;
                phase_ = Phase::Panning;
                panNotified_ = true;
                UpdateVelocity(nowSec);
                return true;
            }
        }
        UpdateVelocity(nowSec);
        (void)dt;
        return false;
    }

    Outcome Finish(double nowSec) {
        if (phase_ == Phase::Panning) {
            UpdateVelocity(nowSec);
            return Outcome::Pan;
        }
        if (phase_ == Phase::Pressing) {
            const float dx = x_ - downX_;
            const float dy = y_ - downY_;
            if (!movedBeyondSlop_ && std::hypot(dx, dy) <= kTapSlopDIP) {
                const double held = nowSec - downTime_;
                if (held < kLongPressSec) return Outcome::Tap;
            }
            return Outcome::None;
        }
        return Outcome::None;
    }

    void Cancel() { phase_ = Phase::Idle; }
    void Reset() { phase_ = Phase::Idle; axis_ = Axis::Undecided; pointerId_ = 0; }

    bool LongPressDue(double nowSec) const {
        return phase_ == Phase::Pressing && !movedBeyondSlop_ && (nowSec - downTime_) >= kLongPressSec;
    }
    void MarkLongPress() { phase_ = Phase::LongPress; }
    void Consume() { phase_ = Phase::Consumed; }

    bool active() const { return phase_ != Phase::Idle; }
    Phase phase() const { return phase_; }
    Axis axis() const { return axis_; }
    std::uint32_t pointerId() const { return pointerId_; }
    bool movedBeyondSlop() const { return movedBeyondSlop_; }
    bool panNotified() const { return panNotified_; }

    float startX() const { return downX_; }
    float startY() const { return downY_; }
    float x() const { return x_; }
    float y() const { return y_; }
    float totalDx() const { return x_ - downX_; }
    float totalDy() const { return y_ - downY_; }
    // Filtered finger velocity in DIP/s over the trailing sample window.
    // Positive Y means the finger is moving down the screen.
    float velocityX() const { return velocityX_; }
    float velocityY() const { return velocityY_; }

    // Double-tap memory: set after each accepted tap, consumed by the caller.
    bool IsDoubleTapCandidate(float x, float y, double nowSec) const {
        const bool hasTap = lastTapTime_ >= 0.0;
        return hasTap && (nowSec - lastTapTime_) <= kDoubleTapWindowSec &&
               std::hypot(x - lastTapX_, y - lastTapY_) <= kDoubleTapSlopDIP;
    }
    void RegisterTap(float x, float y, double nowSec) {
        lastTapX_ = x;
        lastTapY_ = y;
        lastTapTime_ = nowSec;
    }
    void ClearTapMemory() { lastTapTime_ = -1.0; lastTapX_ = lastTapY_ = 0.0f; }

private:
    static constexpr int kSampleCapacity = 10;   // covers the trailing window
    static constexpr double kVelocityWindowSec = 0.10;

    struct Sample { double t; float x, y; };

    void ClearSamples() { sampleCount_ = 0; sampleStart_ = 0; }
    void PushSample(double t, float x, float y) {
        const int next = (sampleStart_ + sampleCount_) % kSampleCapacity;
        samples_[next] = {t, x, y};
        if (sampleCount_ < kSampleCapacity) { ++sampleCount_; }
        else { sampleStart_ = (sampleStart_ + 1) % kSampleCapacity; }
        // Drop samples that aged out of the velocity window.
        while (sampleCount_ > 2 && (t - samples_[sampleStart_].t) > kVelocityWindowSec) {
            sampleStart_ = (sampleStart_ + 1) % kSampleCapacity;
            --sampleCount_;
        }
    }
    const Sample& OldestSample() const { return samples_[sampleStart_]; }
    const Sample& NewestSample() const {
        return samples_[(sampleStart_ + sampleCount_ + kSampleCapacity - 1) % kSampleCapacity];
    }
    // Velocity over the trailing sample window. A pointer that has been
    // stationary for more than ~2 frames reports zero velocity, so a brief
    // hold before lifting never converts into a fling.
    void UpdateVelocity(double refTime) {
        if (sampleCount_ < 2) { velocityX_ = velocityY_ = 0.0f; return; }
        const Sample& a = OldestSample();
        const Sample& b = NewestSample();
        const double span = b.t - a.t;
        if (span < 1e-3) return;
        if (refTime - b.t > 0.04) { velocityX_ = velocityY_ = 0.0f; return; }
        velocityX_ = static_cast<float>((b.x - a.x) / span);
        velocityY_ = static_cast<float>((b.y - a.y) / span);
    }

    int sampleCount_ = 0, sampleStart_ = 0;
    Sample samples_[kSampleCapacity] = {};

    std::uint32_t pointerId_ = 0;
    Phase phase_ = Phase::Idle;
    Axis axis_ = Axis::Undecided;
    float downX_ = 0.0f, downY_ = 0.0f;
    float x_ = 0.0f, y_ = 0.0f;
    float lastX_ = 0.0f, lastY_ = 0.0f;
    double downTime_ = 0.0, lastTime_ = 0.0;
    float velocityX_ = 0.0f, velocityY_ = 0.0f;
    bool movedBeyondSlop_ = false;
    bool panNotified_ = false;

    double lastTapTime_ = -1.0;
    float lastTapX_ = 0.0f, lastTapY_ = 0.0f;
};

} // namespace Touch
} // namespace Aperture
