#pragma once

#include "Common.h"
#include "Settings.h"
#include "PlaybackSession.h"
#include "CommandDispatcher.h"
#include "MediaLibrary.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <atomic>
#include <memory>

namespace Aperture {

// Stable taskbar thumbnail button IDs
constexpr UINT kTaskbarButtonPrev = 1001;
constexpr UINT kTaskbarButtonPlayPause = 1002;
constexpr UINT kTaskbarButtonNext = 1003;

struct TaskbarSnapshot {
    PlaybackState playbackState = PlaybackState::Closed;
    bool isPreviousEnabled = false, isPlayPauseEnabled = false, isNextEnabled = false, isPlaying = false;
    std::wstring playPauseTooltip = L"Play";
    std::wstring prevTooltip = L"Previous";
    std::wstring nextTooltip = L"Next";
    TBPFLAG progressState = TBPF_NOPROGRESS;
    ULONGLONG progressValue = 0;
    ULONGLONG progressTotal = 0;
    bool HasSameButtons(const TaskbarSnapshot& other) const {
        return isPreviousEnabled == other.isPreviousEnabled &&
               isPlayPauseEnabled == other.isPlayPauseEnabled && isNextEnabled == other.isNextEnabled && isPlaying == other.isPlaying &&
               playPauseTooltip == other.playPauseTooltip && prevTooltip == other.prevTooltip && nextTooltip == other.nextTooltip;
    }
    bool HasSameProgress(const TaskbarSnapshot& other) const {
        return progressState == other.progressState && progressValue == other.progressValue && progressTotal == other.progressTotal;
    }
    bool operator==(const TaskbarSnapshot& other) const {
        return playbackState == other.playbackState && HasSameButtons(other) && HasSameProgress(other);
    }
    bool operator!=(const TaskbarSnapshot& other) const { return !(*this == other); }
};

struct TaskbarProgress { TBPFLAG state = TBPF_NOPROGRESS; ULONGLONG value = 0; ULONGLONG total = 0; };

class TaskbarController {
public:
    TaskbarController();
    ~TaskbarController();
    // Pure Projection & Translation for testing and runtime logic
    static TaskbarSnapshot ProjectState( const PlaybackSession& session, const MediaLibrary& library, const Settings& settings,
        double currentTime, double duration );
    static TaskbarProgress ProjectProgress( PlaybackState playbackState, double currentTime, double duration );
    static AppCommand MapButtonToCommand(UINT buttonId);
    // Lifecycle & Initialization
    bool Initialize(HWND hWnd);
    bool EnsureInitialized(HWND hWnd);
    void Shutdown();
    bool IsInitialized() const { return isInitialized; }
    bool AreButtonsAdded() const { return buttonsAdded; }
    // State Synchronization
    void Update(const TaskbarSnapshot& snapshot, bool force = false);
    void Clear();
    // Window Message Handling (WM_COMMAND / TaskbarButtonCreated)
    bool HandleWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, CommandDispatcher& dispatcher);
    void OnTaskbarButtonCreated(HWND hWnd);
    // Snapshot Inspection
    const TaskbarSnapshot& GetLastSnapshot() const { return lastSnapshot; }
    // Icon Management (public for testing / DPI changes)
    void CreateIcons(int width = 0, int height = 0);
    void DestroyIcons();
    // Injection / test helper
    void SetTaskbarListForTesting(Microsoft::WRL::ComPtr<ITaskbarList3> mockList, HWND mockHwnd = (HWND)1);

private:
    void EnsureIconsCreated();
    bool SyncThumbnailButtons(HWND hWnd, const TaskbarSnapshot& snapshot, bool add);
    void ApplyProgress(HWND hWnd, const TaskbarSnapshot& snapshot, bool force);
    static HICON CreateGlyphIcon(int width, int height, int glyphType);
    HWND targetHwnd = NULL;
    bool isInitialized = false, buttonsAdded = false;
    UINT taskbarCreatedMessage = 0;
    Microsoft::WRL::ComPtr<ITaskbarList3> taskbarList;
    HICON hIconPrev = NULL;
    HICON hIconPlay = NULL;
    HICON hIconPause = NULL;
    HICON hIconNext = NULL;
    int currentIconWidth = 0, currentIconHeight = 0;
    TaskbarSnapshot lastSnapshot;
};

} // namespace Aperture
