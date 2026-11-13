#include "TaskbarController.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef THBN_CLICKED
#define THBN_CLICKED 0x1800
#endif

namespace Aperture {

TaskbarController::TaskbarController() = default;

TaskbarController::~TaskbarController() { Shutdown(); }

TaskbarProgress TaskbarController::ProjectProgress( PlaybackState playbackState, double currentTime, double duration ) {
    TaskbarProgress progress;
    if (playbackState == PlaybackState::Closed || playbackState == PlaybackState::Error) {
        progress.state = TBPF_NOPROGRESS;
        progress.value = 0;
        progress.total = 0;
        return progress;
    }
    if (duration <= 0.0 || !std::isfinite(duration)) {
        if (playbackState == PlaybackState::Playing) { progress.state = TBPF_INDETERMINATE; } else { progress.state = TBPF_NOPROGRESS; }
        progress.value = 0;
        progress.total = 0;
        return progress;
    }
    double safeDuration = duration;
    double safePosition = (currentTime >= 0.0 && std::isfinite(currentTime)) ? currentTime : 0.0;
    if (safePosition > safeDuration) { safePosition = safeDuration; }
    progress.value = static_cast<ULONGLONG>(safePosition * 1000.0);
    progress.total = static_cast<ULONGLONG>(safeDuration * 1000.0);
    if (progress.total == 0) { progress.total = 1; }
    // Quantize to ~0.5% steps: ms-resolution progress changes every frame,
    // which defeats the snapshot equality guard in Update() and issues three
    // cross-process COM calls to Explorer per rendered frame during playback.
    const ULONGLONG progressStep = std::max<ULONGLONG>(1, progress.total / 200);
    progress.value = (progress.value / progressStep) * progressStep;
    if (playbackState == PlaybackState::Playing) { progress.state = TBPF_NORMAL; } else if (playbackState == PlaybackState::Paused) {
        progress.state = TBPF_PAUSED;
    } else { progress.state = TBPF_NOPROGRESS; }
    return progress;
}

TaskbarSnapshot TaskbarController::ProjectState( const PlaybackSession& session, const MediaLibrary& library, const Settings& settings,
    double currentTime, double duration ) {
    TaskbarSnapshot snapshot;
    const PlaybackState state = session.GetState();
    snapshot.playbackState = state;
    if (state == PlaybackState::Closed) {
        snapshot.isPreviousEnabled = false;
        snapshot.isPlayPauseEnabled = false;
        snapshot.isNextEnabled = false;
        snapshot.isPlaying = false;
        snapshot.playPauseTooltip = L"Play";
        snapshot.prevTooltip = L"Previous";
        snapshot.nextTooltip = L"Next";
        snapshot.progressState = TBPF_NOPROGRESS;
        snapshot.progressValue = 0;
        snapshot.progressTotal = 0;
        return snapshot;
    }
    snapshot.isPlaying = (state == PlaybackState::Playing);
    snapshot.isPlayPauseEnabled = true;
    snapshot.playPauseTooltip = snapshot.isPlaying ? L"Pause" : L"Play";
    snapshot.prevTooltip = L"Previous";
    snapshot.nextTooltip = L"Next";
    if (session.IsQueuePlayback()) {
        const size_t queueSize = session.GetQueueSize();
        const size_t queueIndex = session.GetQueueIndex();
        snapshot.isNextEnabled = (queueSize > 0 && (queueIndex + 1 < queueSize || settings.loop));
        snapshot.isPreviousEnabled = (queueSize > 0 && (queueIndex > 0 || settings.loop));
    } else {
        const size_t libCount = library.mediaItems.size();
        snapshot.isNextEnabled = (libCount > 1);
        snapshot.isPreviousEnabled = (libCount > 1);
    }
    TaskbarProgress prog = ProjectProgress(state, currentTime, duration);
    snapshot.progressState = prog.state;
    snapshot.progressValue = prog.value;
    snapshot.progressTotal = prog.total;
    return snapshot;
}

AppCommand TaskbarController::MapButtonToCommand(UINT buttonId) {
    switch (buttonId) {
        case kTaskbarButtonPrev: return AppCommand::PlayPrevious;
        case kTaskbarButtonPlayPause: return AppCommand::TogglePlay;
        case kTaskbarButtonNext: return AppCommand::PlayNext;
        default: return AppCommand::None;
    }
}

bool TaskbarController::Initialize(HWND hWnd) {
    targetHwnd = hWnd;
    if (taskbarCreatedMessage == 0) { taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarButtonCreated"); }
    return EnsureInitialized(hWnd);
}

bool TaskbarController::EnsureInitialized(HWND hWnd) {
    if (taskbarCreatedMessage == 0) { taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarButtonCreated"); }
    if (hWnd) { targetHwnd = hWnd; }
    if (isInitialized && taskbarList) { return true; }
    if (!targetHwnd) { return false; }
    HRESULT hr = CoCreateInstance( CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbarList) );
    if (FAILED(hr) || !taskbarList) { taskbarList.Reset(); isInitialized = false; return false; }
    hr = taskbarList->HrInit();
    if (FAILED(hr)) { taskbarList.Reset(); isInitialized = false; return false; }
    isInitialized = true;
    EnsureIconsCreated();
    return true;
}

void TaskbarController::Shutdown() {
    if (taskbarList && targetHwnd) { taskbarList->SetProgressState(targetHwnd, TBPF_NOPROGRESS); }
    DestroyIcons();
    taskbarList.Reset();
    isInitialized = false;
    buttonsAdded = false;
    targetHwnd = NULL;
    lastSnapshot = {};
}

void TaskbarController::SetTaskbarListForTesting(Microsoft::WRL::ComPtr<ITaskbarList3> mockList, HWND mockHwnd) {
    taskbarList = mockList;
    targetHwnd = mockHwnd;
    isInitialized = (taskbarList != nullptr);
    buttonsAdded = false;
}

void TaskbarController::Update(const TaskbarSnapshot& snapshot, bool force) {
    if (!EnsureInitialized(targetHwnd)) { lastSnapshot = snapshot; return; }
    if (!force && buttonsAdded && snapshot == lastSnapshot) { return; }
    const bool initialUpdate = !buttonsAdded;
    if (force || initialUpdate || !snapshot.HasSameButtons(lastSnapshot)) {
        EnsureIconsCreated();
        if (SyncThumbnailButtons(targetHwnd, snapshot, initialUpdate)) buttonsAdded = true;
    }
    if (force || initialUpdate || !snapshot.HasSameProgress(lastSnapshot)) {
        ApplyProgress(targetHwnd, snapshot, force || initialUpdate);
    }
    lastSnapshot = snapshot;
}

void TaskbarController::UpdateState(const PlaybackSession& session, const MediaLibrary& library, const Settings& settings,
                                    double currentTime, double duration, bool force) {
    const PlaybackState state = session.GetState();
    if (state == PlaybackState::Closed) { Clear(); return; }
    const bool isPlaying = (state == PlaybackState::Playing);
    constexpr bool isPlayPauseEnabled = true;
    bool isNextEnabled = false;
    bool isPreviousEnabled = false;
    if (session.IsQueuePlayback()) {
        const size_t queueSize = session.GetQueueSize();
        const size_t queueIndex = session.GetQueueIndex();
        isNextEnabled = (queueSize > 0 && (queueIndex + 1 < queueSize || settings.loop));
        isPreviousEnabled = (queueSize > 0 && (queueIndex > 0 || settings.loop));
    } else {
        const size_t libCount = library.mediaItems.size();
        isNextEnabled = (libCount > 1);
        isPreviousEnabled = (libCount > 1);
    }
    if (!force && isInitialized && taskbarList && buttonsAdded &&
        lastSnapshot.playbackState == state &&
        lastSnapshot.isPlaying == isPlaying &&
        lastSnapshot.isPlayPauseEnabled == isPlayPauseEnabled &&
        lastSnapshot.isPreviousEnabled == isPreviousEnabled &&
        lastSnapshot.isNextEnabled == isNextEnabled &&
        lastSnapshot.playPauseTooltip == (isPlaying ? L"Pause" : L"Play") &&
        lastSnapshot.prevTooltip == L"Previous" &&
        lastSnapshot.nextTooltip == L"Next") {
        const TaskbarProgress prog = ProjectProgress(state, currentTime, duration);
        if (prog.state == lastSnapshot.progressState &&
            prog.value == lastSnapshot.progressValue &&
            prog.total == lastSnapshot.progressTotal) {
            return;
        }
        ApplyProgressValues(targetHwnd, prog.state, prog.value, prog.total, false);
        lastSnapshot.progressState = prog.state;
        lastSnapshot.progressValue = prog.value;
        lastSnapshot.progressTotal = prog.total;
        return;
    }
    Update(ProjectState(session, library, settings, currentTime, duration), force);
}

void TaskbarController::Clear() {
    // The hub calls this on each update. Never initialize a service just to
    // clear it, and publish the closed state only once after playback ends.
    if (!isInitialized) { lastSnapshot = {}; return; }
    Update(TaskbarSnapshot{});
}

bool TaskbarController::HandleWindowMessage( HWND hWnd, UINT uMsg, WPARAM wParam,
    LPARAM /* lParam */,
    CommandDispatcher& dispatcher ) {
    if (taskbarCreatedMessage != 0 && uMsg == taskbarCreatedMessage) { OnTaskbarButtonCreated(hWnd); return true; }
    if (uMsg == WM_COMMAND) {
        if (HIWORD(wParam) == THBN_CLICKED) {
            UINT buttonId = LOWORD(wParam);
            AppCommand cmd = MapButtonToCommand(buttonId);
            if (cmd != AppCommand::None) { dispatcher.Execute(cmd); return true; }
        }
    }
    return false;
}

void TaskbarController::OnTaskbarButtonCreated(HWND hWnd) {
    targetHwnd = hWnd;
    buttonsAdded = false; // Toolbar must be re-added on Explorer restart
    if (taskbarList) { Update(lastSnapshot, true); }
}

void TaskbarController::EnsureIconsCreated() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;
    if (!hIconPrev || !hIconPlay || !hIconPause || !hIconNext || currentIconWidth != cx || currentIconHeight != cy) { CreateIcons(cx, cy); }
}

void TaskbarController::CreateIcons(int width, int height) {
    DestroyIcons();
    if (width <= 0) width = GetSystemMetrics(SM_CXSMICON);
    if (height <= 0) height = GetSystemMetrics(SM_CYSMICON);
    if (width <= 0) width = 16;
    if (height <= 0) height = 16;
    currentIconWidth = width;
    currentIconHeight = height;
    hIconPrev = CreateGlyphIcon(width, height, 0);
    hIconPlay = CreateGlyphIcon(width, height, 1);
    hIconPause = CreateGlyphIcon(width, height, 2);
    hIconNext = CreateGlyphIcon(width, height, 3);
}

void TaskbarController::DestroyIcons() {
    if (hIconPrev) { DestroyIcon(hIconPrev); hIconPrev = NULL; }
    if (hIconPlay) { DestroyIcon(hIconPlay); hIconPlay = NULL; }
    if (hIconPause) { DestroyIcon(hIconPause); hIconPause = NULL; }
    if (hIconNext) { DestroyIcon(hIconNext); hIconNext = NULL; }
    currentIconWidth = 0;
    currentIconHeight = 0;
}

bool TaskbarController::SyncThumbnailButtons(HWND hWnd, const TaskbarSnapshot& snapshot, bool add) {
    if (!taskbarList || !hWnd) { return false; }
    THUMBBUTTON buttons[3] = {};
    // Previous button
    buttons[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[0].iId = kTaskbarButtonPrev;
    buttons[0].hIcon = hIconPrev;
    wcsncpy_s(buttons[0].szTip, snapshot.prevTooltip.c_str(), _TRUNCATE);
    buttons[0].dwFlags = snapshot.isPreviousEnabled ? THBF_ENABLED : THBF_DISABLED;
    // Play/Pause button
    buttons[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[1].iId = kTaskbarButtonPlayPause;
    buttons[1].hIcon = snapshot.isPlaying ? hIconPause : hIconPlay;
    wcsncpy_s(buttons[1].szTip, snapshot.playPauseTooltip.c_str(), _TRUNCATE);
    buttons[1].dwFlags = snapshot.isPlayPauseEnabled ? THBF_ENABLED : THBF_DISABLED;
    // Next button
    buttons[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[2].iId = kTaskbarButtonNext;
    buttons[2].hIcon = hIconNext;
    wcsncpy_s(buttons[2].szTip, snapshot.nextTooltip.c_str(), _TRUNCATE);
    buttons[2].dwFlags = snapshot.isNextEnabled ? THBF_ENABLED : THBF_DISABLED;
    HRESULT hr = add ? taskbarList->ThumbBarAddButtons(hWnd, 3, buttons)
                     : taskbarList->ThumbBarUpdateButtons(hWnd, 3, buttons);
    return SUCCEEDED(hr);
}

void TaskbarController::ApplyProgress(HWND hWnd, const TaskbarSnapshot& snapshot, bool force) {
    ApplyProgressValues(hWnd, snapshot.progressState, snapshot.progressValue, snapshot.progressTotal, force);
}

void TaskbarController::ApplyProgressValues(HWND hWnd, TBPFLAG state, ULONGLONG value, ULONGLONG total, bool force) {
    if (!taskbarList || !hWnd) { return; }
    if (force || state != lastSnapshot.progressState)
        taskbarList->SetProgressState(hWnd, state);
    if (state == TBPF_NORMAL || state == TBPF_PAUSED || state == TBPF_ERROR)
        taskbarList->SetProgressValue(hWnd, value, total);
}

// Glyph rasterization helper
HICON TaskbarController::CreateGlyphIcon(int width, int height, int glyphType) {
    if (width <= 0 || height <= 0) { return NULL; }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    uint32_t* pixels = nullptr;
    HDC hdcScreen = GetDC(NULL);
    HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), NULL, 0);
    ReleaseDC(NULL, hdcScreen);
    if (!hBitmap || !pixels) { return NULL; }
    // Geometry layout in [0..width] x [0..height]
    float padX = width * 0.18f, padY = height * 0.18f, left = padX, right = width - padX;
    float top = padY, bottom = height - padY;
    float cy = (top + bottom) * 0.5f;
    float barWidth = std::max(1.0f, (right - left) * 0.22f);
    float gap = std::max(1.0f, (right - left) * 0.12f);
    auto PointInTriangle = [](float px, float py, float x1, float y1, float x2, float y2, float x3, float y3) -> bool {
        float d1 = (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2);
        float d2 = (px - x3) * (y2 - y3) - (x2 - x3) * (py - y3);
        float d3 = (px - x1) * (y3 - y1) - (x3 - x1) * (py - y1);
        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(hasNeg && hasPos);
    };
    auto SampleGlyph = [&](float px, float py) -> bool {
        switch (glyphType) {
            case 0: { // Previous: bar on left, left-pointing triangle
                if (px >= left && px <= left + barWidth && py >= top && py <= bottom) { return true; }
                float triLeft = left + barWidth + gap;
                if (PointInTriangle(px, py, triLeft, cy, right, top, right, bottom)) { return true; }
                return false;
            }
            case 1: { // Play: right-pointing triangle
                return PointInTriangle(px, py, left + 1.0f, top, left + 1.0f, bottom, right, cy);
            }
            case 2: { // Pause: two vertical bars
                if (px >= left && px <= left + barWidth && py >= top && py <= bottom) { return true; }
                if (px >= right - barWidth && px <= right && py >= top && py <= bottom) { return true; }
                return false;
            }
            case 3: { // Next: right-pointing triangle, bar on right
                float triRight = right - barWidth - gap;
                if (PointInTriangle(px, py, left, top, left, bottom, triRight, cy)) { return true; }
                if (px >= right - barWidth && px <= right && py >= top && py <= bottom) { return true; }
                return false;
            }
            default: return false;
        }
    };
    // 2x2 subpixel anti-aliasing
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float subSamples[4][2] = {{x + 0.25f, y + 0.25f}, {x + 0.75f, y + 0.25f}, {x + 0.25f, y + 0.75f}, {x + 0.75f, y + 0.75f}};
            int hits = 0;
            for (int i = 0; i < 4; ++i) {
                if (SampleGlyph(subSamples[i][0], subSamples[i][1])) { hits++; }
            }
            if (hits > 0) {
                uint8_t alpha = static_cast<uint8_t>((hits / 4.0f) * 255.0f);
                // Premultiplied ARGB: pure white with alpha
                pixels[y * width + x] = (static_cast<uint32_t>(alpha) << 24) |
                                        (static_cast<uint32_t>(alpha) << 16) |
                                        (static_cast<uint32_t>(alpha) << 8) |
                                        static_cast<uint32_t>(alpha);
            } else { pixels[y * width + x] = 0; }
        }
    }
    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    if (!hMask) { DeleteObject(hBitmap); return NULL; }
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = hMask;
    ii.hbmColor = hBitmap;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hBitmap);
    DeleteObject(hMask);
    return hIcon;
}

} // namespace Aperture
