#pragma once

#include "../support/TestFramework.h"
#include "TestCommandDispatcher.h"
#include "../../src/TaskbarController.h"
#include "../../src/PlaybackSession.h"
#include "../../src/CommandDispatcher.h"
#include "../../src/MediaLibrary.h"
#include "../../src/Settings.h"
#include "../../src/MpvPlayer.h"

#include <windows.h>
#include <shobjidl.h>
#include <vector>
#include <cmath>

namespace Aperture::Test {

// Mock ITaskbarList3 for unit testing thumbnail buttons and progress interactions
class MockTaskbarList3 : public ITaskbarList3 {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG rc = --refCount;
        if (rc == 0) delete this;
        return rc;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ITaskbarList || riid == IID_ITaskbarList2 || riid == IID_ITaskbarList3) {
            *ppvObject = static_cast<ITaskbarList3*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    // ITaskbarList
    HRESULT STDMETHODCALLTYPE HrInit() override {
        hrInitCalled = true;
        return hrInitResult;
    }
    HRESULT STDMETHODCALLTYPE AddTab(HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteTab(HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ActivateTab(HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetActiveAlt(HWND) override { return S_OK; }

    // ITaskbarList2
    HRESULT STDMETHODCALLTYPE MarkFullscreenWindow(HWND, BOOL) override { return S_OK; }

    // ITaskbarList3
    HRESULT STDMETHODCALLTYPE SetProgressValue(HWND hWnd, ULONGLONG ullCompleted, ULONGLONG ullTotal) override {
        lastProgressCompleted = ullCompleted;
        lastProgressTotal = ullTotal;
        setProgressValueCount++;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetProgressState(HWND hWnd, TBPFLAG tbpFlags) override {
        lastProgressState = tbpFlags;
        setProgressStateCount++;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterTab(HWND, HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnregisterTab(HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetTabOrder(HWND, HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetTabActive(HWND, HWND, DWORD) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE ThumbBarAddButtons(HWND hWnd, UINT cButtons, LPTHUMBBUTTON pButton) override {
        thumbBarAddButtonsCount++;
        addedButtons.clear();
        if (pButton && cButtons > 0) {
            addedButtons.assign(pButton, pButton + cButtons);
        }
        return thumbBarAddResult;
    }

    HRESULT STDMETHODCALLTYPE ThumbBarUpdateButtons(HWND hWnd, UINT cButtons, LPTHUMBBUTTON pButton) override {
        thumbBarUpdateButtonsCount++;
        updatedButtons.clear();
        if (pButton && cButtons > 0) {
            updatedButtons.assign(pButton, pButton + cButtons);
        }
        return thumbBarUpdateResult;
    }

    HRESULT STDMETHODCALLTYPE ThumbBarSetImageList(HWND, HIMAGELIST) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOverlayIcon(HWND, HICON, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetThumbnailTooltip(HWND, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetThumbnailClip(HWND, RECT*) override { return S_OK; }

    // Inspection fields
    std::atomic<ULONG> refCount{ 1 };
    bool hrInitCalled = false;
    HRESULT hrInitResult = S_OK;
    HRESULT thumbBarAddResult = S_OK;
    HRESULT thumbBarUpdateResult = S_OK;

    int setProgressValueCount = 0;
    int setProgressStateCount = 0;
    int thumbBarAddButtonsCount = 0;
    int thumbBarUpdateButtonsCount = 0;

    TBPFLAG lastProgressState = TBPF_NOPROGRESS;
    ULONGLONG lastProgressCompleted = 0;
    ULONGLONG lastProgressTotal = 0;

    std::vector<THUMBBUTTON> addedButtons;
    std::vector<THUMBBUTTON> updatedButtons;
};

// 1. Command Mapping Tests
static void step_TaskbarController_CommandMapping() {
    TEST_ASSERT_EQ(static_cast<int>(TaskbarController::MapButtonToCommand(kTaskbarButtonPrev)),
                   static_cast<int>(AppCommand::PlayPrevious));
    TEST_ASSERT_EQ(static_cast<int>(TaskbarController::MapButtonToCommand(kTaskbarButtonPlayPause)),
                   static_cast<int>(AppCommand::TogglePlay));
    TEST_ASSERT_EQ(static_cast<int>(TaskbarController::MapButtonToCommand(kTaskbarButtonNext)),
                   static_cast<int>(AppCommand::PlayNext));
    TEST_ASSERT_EQ(static_cast<int>(TaskbarController::MapButtonToCommand(0)),
                   static_cast<int>(AppCommand::None));
    TEST_ASSERT_EQ(static_cast<int>(TaskbarController::MapButtonToCommand(9999)),
                   static_cast<int>(AppCommand::None));
}

// 2. Capability and State Projection Tests
static void step_TaskbarController_CapabilityAndStateProjection() {
    PlaybackSession session;
    MediaLibrary library;
    Settings settings;

    // Closed session projection
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 0.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Closed));
        TEST_ASSERT(!s.isPlayPauseEnabled);
        TEST_ASSERT(!s.isPreviousEnabled);
        TEST_ASSERT(!s.isNextEnabled);
        TEST_ASSERT(!s.isPlaying);
        TEST_ASSERT_EQ(s.playPauseTooltip, std::wstring(L"Play"));
        TEST_ASSERT_EQ(s.progressState, TBPF_NOPROGRESS);
    }

    // Direct open playing media
    std::wstring mediaPath = L"C:\\Media\\TestVideo.mp4";
    session.OpenDirect(mediaPath, 0.0);
    session.OnPlayerOpened(true);

    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 25.0, 100.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Playing));
        TEST_ASSERT(s.isPlaying);
        TEST_ASSERT(s.isPlayPauseEnabled);
        TEST_ASSERT_EQ(s.playPauseTooltip, std::wstring(L"Pause"));
        TEST_ASSERT_EQ(s.progressState, TBPF_NORMAL);
        TEST_ASSERT_EQ(s.progressValue, 25000ULL);
        TEST_ASSERT_EQ(s.progressTotal, 100000ULL);
    }

    // Paused state projection
    session.SetPlayState(false);
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 25.0, 100.0);
        TEST_ASSERT_EQ(static_cast<int>(s.playbackState), static_cast<int>(PlaybackState::Paused));
        TEST_ASSERT(!s.isPlaying);
        TEST_ASSERT(s.isPlayPauseEnabled);
        TEST_ASSERT_EQ(s.playPauseTooltip, std::wstring(L"Play"));
        TEST_ASSERT_EQ(s.progressState, TBPF_PAUSED);
        TEST_ASSERT_EQ(s.progressValue, 25000ULL);
        TEST_ASSERT_EQ(s.progressTotal, 100000ULL);
    }

    // Queue Navigation Capabilities (no loop)
    settings.loop = false;
    std::vector<std::wstring> queue = {
        L"C:\\Media\\track1.mp3",
        L"C:\\Media\\track2.mp3",
        L"C:\\Media\\track3.mp3"
    };
    session.OpenQueue(queue, 0); // At first track
    session.OnPlayerOpened(true);

    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(!s.isPreviousEnabled); // First track, loop=false
        TEST_ASSERT(s.isNextEnabled);
    }

    session.OpenQueue(queue, 1);
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isPreviousEnabled);
        TEST_ASSERT(s.isNextEnabled);
    }

    session.OpenQueue(queue, 2);
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isPreviousEnabled);
        TEST_ASSERT(!s.isNextEnabled); // Last track, loop=false
    }

    // Queue with loop enabled
    settings.loop = true;
    session.OpenQueue(queue, 2);
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isPreviousEnabled);
        TEST_ASSERT(s.isNextEnabled); // Enabled due to loop
    }

    session.OpenQueue(queue, 0);
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isPreviousEnabled); // Enabled due to loop
        TEST_ASSERT(s.isNextEnabled);
    }

    // Library Navigation Capabilities
    session.OpenDirect(mediaPath, 0.0);
    session.OnPlayerOpened(true);

    // Empty or single item library
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(!s.isPreviousEnabled);
        TEST_ASSERT(!s.isNextEnabled);
    }

    // Library with > 1 items
    library.AddOrUpdateMedia(L"C:\\Media\\item1.mp4");
    library.AddOrUpdateMedia(L"C:\\Media\\item2.mp4");
    {
        TaskbarSnapshot s = TaskbarController::ProjectState(session, library, settings, 0.0, 60.0);
        TEST_ASSERT(s.isPreviousEnabled);
        TEST_ASSERT(s.isNextEnabled);
    }
}

// 3. Progress Projection and Bounds Testing
static void step_TaskbarController_ProgressProjectionAndBounds() {
    // Closed / Stopped
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Closed, 10.0, 100.0);
        TEST_ASSERT_EQ(p.state, TBPF_NOPROGRESS);
        TEST_ASSERT_EQ(p.value, 0ULL);
        TEST_ASSERT_EQ(p.total, 0ULL);
    }
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Error, 10.0, 100.0);
        TEST_ASSERT_EQ(p.state, TBPF_NOPROGRESS);
        TEST_ASSERT_EQ(p.value, 0ULL);
        TEST_ASSERT_EQ(p.total, 0ULL);
    }

    // Indeterminate / Stream (duration <= 0 or infinite/NaN)
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Playing, 10.0, 0.0);
        TEST_ASSERT_EQ(p.state, TBPF_INDETERMINATE);
        TEST_ASSERT_EQ(p.value, 0ULL);
        TEST_ASSERT_EQ(p.total, 0ULL);
    }
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Playing, 10.0, -50.0);
        TEST_ASSERT_EQ(p.state, TBPF_INDETERMINATE);
    }
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Paused, 10.0, 0.0);
        TEST_ASSERT_EQ(p.state, TBPF_NOPROGRESS);
    }

    // Normal Playing Progress. Values are quantized to ~0.5% steps so the
    // snapshot change guard in Update() holds between rendered frames (the
    // taskbar overlay receives updates only when the bucket changes).
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Playing, 30.5, 120.0);
        TEST_ASSERT_EQ(p.state, TBPF_NORMAL);
        TEST_ASSERT_EQ(p.total, 120000ULL);
        TEST_ASSERT_EQ(p.value, 30000ULL); // 30500 ms floors into the 600 ms (0.5%) bucket
        // Positions within the same bucket project identically: the per-frame
        // guard must not observe a change until the bucket actually moves.
        TaskbarProgress sameBucket = TaskbarController::ProjectProgress(PlaybackState::Playing, 30.4, 120.0);
        TEST_ASSERT_EQ(sameBucket.value, p.value);   // 30400 ms is in the same 600 ms bucket
        TaskbarProgress nextBucket = TaskbarController::ProjectProgress(PlaybackState::Playing, 30.7, 120.0);
        TEST_ASSERT(nextBucket.value > p.value);      // 30700 ms crosses into the next bucket
        // Endpoints stay exact.
        TaskbarProgress start = TaskbarController::ProjectProgress(PlaybackState::Playing, 0.0, 120.0);
        TaskbarProgress end = TaskbarController::ProjectProgress(PlaybackState::Playing, 120.0, 120.0);
        TEST_ASSERT_EQ(start.value, 0ULL);
        TEST_ASSERT_EQ(end.value, p.total);
    }

    // Paused Progress
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Paused, 50.0, 100.0);
        TEST_ASSERT_EQ(p.state, TBPF_PAUSED);
        TEST_ASSERT_EQ(p.value, 50000ULL);
        TEST_ASSERT_EQ(p.total, 100000ULL);
    }

    // Bounds Clamping: Position < 0
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Playing, -10.0, 100.0);
        TEST_ASSERT_EQ(p.state, TBPF_NORMAL);
        TEST_ASSERT_EQ(p.value, 0ULL);
        TEST_ASSERT_EQ(p.total, 100000ULL);
    }

    // Bounds Clamping: Position > Duration
    {
        TaskbarProgress p = TaskbarController::ProjectProgress(PlaybackState::Playing, 150.0, 100.0);
        TEST_ASSERT_EQ(p.state, TBPF_NORMAL);
        TEST_ASSERT_EQ(p.value, 100000ULL);
        TEST_ASSERT_EQ(p.total, 100000ULL);
    }
}

// 4. Failed Initialization and Degradation
static void step_TaskbarController_FailedInitializationAndDegradation() {
    TaskbarController controller;
    TEST_ASSERT(!controller.IsInitialized());
    TEST_ASSERT(!controller.AreButtonsAdded());

    // Update with NULL HWND and no COM should degrade gracefully
    TaskbarSnapshot snapshot;
    snapshot.playbackState = PlaybackState::Playing;
    snapshot.isPlaying = true;
    snapshot.isPlayPauseEnabled = true;
    snapshot.progressState = TBPF_NORMAL;

    controller.Update(snapshot);
    TEST_ASSERT(!controller.IsInitialized());
    TEST_ASSERT(!controller.AreButtonsAdded());

    // Clear should not crash
    controller.Clear();

    // Shutdown should not crash
    controller.Shutdown();
    TEST_ASSERT(!controller.IsInitialized());
}

// 5. Mock ITaskbarList3 Integration (Add, Update, Progress)
static void step_TaskbarController_MockTaskbarListIntegration() {
    TaskbarController controller;
    Microsoft::WRL::ComPtr<MockTaskbarList3> mock = new MockTaskbarList3();
    HWND fakeHwnd = reinterpret_cast<HWND>(0x12345);

    controller.SetTaskbarListForTesting(mock, fakeHwnd);
    TEST_ASSERT(controller.IsInitialized());
    TEST_ASSERT(!controller.AreButtonsAdded());

    TaskbarSnapshot snapshot;
    snapshot.playbackState = PlaybackState::Playing;
    snapshot.isPlaying = true;
    snapshot.isPlayPauseEnabled = true;
    snapshot.isPreviousEnabled = true;
    snapshot.isNextEnabled = true;
    snapshot.playPauseTooltip = L"Pause";
    snapshot.prevTooltip = L"Previous Track";
    snapshot.nextTooltip = L"Next Track";
    snapshot.progressState = TBPF_NORMAL;
    snapshot.progressValue = 15000;
    snapshot.progressTotal = 60000;

    // First update -> AddThumbnailButtons
    controller.Update(snapshot);
    TEST_ASSERT(controller.AreButtonsAdded());
    TEST_ASSERT_EQ(mock->thumbBarAddButtonsCount, 1);
    TEST_ASSERT_EQ(mock->thumbBarUpdateButtonsCount, 0);
    TEST_ASSERT_EQ(mock->addedButtons.size(), 3ULL);
    TEST_ASSERT_EQ(mock->addedButtons[0].iId, kTaskbarButtonPrev);
    TEST_ASSERT_EQ(mock->addedButtons[1].iId, kTaskbarButtonPlayPause);
    TEST_ASSERT_EQ(mock->addedButtons[2].iId, kTaskbarButtonNext);
    TEST_ASSERT_EQ(mock->lastProgressState, TBPF_NORMAL);
    TEST_ASSERT_EQ(mock->lastProgressCompleted, 15000ULL);
    TEST_ASSERT_EQ(mock->lastProgressTotal, 60000ULL);

    // Second update with changed snapshot -> UpdateThumbnailButtons
    snapshot.playbackState = PlaybackState::Paused;
    snapshot.isPlaying = false;
    snapshot.playPauseTooltip = L"Play";
    snapshot.progressState = TBPF_PAUSED;
    snapshot.progressValue = 20000;

    controller.Update(snapshot);
    TEST_ASSERT_EQ(mock->thumbBarAddButtonsCount, 1);
    TEST_ASSERT_EQ(mock->thumbBarUpdateButtonsCount, 1);
    TEST_ASSERT_EQ(mock->updatedButtons.size(), 3ULL);
    TEST_ASSERT_EQ(mock->lastProgressState, TBPF_PAUSED);
    TEST_ASSERT_EQ(mock->lastProgressCompleted, 20000ULL);

    // Explorer restart event
    controller.OnTaskbarButtonCreated(fakeHwnd);
    TEST_ASSERT(controller.AreButtonsAdded()); // Update is triggered inside OnTaskbarButtonCreated
    TEST_ASSERT_EQ(mock->thumbBarAddButtonsCount, 2); // Re-added after restart

    // Teardown
    controller.Shutdown();
    TEST_ASSERT(!controller.IsInitialized());
    TEST_ASSERT(!controller.AreButtonsAdded());
    TEST_ASSERT_EQ(mock->lastProgressState, TBPF_NOPROGRESS);
}

// 6. Window Message WM_COMMAND Dispatching Tests
static void step_TaskbarController_WindowMessageHandling() {
    TaskbarController controller;
    MockCommandContext context;
    CommandDispatcher dispatcher(&context);
    std::vector<std::wstring> queue = { L"C:\\Media\\one.mp4", L"C:\\Media\\two.mp4" };
    context.session.OpenQueue(queue, 0);
    context.session.OnPlayerOpened(true);

    HWND fakeHwnd = reinterpret_cast<HWND>(0x12345);

    // Click Previous Button
    WPARAM wparamPrev = MAKEWPARAM(kTaskbarButtonPrev, THBN_CLICKED);
    bool handled = controller.HandleWindowMessage(fakeHwnd, WM_COMMAND, wparamPrev, 0, dispatcher);
    TEST_ASSERT(handled);
    TEST_ASSERT(context.playPrevInQueueCalled);

    // Click Play/Pause Button
    WPARAM wparamPlay = MAKEWPARAM(kTaskbarButtonPlayPause, THBN_CLICKED);
    handled = controller.HandleWindowMessage(fakeHwnd, WM_COMMAND, wparamPlay, 0, dispatcher);
    TEST_ASSERT(handled);
    TEST_ASSERT(context.playPauseBadgeTriggered);

    // Click Next Button
    WPARAM wparamNext = MAKEWPARAM(kTaskbarButtonNext, THBN_CLICKED);
    handled = controller.HandleWindowMessage(fakeHwnd, WM_COMMAND, wparamNext, 0, dispatcher);
    TEST_ASSERT(handled);
    TEST_ASSERT(context.playNextInQueueCalled);

    // Non-taskbar command (different notification code)
    WPARAM wparamOtherNotif = MAKEWPARAM(kTaskbarButtonPlayPause, 0);
    handled = controller.HandleWindowMessage(fakeHwnd, WM_COMMAND, wparamOtherNotif, 0, dispatcher);
    TEST_ASSERT(!handled);

    // Non-taskbar command ID
    WPARAM wparamOtherId = MAKEWPARAM(9999, THBN_CLICKED);
    handled = controller.HandleWindowMessage(fakeHwnd, WM_COMMAND, wparamOtherId, 0, dispatcher);
    TEST_ASSERT(!handled);

    // Different window message
    handled = controller.HandleWindowMessage(fakeHwnd, WM_KEYDOWN, VK_SPACE, 0, dispatcher);
    TEST_ASSERT(!handled);
}

// 7. Icon Creation and Destruction Lifecycle
static void step_TaskbarController_IconLifecycle() {
    TaskbarController controller;
    Microsoft::WRL::ComPtr<MockTaskbarList3> mock;
    mock.Attach(new MockTaskbarList3());
    controller.SetTaskbarListForTesting(mock, reinterpret_cast<HWND>(0x12345));
    TaskbarSnapshot playing;
    playing.playbackState = PlaybackState::Playing;
    playing.isPlaying = playing.isPlayPauseEnabled = true;
    for (int size : {16, 24, 32}) {
        controller.CreateIcons(size, size);
        controller.Update(playing, true);
        const auto& buttons = mock->updatedButtons.empty() ? mock->addedButtons : mock->updatedButtons;
        TEST_ASSERT_EQ(buttons.size(), size_t(3));
        for (const auto& button : buttons) {
            ICONINFO info{};
            const bool valid = GetIconInfo(button.hIcon, &info) != FALSE;
            TEST_ASSERT(valid);
            if (!valid) continue;
            BITMAP bitmap{};
            TEST_ASSERT(GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) != 0);
            // Publishing uses the shell's current icon metrics.
            TEST_ASSERT_EQ(bitmap.bmWidth, GetSystemMetrics(SM_CXSMICON));
            TEST_ASSERT_EQ(bitmap.bmHeight, GetSystemMetrics(SM_CYSMICON));
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
        }
    }
    const auto icons = mock->updatedButtons;
    controller.DestroyIcons();
    controller.DestroyIcons();
    for (const auto& button : icons) {
        ICONINFO info{};
        const bool valid = GetIconInfo(button.hIcon, &info) != FALSE;
        TEST_ASSERT(!valid);
        if (valid) { DeleteObject(info.hbmColor); DeleteObject(info.hbmMask); }
    }
}
static void step_TaskbarController_IdleClearAndProgressOnlyUpdatesAvoidExplorerCalls() {
    TaskbarController controller;
    Microsoft::WRL::ComPtr<MockTaskbarList3> mock;
    mock.Attach(new MockTaskbarList3());
    HWND hwnd = reinterpret_cast<HWND>(0x12345);
    controller.SetTaskbarListForTesting(mock, hwnd);
    TaskbarSnapshot playing;
    playing.playbackState = PlaybackState::Playing;
    playing.isPlaying = playing.isPlayPauseEnabled = true;
    playing.progressState = TBPF_NORMAL;
    playing.progressTotal = 100000;
    controller.Update(playing);
    const int initialStates = mock->setProgressStateCount;
    for (int frame = 1; frame <= 120; ++frame) {
        playing.progressValue = frame * 500;
        controller.Update(playing);
    }
    TEST_ASSERT_EQ(mock->thumbBarAddButtonsCount, 1);
    TEST_ASSERT_EQ(mock->thumbBarUpdateButtonsCount, 0);
    TEST_ASSERT_EQ(mock->setProgressStateCount, initialStates);
    TEST_ASSERT_EQ(mock->setProgressValueCount, 121);
    controller.Clear();
    const int clears = mock->setProgressStateCount;
    const int buttons = mock->thumbBarUpdateButtonsCount;
    for (int frame = 0; frame < 600; ++frame) controller.Clear();
    TEST_ASSERT_EQ(mock->setProgressStateCount, clears);
    TEST_ASSERT_EQ(mock->thumbBarUpdateButtonsCount, buttons);
    TEST_ASSERT_EQ(mock->lastProgressState, TBPF_NOPROGRESS);
    controller.OnTaskbarButtonCreated(hwnd);
    TEST_ASSERT_EQ(mock->thumbBarAddButtonsCount, 2);
    TEST_ASSERT_EQ(mock->setProgressStateCount, clears + 1);
}


TEST_CASE(TaskbarController, MockTaskbarAndExplorerThrottling) {
    step_TaskbarController_CommandMapping();
    step_TaskbarController_CapabilityAndStateProjection();
    step_TaskbarController_ProgressProjectionAndBounds();
    step_TaskbarController_FailedInitializationAndDegradation();
    step_TaskbarController_MockTaskbarListIntegration();
    step_TaskbarController_WindowMessageHandling();
    step_TaskbarController_IconLifecycle();
    step_TaskbarController_IdleClearAndProgressOnlyUpdatesAvoidExplorerCalls();
}

} // namespace Aperture::Test
