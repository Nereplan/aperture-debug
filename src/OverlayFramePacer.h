#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace Aperture {

// UI-thread state only. A request made before the deadline stays pending,
// including the final animation frame; late frames do not create catch-up bursts.
class OverlayFramePacer {
public:
    void Reset() {
        pending = false; frameIntervalSeconds = 1.0 / 60.0; nextFrameSeconds = 0.0; lastFrameSeconds = 0.0;
        hasFrame = false; subtitleText.clear(); lastControlsVisible = false; lastPlaybackSecond = -1;
    }
    void SetRefreshRate(double hertz) {
        if (!std::isfinite(hertz) || hertz < 20.0 || hertz > 360.0) hertz = 60.0;
        frameIntervalSeconds = 1.0 / hertz;
        if (hasFrame) nextFrameSeconds = lastFrameSeconds + frameIntervalSeconds;
    }
    double GetRefreshRate() const { return 1.0 / frameIntervalSeconds; }
    void Request() { pending = true; }
    bool HasPendingFrame() const { return pending; }
    bool IsDue(double now) const { return pending && (!hasFrame || now >= nextFrameSeconds); }
    double SecondsUntilDeadline(double now) const { return hasFrame ? std::max(0.0, nextFrameSeconds - now) : 0.0; }
    void FrameStarted(double now) {
        pending = false;
        hasFrame = true;
        lastFrameSeconds = now;
        nextFrameSeconds = now + frameIntervalSeconds;
    }
    bool PlaybackContentChanged(bool continuousVisual, std::wstring_view currentSubtitle) {
        const bool changed = currentSubtitle != subtitleText;
        if (changed) subtitleText.assign(currentSubtitle);
        return continuousVisual || changed;
    }
    // Redraws visible static HUD controls only when animation, subtitle text,
    // visibility, or the displayed playback second changes.
    bool PlaybackStateChanged(bool continuousAnimation, std::wstring_view currentSubtitle,
                              bool controlsVisible, long long currentSecond) {
        const bool subChanged = currentSubtitle != subtitleText;
        if (subChanged) subtitleText.assign(currentSubtitle);
        const bool visChanged = controlsVisible != lastControlsVisible;
        lastControlsVisible = controlsVisible;
        const bool secChanged = controlsVisible && (currentSecond != lastPlaybackSecond);
        lastPlaybackSecond = controlsVisible ? currentSecond : -1;
        return continuousAnimation || subChanged || visChanged || secChanged;
    }

private:
    bool pending = false, hasFrame = false, lastControlsVisible = false;
    double frameIntervalSeconds = 1.0 / 60.0;
    double nextFrameSeconds = 0.0, lastFrameSeconds = 0.0;
    long long lastPlaybackSecond = -1;
    std::wstring subtitleText;
};

} // namespace Aperture
