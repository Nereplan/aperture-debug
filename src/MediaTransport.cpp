#include "MediaTransport.h"
#include <roapi.h>
#include <winstring.h>
#include <algorithm>
#include <cmath>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Media;
using namespace ABI::Windows::Foundation;

namespace Aperture {

MediaTransport::MediaTransport() = default;

MediaTransport::~MediaTransport() { Shutdown(); }

MediaTransportSnapshot MediaTransport::ProjectState( const PlaybackSession& session, const MediaLibrary& library, const Settings& settings,
    double currentTime, double duration ) {
    MediaTransportSnapshot snapshot;
    const PlaybackState state = session.GetState();
    snapshot.playbackState = state;
    if (state == PlaybackState::Closed) {
        snapshot.isPlayEnabled = false;
        snapshot.isPauseEnabled = false;
        snapshot.isStopEnabled = false;
        snapshot.isNextEnabled = false;
        snapshot.isPreviousEnabled = false;
        snapshot.isSeekEnabled = false;
        return snapshot;
    }
    const std::wstring& filePath = session.GetCurrentFilePath();
    const MediaItem* item = library.FindByPath(filePath);
    if (item && !item->title.empty()) { snapshot.title = item->title; } else { snapshot.title = GetFileTitleFromPath(filePath); }
    // Artist or subtitle projection: use artist when metadata exists, or filename as fallback
    std::wstring artistName;
    if (item && item->extraFields.is_object() && item->extraFields.as_object().count("artist")) {
        const auto& artVal = item->extraFields.as_object().at("artist");
        if (artVal.is_string()) { artistName = Utf8ToUtf16(artVal.as_string()); }
    }
    if (artistName.empty()) { artistName = GetFileNameFromPath(filePath); }
    snapshot.artist = artistName;
    // Duration & position
    double safeDuration = duration > 0.0 ? duration : (item ? item->durationSec : 0.0);
    if (safeDuration < 0.0 || !std::isfinite(safeDuration)) safeDuration = 0.0;
    snapshot.durationSec = safeDuration;
    double safePosition = currentTime >= 0.0 ? currentTime : session.GetPlaybackPosition();
    if (safePosition < 0.0 || !std::isfinite(safePosition)) safePosition = 0.0;
    if (safeDuration > 0.0 && safePosition > safeDuration) safePosition = safeDuration;
    snapshot.positionSec = safePosition;
    // Capabilities
    snapshot.isStopEnabled = true;
    snapshot.isSeekEnabled = safeDuration > 0.0;
    if (state == PlaybackState::Playing) {
        snapshot.isPlayEnabled = false;
        snapshot.isPauseEnabled = true;
    } else if (state == PlaybackState::Paused) {
        snapshot.isPlayEnabled = true;
        snapshot.isPauseEnabled = false;
    } else { snapshot.isPlayEnabled = true; snapshot.isPauseEnabled = false; }
    // Next / Previous navigation capabilities
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
    return snapshot;
}

AppCommand MediaTransport::MapButtonToCommand(SystemMediaTransportControlsButton button) {
    switch (button) {
        case SystemMediaTransportControlsButton_Play: return AppCommand::Play;
        case SystemMediaTransportControlsButton_Pause: return AppCommand::Pause;
        case SystemMediaTransportControlsButton_Stop: return AppCommand::ClosePlayer;
        case SystemMediaTransportControlsButton_Next: return AppCommand::PlayNext;
        case SystemMediaTransportControlsButton_Previous: return AppCommand::PlayPrevious;
        default: return AppCommand::None;
    }
}

MediaTransportTimeline MediaTransport::ComputeTimelineProperties(double currentTime, double duration) {
    MediaTransportTimeline timeline;
    double safeDuration = (duration > 0.0 && std::isfinite(duration)) ? duration : 0.0;
    double safePosition = (currentTime >= 0.0 && std::isfinite(currentTime)) ? currentTime : 0.0;
    if (safePosition > safeDuration) { safePosition = safeDuration; }
    timeline.startTimeSec = 0.0;
    timeline.endTimeSec = safeDuration;
    timeline.minSeekTimeSec = 0.0;
    timeline.maxSeekTimeSec = safeDuration;
    timeline.positionSec = safePosition;
    return timeline;
}

double MediaTransport::TimeSpanToSeconds(INT64 duration100ns) {
    if (duration100ns <= 0) return 0.0;
    return static_cast<double>(duration100ns) / 10000000.0;
}

INT64 MediaTransport::SecondsToTimeSpan(double seconds) {
    if (seconds <= 0.0 || !std::isfinite(seconds)) return 0;
    return static_cast<INT64>(seconds * 10000000.0 + 0.5);
}

bool MediaTransport::Initialize(HWND hWnd) {
    if (isInitialized && targetHwnd == hWnd) return true;
    Shutdown();
    if (!hWnd || !IsWindow(hWnd)) { return false; }
    targetHwnd = hWnd;
    callbacksDisabled.store(false, std::memory_order_release);
    HStringReference classId(RuntimeClass_Windows_Media_SystemMediaTransportControls);
    ComPtr<ISystemMediaTransportControlsInterop> interop;
    HRESULT hr = RoGetActivationFactory(classId.Get(), IID_PPV_ARGS(&interop));
    if (FAILED(hr) || !interop) { return false; }
    hr = interop->GetForWindow(targetHwnd, IID_PPV_ARGS(&smtc));
    if (FAILED(hr) || !smtc) { return false; }
    smtc.As(&smtc2);
    hr = smtc->get_DisplayUpdater(&displayUpdater);
    if (FAILED(hr) || !displayUpdater) { smtc.Reset(); smtc2.Reset(); return false; }
    RegisterEventHandlers();
    isInitialized = true;
    isEnabled = true;
    return true;
}

bool MediaTransport::EnsureInitialized(HWND hWnd) { if (isInitialized) return true; return Initialize(hWnd); }

void MediaTransport::Shutdown() {
    SetCallbacksDisabled(true);
    UnregisterEventHandlers();
    ClearPendingActions();
    if (smtc) { smtc->put_IsEnabled(false); smtc->put_PlaybackStatus(MediaPlaybackStatus_Closed); }
    if (displayUpdater) { displayUpdater->ClearAll(); displayUpdater->Update(); }
    cachedTimelineProps.Reset();
    displayUpdater.Reset();
    smtc2.Reset();
    smtc.Reset();
    isInitialized = false;
    isEnabled = false;
    targetHwnd = NULL;
    lastSnapshot = MediaTransportSnapshot{};
    lastTimelineUpdateTimestampMs = 0;
    lastPublishedPositionSec = -1.0;
}

void MediaTransport::RegisterEventHandlers() {
    if (!smtc) return;
    auto buttonCallback = Callback<ITypedEventHandler<SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>>(
        [this](ISystemMediaTransportControls* /*sender*/, ISystemMediaTransportControlsButtonPressedEventArgs* args) -> HRESULT {
            if (callbacksDisabled.load(std::memory_order_acquire)) return S_OK;
            if (!args) return S_OK;
            SystemMediaTransportControlsButton btn;
            if (SUCCEEDED(args->get_Button(&btn))) {
                AppCommand cmd = MapButtonToCommand(btn);
                if (cmd != AppCommand::None) { QueueAction(cmd); }
            }
            return S_OK;
        } );
    smtc->add_ButtonPressed(buttonCallback.Get(), &buttonPressedToken);
    if (smtc2) {
        auto posCallback = Callback<ITypedEventHandler<SystemMediaTransportControls*, PlaybackPositionChangeRequestedEventArgs*>>(
            [this](ISystemMediaTransportControls* /*sender*/, IPlaybackPositionChangeRequestedEventArgs* args) -> HRESULT {
                if (callbacksDisabled.load(std::memory_order_acquire)) return S_OK;
                if (!args) return S_OK;
                TimeSpan requestedPos = {};
                if (SUCCEEDED(args->get_RequestedPlaybackPosition(&requestedPos))) {
                    double targetSec = TimeSpanToSeconds(requestedPos.Duration);
                    CommandArgs cmdArgs;
                    cmdArgs.doubleVal = targetSec;
                    QueueAction(AppCommand::SeekTo, cmdArgs);
                }
                return S_OK;
            } );
        smtc2->add_PlaybackPositionChangeRequested(posCallback.Get(), &positionChangeToken);
    }
}

void MediaTransport::UnregisterEventHandlers() {
    if (smtc && buttonPressedToken.value != 0) {
        smtc->remove_ButtonPressed(buttonPressedToken);
        buttonPressedToken = {};
    }
    if (smtc2 && positionChangeToken.value != 0) {
        smtc2->remove_PlaybackPositionChangeRequested(positionChangeToken);
        positionChangeToken = {};
    }
}

void MediaTransport::Update(const MediaTransportSnapshot& snapshot, bool force) {
    if (!isInitialized) { lastSnapshot = snapshot; return; }
    const bool stateChanged = (lastSnapshot.playbackState != snapshot.playbackState);
    const bool metadataChanged = (lastSnapshot.title != snapshot.title || lastSnapshot.artist != snapshot.artist ||
                                  lastSnapshot.durationSec != snapshot.durationSec);
    const bool capsChanged =
        (lastSnapshot.isPlayEnabled != snapshot.isPlayEnabled || lastSnapshot.isPauseEnabled != snapshot.isPauseEnabled ||
         lastSnapshot.isStopEnabled != snapshot.isStopEnabled || lastSnapshot.isNextEnabled != snapshot.isNextEnabled ||
                              lastSnapshot.isPreviousEnabled != snapshot.isPreviousEnabled);
    if (force || stateChanged || metadataChanged || capsChanged) {
        ApplySnapshotToSmtc(snapshot);
        lastSnapshot = snapshot;
        lastTimelineUpdateTimestampMs = GetTickCount64();
        lastPublishedPositionSec = snapshot.positionSec;
        return;
    }
    // Periodic / rate-limited position update
    const ULONGLONG now = GetTickCount64();
    const bool intervalElapsed = (now - lastTimelineUpdateTimestampMs >= kMinTimelineUpdateIntervalMs);
    const bool positionDrifted = (std::abs(snapshot.positionSec - lastPublishedPositionSec) >= 1.0);
    if (intervalElapsed && positionDrifted) {
        ApplyTimelineToSmtc(ComputeTimelineProperties(snapshot.positionSec, snapshot.durationSec));
        lastSnapshot.positionSec = snapshot.positionSec;
        lastTimelineUpdateTimestampMs = now;
        lastPublishedPositionSec = snapshot.positionSec;
    }
}

void MediaTransport::Clear() {
    MediaTransportSnapshot emptySnapshot;
    emptySnapshot.playbackState = PlaybackState::Closed;
    lastSnapshot = emptySnapshot;
    lastPublishedPositionSec = -1.0;
    lastTimelineUpdateTimestampMs = 0;
    if (!isInitialized || !smtc) return;
    smtc->put_IsEnabled(false);
    smtc->put_PlaybackStatus(MediaPlaybackStatus_Closed);
    smtc->put_IsPlayEnabled(false);
    smtc->put_IsPauseEnabled(false);
    smtc->put_IsStopEnabled(false);
    smtc->put_IsNextEnabled(false);
    smtc->put_IsPreviousEnabled(false);
    if (displayUpdater) { displayUpdater->ClearAll(); displayUpdater->Update(); }
    ApplyTimelineToSmtc(ComputeTimelineProperties(0.0, 0.0));
}

void MediaTransport::ApplySnapshotToSmtc(const MediaTransportSnapshot& snapshot) {
    if (!smtc) return;
    const bool isPlaybackActive = (snapshot.playbackState != PlaybackState::Closed);
    smtc->put_IsEnabled(isPlaybackActive);
    MediaPlaybackStatus status = MediaPlaybackStatus_Closed;
    switch (snapshot.playbackState) {
        case PlaybackState::Playing: status = MediaPlaybackStatus_Playing; break;
        case PlaybackState::Paused: status = MediaPlaybackStatus_Paused; break;
        case PlaybackState::Initializing: status = MediaPlaybackStatus_Changing; break;
        case PlaybackState::Error: status = MediaPlaybackStatus_Stopped; break;
        case PlaybackState::Closed:
        default: status = MediaPlaybackStatus_Closed; break;
    }
    smtc->put_PlaybackStatus(status);
    smtc->put_IsPlayEnabled(snapshot.isPlayEnabled);
    smtc->put_IsPauseEnabled(snapshot.isPauseEnabled);
    smtc->put_IsStopEnabled(snapshot.isStopEnabled);
    smtc->put_IsNextEnabled(snapshot.isNextEnabled);
    smtc->put_IsPreviousEnabled(snapshot.isPreviousEnabled);
    if (displayUpdater && isPlaybackActive) {
        displayUpdater->put_Type(MediaPlaybackType_Video);
        ComPtr<IVideoDisplayProperties> videoProps;
        if (SUCCEEDED(displayUpdater->get_VideoProperties(&videoProps)) && videoProps) {
            HStringReference hTitle(snapshot.title.c_str());
            videoProps->put_Title(hTitle.Get());
            HStringReference hArtist(snapshot.artist.c_str());
            videoProps->put_Subtitle(hArtist.Get());
        }
        displayUpdater->Update();
    } else if (displayUpdater && !isPlaybackActive) { displayUpdater->ClearAll(); displayUpdater->Update(); }
    ApplyTimelineToSmtc(ComputeTimelineProperties(snapshot.positionSec, snapshot.durationSec));
}

void MediaTransport::ApplyTimelineToSmtc(const MediaTransportTimeline& timeline) {
    if (!smtc2) return;
    if (!cachedTimelineProps) {
        HStringReference classId(RuntimeClass_Windows_Media_SystemMediaTransportControlsTimelineProperties);
        ComPtr<IInspectable> inspectable;
        HRESULT hr = RoActivateInstance(classId.Get(), &inspectable);
        if (FAILED(hr) || !inspectable) { return; }
        hr = inspectable.As(&cachedTimelineProps);
        if (FAILED(hr) || !cachedTimelineProps) { cachedTimelineProps.Reset(); return; }
    }
    TimeSpan startTime = { SecondsToTimeSpan(timeline.startTimeSec) };
    TimeSpan endTime = { SecondsToTimeSpan(timeline.endTimeSec) };
    TimeSpan minSeek = { SecondsToTimeSpan(timeline.minSeekTimeSec) };
    TimeSpan maxSeek = { SecondsToTimeSpan(timeline.maxSeekTimeSec) };
    TimeSpan position = { SecondsToTimeSpan(timeline.positionSec) };
    cachedTimelineProps->put_StartTime(startTime);
    cachedTimelineProps->put_EndTime(endTime);
    cachedTimelineProps->put_MinSeekTime(minSeek);
    cachedTimelineProps->put_MaxSeekTime(maxSeek);
    cachedTimelineProps->put_Position(position);
    smtc2->UpdateTimelineProperties(cachedTimelineProps.Get());
}

void MediaTransport::QueueAction(AppCommand cmd, const CommandArgs& args) {
    if (callbacksDisabled.load(std::memory_order_acquire)) { return; }
    {
        std::lock_guard<std::mutex> lock(actionMutex);
        pendingActions.push_back(TransportAction{ cmd, args });
    }
    if (targetHwnd) {
        bool expected = false;
        if (actionPosted.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (!PostMessageW(targetHwnd, kMediaTransportEventMessage, 0, 0)) { actionPosted.store(false, std::memory_order_release); }
        }
    }
}

size_t MediaTransport::ProcessPendingCommands(CommandDispatcher& dispatcher) {
    if (callbacksDisabled.load(std::memory_order_acquire)) { ClearPendingActions(); return 0; }
    std::deque<TransportAction> actions;
    { std::lock_guard<std::mutex> lock(actionMutex); actions.swap(pendingActions); }
    actionPosted.store(false, std::memory_order_release);
    size_t executedCount = 0;
    for (const auto& action : actions) {
        if (action.command != AppCommand::None) { dispatcher.Execute(action.command, action.args); executedCount++; }
    }
    return executedCount;
}

size_t MediaTransport::GetPendingActionCount() const { std::lock_guard<std::mutex> lock(actionMutex); return pendingActions.size(); }

void MediaTransport::ClearPendingActions() {
    std::lock_guard<std::mutex> lock(actionMutex);
    pendingActions.clear();
    actionPosted.store(false, std::memory_order_release);
}

void MediaTransport::SetCallbacksDisabled(bool disabled) {
    callbacksDisabled.store(disabled, std::memory_order_release);
    if (disabled) { ClearPendingActions(); }
}

} // namespace Aperture
