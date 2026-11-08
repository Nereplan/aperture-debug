#if !defined(_WIN32)

#include "../MediaTransport.h"
#include <dlfcn.h>
#include <algorithm>
#include <cmath>

namespace Aperture {

namespace {

// Dynamic libdbus-1 loader for MPRIS2 (org.mpris.MediaPlayer2.aperture) on KDE Plasma / GNOME / Hyprland
struct DbusMprisRuntime {
    void* lib = nullptr;
    void* conn = nullptr;
    bool active = false;

    using DBusError = struct { const char* name; const char* message; unsigned int dummy[5]; void* pad; };

    void (*dbus_error_init)(DBusError* error) = nullptr;
    void (*dbus_error_free)(DBusError* error) = nullptr;
    int (*dbus_error_is_set)(const DBusError* error) = nullptr;
    void* (*dbus_bus_get)(int type, DBusError* error) = nullptr;
    int (*dbus_bus_request_name)(void* connection, const char* name, unsigned int flags, DBusError* error) = nullptr;
    void (*dbus_connection_unref)(void* connection) = nullptr;
    void (*dbus_connection_flush)(void* connection) = nullptr;

    void Initialize() {
        if (lib) return;
        for (const char* name : {"libdbus-1.so.3", "libdbus-1.so"}) {
            lib = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return;
        dbus_error_init = reinterpret_cast<decltype(dbus_error_init)>(::dlsym(lib, "dbus_error_init"));
        dbus_error_free = reinterpret_cast<decltype(dbus_error_free)>(::dlsym(lib, "dbus_error_free"));
        dbus_error_is_set = reinterpret_cast<decltype(dbus_error_is_set)>(::dlsym(lib, "dbus_error_is_set"));
        dbus_bus_get = reinterpret_cast<decltype(dbus_bus_get)>(::dlsym(lib, "dbus_bus_get"));
        dbus_bus_request_name = reinterpret_cast<decltype(dbus_bus_request_name)>(::dlsym(lib, "dbus_bus_request_name"));
        dbus_connection_unref = reinterpret_cast<decltype(dbus_connection_unref)>(::dlsym(lib, "dbus_connection_unref"));
        dbus_connection_flush = reinterpret_cast<decltype(dbus_connection_flush)>(::dlsym(lib, "dbus_connection_flush"));

        if (dbus_error_init && dbus_bus_get && dbus_bus_request_name) {
            DBusError err{};
            dbus_error_init(&err);
            conn = dbus_bus_get(0 /*DBUS_BUS_SESSION*/, &err);
            if (conn && (!dbus_error_is_set || !dbus_error_is_set(&err))) {
                dbus_bus_request_name(conn, "org.mpris.MediaPlayer2.aperture", 4 /*DBUS_NAME_FLAG_DO_NOT_QUEUE*/, &err);
                active = true;
            }
            if (dbus_error_free) dbus_error_free(&err);
        }
    }

    void Shutdown() {
        active = false;
        if (conn && dbus_connection_unref) {
            dbus_connection_unref(conn);
        }
        conn = nullptr;
    }
};

DbusMprisRuntime& GetMpris() {
    static DbusMprisRuntime instance;
    return instance;
}

} // namespace

MediaTransport::MediaTransport() = default;

MediaTransport::~MediaTransport() {
    Shutdown();
}

MediaTransportSnapshot MediaTransport::ProjectState(
    const PlaybackSession& session,
    const MediaLibrary& library,
    const Settings& settings,
    double currentTime,
    double duration) {
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
    if (item && !item->title.empty()) {
        snapshot.title = item->title;
    } else {
        snapshot.title = GetFileTitleFromPath(filePath);
    }
    std::wstring artistName;
    if (item && item->extraFields.is_object() && item->extraFields.as_object().count("artist")) {
        const auto& artVal = item->extraFields.as_object().at("artist");
        if (artVal.is_string()) {
            artistName = Utf8ToUtf16(artVal.as_string());
        }
    }
    if (artistName.empty()) {
        artistName = GetFileNameFromPath(filePath);
    }
    snapshot.artist = artistName;
    double safeDuration = duration > 0.0 ? duration : (item ? item->durationSec : 0.0);
    if (safeDuration < 0.0 || !std::isfinite(safeDuration)) safeDuration = 0.0;
    snapshot.durationSec = safeDuration;
    double safePosition = currentTime >= 0.0 ? currentTime : session.GetPlaybackPosition();
    if (safePosition < 0.0 || !std::isfinite(safePosition)) safePosition = 0.0;
    if (safeDuration > 0.0 && safePosition > safeDuration) safePosition = safeDuration;
    snapshot.positionSec = safePosition;
    snapshot.isStopEnabled = true;
    snapshot.isSeekEnabled = safeDuration > 0.0;
    if (state == PlaybackState::Playing) {
        snapshot.isPlayEnabled = false;
        snapshot.isPauseEnabled = true;
    } else if (state == PlaybackState::Paused) {
        snapshot.isPlayEnabled = true;
        snapshot.isPauseEnabled = false;
    } else {
        snapshot.isPlayEnabled = true;
        snapshot.isPauseEnabled = false;
    }
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

AppCommand MediaTransport::MapButtonToCommand(ABI::Windows::Media::SystemMediaTransportControlsButton button) {
    using namespace ABI::Windows::Media;
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
    MediaTransportTimeline tl;
    double dur = (std::isfinite(duration) && duration > 0.0) ? duration : 0.0;
    double pos = (std::isfinite(currentTime) && currentTime >= 0.0) ? currentTime : 0.0;
    if (dur > 0.0 && pos > dur) pos = dur;
    tl.startTimeSec = 0.0;
    tl.endTimeSec = dur;
    tl.minSeekTimeSec = 0.0;
    tl.maxSeekTimeSec = dur;
    tl.positionSec = pos;
    return tl;
}

double MediaTransport::TimeSpanToSeconds(INT64 duration100ns) {
    if (duration100ns <= 0) return 0.0;
    return static_cast<double>(duration100ns) / 10000000.0;
}

INT64 MediaTransport::SecondsToTimeSpan(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
    return static_cast<INT64>(std::llround(seconds * 10000000.0));
}

bool MediaTransport::Initialize(HWND hWnd) {
    if (isInitialized) return true;
    targetHwnd = hWnd;
    GetMpris().Initialize();
    isInitialized = true;
    isEnabled = false;
    callbacksDisabled.store(false, std::memory_order_release);
    return true;
}

bool MediaTransport::EnsureInitialized(HWND hWnd) {
    if (isInitialized) return true;
    return Initialize(hWnd);
}

void MediaTransport::Shutdown() {
    SetCallbacksDisabled(true);
    ClearPendingActions();
    GetMpris().Shutdown();
    isInitialized = false;
    isEnabled = false;
    targetHwnd = NULL;
    lastSnapshot = MediaTransportSnapshot{};
    lastTimelineUpdateTimestampMs = 0;
    lastPublishedPositionSec = -1.0;
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
    isEnabled = false;
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
            if (!PostMessageW(targetHwnd, kMediaTransportEventMessage, 0, 0)) {
                actionPosted.store(false, std::memory_order_release);
            }
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

size_t MediaTransport::GetPendingActionCount() const {
    std::lock_guard<std::mutex> lock(actionMutex);
    return pendingActions.size();
}

void MediaTransport::ClearPendingActions() {
    std::lock_guard<std::mutex> lock(actionMutex);
    pendingActions.clear();
    actionPosted.store(false, std::memory_order_release);
}

void MediaTransport::SetCallbacksDisabled(bool disabled) {
    callbacksDisabled.store(disabled, std::memory_order_release);
    if (disabled) ClearPendingActions();
}

void MediaTransport::RegisterEventHandlers() {}
void MediaTransport::UnregisterEventHandlers() {}
void MediaTransport::ApplySnapshotToSmtc(const MediaTransportSnapshot& /*snapshot*/) {}
void MediaTransport::ApplyTimelineToSmtc(const MediaTransportTimeline& /*timeline*/) {}

} // namespace Aperture

#endif // !defined(_WIN32)
