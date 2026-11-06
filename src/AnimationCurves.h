#pragma once

// Cubic-Bezier easing solver (WebKit/Blink) and shared motion duration tokens.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace Aperture {

// --- Cubic-Bézier & Animation Curves (WebKit / Blink Fast Newton-Raphson Solver) ---
struct CubicBezier {
    float x1, y1, x2, y2;
    constexpr CubicBezier(float a, float b, float c, float d) : x1(a), y1(b), x2(c), y2(d) {}
    static inline float SampleCurveX(float t, float p1, float p2) {
        return (((1.0f - 3.0f * p2 + 3.0f * p1) * t + (3.0f * p2 - 6.0f * p1)) * t + (3.0f * p1)) * t;
    }
    static inline float SampleCurveY(float t, float p1, float p2) {
        return (((1.0f - 3.0f * p2 + 3.0f * p1) * t + (3.0f * p2 - 6.0f * p1)) * t + (3.0f * p1)) * t;
    }
    static inline float SampleCurveDerivativeX(float t, float p1, float p2) {
        return (3.0f * (1.0f - 3.0f * p2 + 3.0f * p1) * t + 2.0f * (3.0f * p2 - 6.0f * p1)) * t + (3.0f * p1);
    }
    inline float SolveTForX(float x) const {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        // Newton-Raphson method (4 iterations)
        float t = x;
        for (int i = 0; i < 4; ++i) {
            float curX = SampleCurveX(t, x1, x2) - x;
            float dX = SampleCurveDerivativeX(t, x1, x2);
            if (std::abs(dX) < 1e-6f) break;
            t -= curX / dX;
        }
        // Fallback to bisection if t goes out of range
        if (t < 0.0f || t > 1.0f) {
            float lo = 0.0f, hi = 1.0f;
            t = x;
            for (int i = 0; i < 8; ++i) { float cx = SampleCurveX(t, x1, x2); if (cx > x) hi = t; else lo = t; t = (lo + hi) * 0.5f; }
        }
        return std::clamp(t, 0.0f, 1.0f);
    }
    inline float Ease(float x) const {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        if (x1 == y1 && x2 == y2) return x;
        float t = SolveTForX(x);
        return SampleCurveY(t, y1, y2);
    }
};

inline const CubicBezier kEaseStandard(0.25f, 1.0f, 0.5f, 1.0f);
inline const CubicBezier kEaseSpring(0.16f, 1.0f, 0.3f, 1.0f);
// Seek-OSD choreography: decelerating entrance, ease-out exit drift, soft press pop.
inline const CubicBezier kEaseEnter(0.2f, 0.0f, 0.0f, 1.0f);
inline const CubicBezier kEaseExit(0.0f, 0.0f, 0.58f, 1.0f);
inline const CubicBezier kEasePop(0.25f, 0.1f, 0.25f, 1.0f);
// Dynamic-island morphs per the reference design: shell spring (no
// overshoot) for size/radius, collapse spring for the exit, pop for content.
inline const CubicBezier kEaseIsland(0.34f, 1.36f, 0.42f, 1.0f);
inline const CubicBezier kEaseIslandShell(0.22f, 1.0f, 0.36f, 1.0f);
inline const CubicBezier kEaseIslandCollapse(0.32f, 0.72f, 0.0f, 1.0f);

namespace MotionTokens {
// Motion budget: hover and press transitions land fast (0.10-0.18s), larger
// layout changes settle within ~0.3s. Feedback should read as immediate.
inline constexpr float kFast = 0.12f;
inline constexpr float kToggle = 0.14f;
inline constexpr float kResponsive = 0.18f;
inline constexpr float kSettle = 0.25f;
inline constexpr float kModerate = 0.22f;
inline constexpr float kDeliberate = 0.24f;
inline constexpr float kReveal = 0.30f;
inline constexpr float kPreview = 0.32f;
inline constexpr float kPlayPauseBadge = 0.32f;
inline constexpr float kHoverRevealDelay = 0.35f;
inline constexpr float kMinReversal = 0.06f;
inline constexpr float kMinimumDuration = 0.01f;

inline const CubicBezier* Standard() { return &kEaseStandard; }
inline const CubicBezier* Spring() { return &kEaseSpring; }
}

inline float NowSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

struct AnimatedFloat {
    float from = 0.0f, to = 0.0f, current = 0.0f;
    float startTime = 0.0f, duration = MotionTokens::kResponsive;
    const CubicBezier* ease = &kEaseStandard;
    bool animating = false;
    void SnapTo(float v) { from = to = current = v; animating = false; }
    void AnimateTo(float target, float durSec, const CubicBezier* curve = &kEaseStandard) {
        if (std::abs(target - to) < 0.0001f && animating) return;
        // Hover handlers may request the settled target on every update.
        // Do not restart a zero-distance animation and keep repainting forever.
        if (target == current) { SnapTo(target); return; }
        // When reversing mid-flight, scale duration proportionally to remaining distance
        float prevTravel = std::abs(to - from);
        float remDist = std::abs(target - current);
        if (animating && prevTravel > 0.001f && remDist < prevTravel)
            durSec = std::max(MotionTokens::kMinReversal, durSec * (remDist / prevTravel));
        from = current;
        to = target;
        duration = std::max(MotionTokens::kMinimumDuration, durSec);
        ease = curve;
        startTime = NowSeconds();
        animating = true;
    }
    bool Tick() {
        return animating ? Tick(NowSeconds()) : false;
    }
    bool Tick(float now) {
        if (!animating) return false;
        float t = (now - startTime) / duration;
        // The terminal sample still changes the visual state and layout.
        if (t >= 1.0f) { current = to; animating = false; return true; }
        current = from + (to - from) * ease->Ease(t);
        return true;
    }
};

} // namespace Aperture
