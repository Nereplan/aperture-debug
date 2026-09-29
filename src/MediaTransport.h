#pragma once

#include "Common.h"
#include "Settings.h"
#include "PlaybackSession.h"
#include "CommandDispatcher.h"
#include "MediaLibrary.h"

#include <windows.h>
#include <windows.media.h>
#include <systemmediatransportcontrolsinterop.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>

namespace Aperture {

struct MediaTransportSnapshot {
    std::wstring title, artist, albumTitle;
    double durationSec = 0.0, positionSec = 0.0;
    PlaybackState playbackState = PlaybackState::Closed;
    bool isPlayEnabled = false, isPauseEnabled = false, isStopEnabled = false, isNextEnabled = false;
    bool isPreviousEnabled = false, isSeekEnabled = false;
    bool operator==(const MediaTransportSnapshot& other) const {
        return title == other.title && artist == other.artist && albumTitle == other.albumTitle && durationSec == other.durationSec &&
               positionSec == other.positionSec && playbackState == other.playbackState && isPlayEnabled == other.isPlayEnabled &&
               isPauseEnabled == other.isPauseEnabled && isStopEnabled == other.isStopEnabled && isNextEnabled == other.isNextEnabled &&
               isPreviousEnabled == other.isPreviousEnabled && isSeekEnabled == other.isSeekEnabled;
    }
    bool operator!=(const MediaTransportSnapshot& other) const { return !(*this == other); }
};

struct MediaTransportTimeline {
    double startTimeSec = 0.0, endTimeSec = 0.0, minSeekTimeSec = 0.0, maxSeekTimeSec = 0.0;
    double positionSec = 0.0;
};

struct TransportAction { AppCommand command = AppCommand::None; CommandArgs args; };

class MediaTransport {
public:
    MediaTransport();
    ~MediaTransport();
    // Pure Projection & Translation (for testing and runtime logic)
    static MediaTransportSnapshot ProjectState( const PlaybackSession& session, const MediaLibrary& library, const Settings& settings,
        double currentTime, double duration );
    static AppCommand MapButtonToCommand(ABI::Windows::Media::SystemMediaTransportControlsButton button);
    static MediaTransportTimeline ComputeTimelineProperties(double currentTime, double duration);
    static double TimeSpanToSeconds(INT64 duration100ns);
    static INT64 SecondsToTimeSpan(double seconds);
    // Lifecycle & Initialization
    bool Initialize(HWND hWnd);
    bool EnsureInitialized(HWND hWnd);
    void Shutdown();
    bool IsInitialized() const { return isInitialized; }
    bool IsEnabled() const { return isEnabled; }
    // State Synchronization
    void Update(const MediaTransportSnapshot& snapshot, bool force = false);
    void Clear();
    // Callback Queue & Marshaling
    void QueueAction(AppCommand cmd, const CommandArgs& args = {});
    size_t ProcessPendingCommands(CommandDispatcher& dispatcher);
    size_t GetPendingActionCount() const;
    void ClearPendingActions();
    void SetCallbacksDisabled(bool disabled);
    bool AreCallbacksDisabled() const { return callbacksDisabled.load(std::memory_order_acquire); }
    // Snapshot Inspection
    const MediaTransportSnapshot& GetLastSnapshot() const { return lastSnapshot; }
    // Window Message Identifier for App Marshaling
    static constexpr UINT kMediaTransportEventMessage = WM_APP + 0x4E;

private:
    void RegisterEventHandlers();
    void UnregisterEventHandlers();
    void ApplySnapshotToSmtc(const MediaTransportSnapshot& snapshot);
    void ApplyTimelineToSmtc(const MediaTransportTimeline& timeline);
    HWND targetHwnd = NULL;
    bool isInitialized = false, isEnabled = false;
    std::atomic<bool> callbacksDisabled{ false };
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControls> smtc;
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControls2> smtc2;
    Microsoft::WRL::ComPtr<ABI::Windows::Media::ISystemMediaTransportControlsDisplayUpdater> displayUpdater;
    EventRegistrationToken buttonPressedToken = {};
    EventRegistrationToken positionChangeToken = {};
    MediaTransportSnapshot lastSnapshot;
    ULONGLONG lastTimelineUpdateTimestampMs = 0;
    double lastPublishedPositionSec = -1.0;
    static constexpr ULONGLONG kMinTimelineUpdateIntervalMs = 500;
    mutable std::mutex actionMutex;
    std::deque<TransportAction> pendingActions;
    std::atomic<bool> actionPosted{ false };
};

} // namespace Aperture
