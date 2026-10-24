#pragma once
#include "Settings.h"
#include "Localization.h"

namespace Aperture::Presentation {
inline constexpr float ButtonRadius = 8.0f;
inline constexpr float DialogRadius = 14.0f;
inline constexpr float ConfirmationHeight = 224.0f;
inline constexpr float PlayerPadding = 20.0f;
inline constexpr float PlayerControlsY = 30.0f;

// Canonical hub content view rect, inset below the nav bar chrome. Rendering
// (AppViewModels) and every pointer path (move, down, up, wheel) derive from
// this one definition so hit registration cannot drift from what is drawn.
inline D2D1_RECT_F HubView(float width, float height, float dp) {
    const float pad = 18.0f * dp, padY = 14.0f * dp, viewTop = 84.0f * dp;
    return D2D1::RectF(pad, viewTop, std::max(pad + 1.0f * dp, width - pad),
                       std::max(viewTop + 1.0f * dp, height - padY));
}

// Canonical top-nav geometry (client coordinates). Layout: segmented tab track
// on the leading edge, "Settings" and the primary "Open File" action on the
// trailing edge. RTL mirrors every rect. `measure(text)` returns the Medium
// font text width in pixels. Tab widths are constant (the Playlists tab is
// sized for its drag-drop label) so nothing shifts while dragging.
struct NavLayout {
    D2D1_RECT_F tabs[3];
    D2D1_RECT_F track, settings, open;
    float tabY = 0, tabH = 0, iconSize = 0, iconGap = 0;
};
template <class Measure>
inline NavLayout Nav(float winW, float dp, bool rtl, Measure&& measure) {
    NavLayout l{};
    const float pad = 18.0f * dp, padY = 14.0f * dp;
    l.tabH = 32.0f * dp; l.tabY = padY + 4.0f * dp; l.iconSize = 14.0f * dp; l.iconGap = 8.0f * dp;
    const float tabPad = 16.0f * dp, tabGap = 2.0f * dp, actionGap = 10.0f * dp, inset = 3.0f * dp;
    float w[3] = {
        measure(Loc::Tr("nav.media")),
        std::max(measure(Loc::Tr("nav.playlists")), measure(Loc::Tr("nav.drop_to_playlist"))),
        measure(Loc::Tr("nav.history")) };
    float total = tabGap * 2.0f;
    for (float& v : w) { v += l.iconSize + l.iconGap + tabPad * 2.0f; total += v; }
    const float openW = measure(Loc::Tr("nav.open_file")) + 36.0f * dp;
    const float settingsW = l.iconSize + l.iconGap + measure(Loc::Tr("nav.settings")) + 28.0f * dp;
    const float openX = winW - pad - openW, settingsX = openX - actionGap - settingsW;
    const float available = std::max(0.0f, settingsX - actionGap - pad - inset - tabGap * 2.0f);
    const float scale = std::min(1.0f, available / (total - tabGap * 2.0f));
    total = tabGap * 2.0f;
    for (float& width : w) { width *= scale; total += width; }
    float x = pad;
    const float startX = x;
    for (int i = 0; i < 3; ++i) { l.tabs[i] = D2D1::RectF(x, l.tabY, x + w[i], l.tabY + l.tabH); x += w[i] + tabGap; }
    l.track = D2D1::RectF(startX - inset, l.tabY - inset, startX + total + inset, l.tabY + l.tabH + inset);
    l.settings = D2D1::RectF(settingsX, l.tabY, settingsX + settingsW, l.tabY + l.tabH);
    l.open = D2D1::RectF(openX, l.tabY, openX + openW, l.tabY + l.tabH);
    if (rtl) {
        const auto mirror = [winW](D2D1_RECT_F& r) { const float left = winW - r.right; r.right = winW - r.left; r.left = left; };
        for (auto& t : l.tabs) mirror(t);
        mirror(l.track); mirror(l.settings); mirror(l.open);
    }
    return l;
}

inline D2D1_COLOR_F ForegroundOn(D2D1_COLOR_F background) {
    const auto linear = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float luminance = 0.2126f * linear(background.r) + 0.7152f * linear(background.g) + 0.0722f * linear(background.b);
    return luminance > 0.179f ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1);
}

struct ButtonColors { D2D1_COLOR_F fill, border, text; };
inline ButtonColors Button(const ThemePalette& theme, bool primary, bool danger, bool hovered, bool pressed) {
    if (primary) {
        auto fill = danger ? theme.red : theme.accent;
        fill = LerpColor(fill, pressed ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1), pressed ? 0.15f : hovered ? 0.08f : 0.0f);
        return {fill, fill, ForegroundOn(fill)};
    }
    return {LerpColor(theme.surfaceElevated, theme.text, pressed ? 0.10f : hovered ? 0.06f : 0.0f),
            pressed ? theme.focusRing : theme.border, theme.text};
}

struct PlaybackDiagnosticsLayout { D2D1_RECT_F card{}, close{}; bool visible = false; };
inline PlaybackDiagnosticsLayout PlaybackDiagnostics(const D2D1_RECT_F& bounds, float dp) {
    const float width = std::min(420.0f * dp, RectWidth(bounds) - 32.0f * dp);
    const float height = std::min(450.0f * dp, RectHeight(bounds) - 80.0f * dp);
    if (width < 200.0f || height < 150.0f) return {};
    const auto card = D2D1::RectF(bounds.left + 20.0f * dp, bounds.top + 70.0f * dp,
        bounds.left + 20.0f * dp + width, bounds.top + 70.0f * dp + height);
    return {card, D2D1::RectF(card.right - 38.0f * dp, card.top, card.right - 2.0f * dp, card.top + 36.0f * dp), true};
}

// In-app diagnostics log viewer (clone of the diagnostics panel geometry).
struct LogViewerLayout { D2D1_RECT_F card{}, close{}; bool visible = false; };
inline LogViewerLayout LogViewer(const D2D1_RECT_F& bounds, float dp) {
    const float width = std::min(560.0f * dp, RectWidth(bounds) - 32.0f * dp);
    const float height = std::min(430.0f * dp, RectHeight(bounds) - 80.0f * dp);
    if (width < 240.0f || height < 160.0f) return {};
    const auto card = D2D1::RectF(bounds.left + 20.0f * dp, bounds.top + 70.0f * dp,
        bounds.left + 20.0f * dp + width, bounds.top + 70.0f * dp + height);
    return {card, D2D1::RectF(card.right - 38.0f * dp, card.top, card.right - 2.0f * dp, card.top + 36.0f * dp), true};
}

struct PlayerLayout {
    D2D1_RECT_F back, prev, play, next, mute, volume, subtitles, settings, pip, fullscreen;
    bool showVolume, showSubtitles, showPip;
};
inline PlayerLayout Player(float width, float height, float dp, bool queuePlayback = false) {
    PlayerLayout l{};
    const float size = 36 * dp, pad = PlayerPadding * dp;
    const float y = height - 76 * dp + PlayerControlsY * dp;
    const auto button = [&](float x) { return D2D1::RectF(x, y, x + size, y + size); };
    l.back = D2D1::RectF(24 * dp, 16 * dp, 60 * dp, 52 * dp);
    float transportX = pad;
    if (queuePlayback) { l.prev = button(transportX); transportX += size + 6 * dp; }
    l.play = button(transportX);
    transportX += size + 6 * dp;
    l.next = button(transportX);
    transportX += size + 8 * dp;
    l.mute = button(transportX);
    l.showVolume = l.showSubtitles = width >= 720 * dp;
    l.showPip = width >= 520 * dp;
    if (l.showVolume) l.volume = D2D1::RectF(l.mute.right + 4 * dp, y, l.mute.right + 64 * dp, y + size);
    float x = width - pad - size;
    l.fullscreen = button(x); x -= size + 6 * dp;
    if (l.showPip) { l.pip = button(x); x -= size + 6 * dp; }
    l.settings = button(x); x -= size + 6 * dp;
    if (l.showSubtitles) l.subtitles = button(x);
    return l;
}

struct PiPLayout {
    D2D1_RECT_F topActions, previous, next, audio, pin, restore, close;
    D2D1_RECT_F controls, playPause, timeline, timelineHit, time;
    D2D1_RECT_F volumeSlider, volumeTrack;
    bool showTime = false;
};

inline PiPLayout PiP(float width, float height, float dp) {
    PiPLayout l{};
    dp = std::max(0.5f, dp);
    const float outer = 10.0f * dp, topBtnSize = 28.0f * dp, topPad = 3.0f * dp, topGap = 4.0f * dp;
    // 1. Restore full window button is located on the top left (sleek, scaled down)
    l.restore = D2D1::RectF(outer, outer, outer + topBtnSize + topPad * 2.0f, outer + topBtnSize + topPad * 2.0f);
    // 2. Top right actions: audio, pin, close
    const float topWidth = topPad * 2.0f + topBtnSize * 3.0f + topGap * 2.0f;
    l.topActions = D2D1::RectF(std::max(l.restore.right + 8.0f * dp, width - outer - topWidth), outer,
        std::max(outer + topWidth, width - outer), outer + topBtnSize + topPad * 2.0f);
    float actionX = l.topActions.left + topPad;
    const float actionY = l.topActions.top + topPad;
    const auto action = [&]() {
        D2D1_RECT_F rect = D2D1::RectF(actionX, actionY, actionX + topBtnSize, actionY + topBtnSize);
        actionX += topBtnSize + topGap;
        return rect;
    };
    l.audio = action();
    l.pin = action();
    l.close = action();

    // 3. YouTube-style vertical volume slider directly beneath the audio button
    const float sliderW = 32.0f * dp;
    const float sliderH = 104.0f * dp;
    const float sliderCx = (l.audio.left + l.audio.right) * 0.5f;
    const float sliderTop = l.audio.bottom + 4.0f * dp;
    l.volumeSlider = D2D1::RectF(sliderCx - sliderW * 0.5f, sliderTop, sliderCx + sliderW * 0.5f, sliderTop + sliderH);
    const float trackPadV = 14.0f * dp;
    l.volumeTrack = D2D1::RectF(sliderCx - 10.0f * dp, sliderTop + trackPadV, sliderCx + 10.0f * dp, sliderTop + sliderH - trackPadV);

    // 4. Bottom controls: [Previous] [Play/Pause] [Next] on the left, then timeline, then compact timer
    const float controlHeight = 40.0f * dp;
    l.controls = D2D1::RectF(outer, std::max(outer, height - outer - controlHeight),
        std::max(outer + 1.0f, width - outer), std::max(outer + controlHeight, height - outer));
    const float btnSize = 28.0f * dp;
    const float btnGap = 4.0f * dp;
    const float bottomBtnY = l.controls.top + (RectHeight(l.controls) - btnSize) * 0.5f;

    float curX = l.controls.left + 4.0f * dp;
    l.previous = D2D1::RectF(curX, bottomBtnY, curX + btnSize, bottomBtnY + btnSize);
    curX += btnSize + btnGap;
    l.playPause = D2D1::RectF(curX, bottomBtnY, curX + btnSize, bottomBtnY + btnSize);
    curX += btnSize + btnGap;
    l.next = D2D1::RectF(curX, bottomBtnY, curX + btnSize, bottomBtnY + btnSize);
    curX += btnSize + 8.0f * dp;

    l.showTime = width >= 340.0f * dp;
    const float timeWidth = l.showTime ? 46.0f * dp : 0.0f;
    const float timelineLeft = curX;
    const float timelineRight = std::max(timelineLeft + 1.0f * dp, l.controls.right - 6.0f * dp - timeWidth);
    const float timelineY = (l.controls.top + l.controls.bottom) * 0.5f;
    l.timeline = D2D1::RectF(timelineLeft, timelineY - 1.5f * dp, timelineRight, timelineY + 1.5f * dp);
    l.timelineHit = D2D1::RectF(timelineLeft, timelineY - 11.0f * dp, timelineRight, timelineY + 11.0f * dp);
    if (l.showTime) { l.time = D2D1::RectF(timelineRight + 6.0f * dp, l.controls.top, l.controls.right - 2.0f * dp, l.controls.bottom); }
    return l;
}

inline std::wstring NextLabel(bool queue, size_t index, size_t count) {
    return Loc::Tr(queue && count > 0 ? (index + 1 >= count ? "ux.queue_restart" : "ux.queue_next") : "ux.library_next");
}

inline std::wstring StorageGuidance(const StorageStatus& status) {
    if (status.win32Error == ERROR_DISK_FULL || status.win32Error == ERROR_HANDLE_DISK_FULL) return Loc::Tr("ux.storage_full");
    if (status.code == StorageErrorCode::FileReadFailed || status.code == StorageErrorCode::EmptyData) return Loc::Tr("ux.storage_read");
    if (status.code == StorageErrorCode::ReadOnlyMedia || status.code == StorageErrorCode::DirectoryNotWritable ||
        status.win32Error == ERROR_ACCESS_DENIED || status.win32Error == ERROR_WRITE_PROTECT)
        return Loc::Tr("ux.storage_permission");
    return Loc::Tr("ux.storage_retry");
}
}
