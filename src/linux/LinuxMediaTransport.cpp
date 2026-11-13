#if !defined(_WIN32)

#include "../MediaTransport.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Aperture {

namespace {

constexpr int kDbusTypeBoolean = 'b';
constexpr int kDbusTypeInt64 = 'x';
constexpr int kDbusTypeDouble = 'd';
constexpr int kDbusTypeString = 's';
constexpr int kDbusTypeObjectPath = 'o';
constexpr int kDbusTypeArray = 'a';
constexpr int kDbusTypeVariant = 'v';
constexpr int kDbusTypeDictEntry = 'e';

constexpr unsigned int kDbusWatchReadable = 1;
constexpr unsigned int kDbusWatchWritable = 2;
constexpr unsigned int kDbusWatchError = 4;
constexpr unsigned int kDbusWatchHangup = 8;

constexpr int kDbusDispatchDataRemains = 0;

constexpr int kDbusHandlerResultHandled = 0;
constexpr int kDbusHandlerResultNotYetHandled = 1;

constexpr const char* kMprisBusName = "org.mpris.MediaPlayer2.aperture";
constexpr const char* kMprisObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kIfaceIntrospectable = "org.freedesktop.DBus.Introspectable";
constexpr const char* kIfaceProperties = "org.freedesktop.DBus.Properties";
constexpr const char* kIfaceMprisRoot = "org.mpris.MediaPlayer2";
constexpr const char* kIfaceMprisPlayer = "org.mpris.MediaPlayer2.Player";

constexpr const char* kIntrospectXml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n"
    "      <arg name=\"interface_name\" type=\"s\"/>\n"
    "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2\">\n"
    "    <method name=\"Raise\"/>\n"
    "    <method name=\"Quit\"/>\n"
    "    <property name=\"CanQuit\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanRaise\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"HasTrackList\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"Identity\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"DesktopEntry\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.mpris.MediaPlayer2.Player\">\n"
    "    <method name=\"Next\"/>\n"
    "    <method name=\"Previous\"/>\n"
    "    <method name=\"Pause\"/>\n"
    "    <method name=\"PlayPause\"/>\n"
    "    <method name=\"Stop\"/>\n"
    "    <method name=\"Play\"/>\n"
    "    <method name=\"Seek\">\n"
    "      <arg name=\"Offset\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SetPosition\">\n"
    "      <arg name=\"TrackId\" type=\"o\" direction=\"in\"/>\n"
    "      <arg name=\"Position\" type=\"x\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"OpenUri\">\n"
    "      <arg name=\"Uri\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <signal name=\"Seeked\">\n"
    "      <arg name=\"Position\" type=\"x\"/>\n"
    "    </signal>\n"
    "    <property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"Rate\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>\n"
    "    <property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>\n"
    "    <property name=\"Volume\" type=\"d\" access=\"readwrite\"/>\n"
    "    <property name=\"Position\" type=\"x\" access=\"read\"/>\n"
    "    <property name=\"MinimumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"MaximumRate\" type=\"d\" access=\"read\"/>\n"
    "    <property name=\"CanGoNext\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPlay\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanPause\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanSeek\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"CanControl\" type=\"b\" access=\"read\"/>\n"
    "  </interface>\n"
    "</node>\n";

bool CreateWakePipe(int fds[2]) {
#if defined(__linux__)
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0) return true;
#endif
    if (::pipe(fds) != 0) return false;
    for (int i = 0; i < 2; ++i) {
        int fl = ::fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) ::fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
        int fdFl = ::fcntl(fds[i], F_GETFD, 0);
        if (fdFl >= 0) ::fcntl(fds[i], F_SETFD, fdFl | FD_CLOEXEC);
    }
    return true;
}

const char* PlaybackStateToMprisStatus(PlaybackState st) {
    switch (st) {
        case PlaybackState::Playing: return "Playing";
        case PlaybackState::Paused: return "Paused";
        default: return "Stopped";
    }
}

struct DbusMprisRuntime {
    using dbus_bool_t = uint32_t;
    struct DBusError {
        const char* name;
        const char* message;
        unsigned int flags;
        void* pad1;
        uint64_t extra[4];
    };
    struct DBusMessageIter {
        uint64_t opaque[16];
    };
    struct DBusObjectPathVTable {
        void (*unregister_function)(void* connection, void* user_data);
        int (*message_function)(void* connection, void* message, void* user_data);
        void (*pad1)(void*);
        void (*pad2)(void*);
        void (*pad3)(void*);
        void (*pad4)(void*);
    };

    using DBusAddWatchFn = dbus_bool_t (*)(void* watch, void* data);
    using DBusRemoveWatchFn = void (*)(void* watch, void* data);
    using DBusToggleWatchFn = void (*)(void* watch, void* data);
    using DBusWakeupMainFn = void (*)(void* data);
    using DBusFreeDataFn = void (*)(void* memory);

    void* lib = nullptr;
    void* conn = nullptr;
    std::atomic<bool> active{false};
    std::atomic<bool> stopping{false};
    std::atomic<int> wakeReadFd{-1};
    std::atomic<int> wakeWriteFd{-1};
    std::thread workerThread;

    std::mutex watchMutex;
    std::vector<void*> watches;

    std::mutex stateMutex;
    MediaTransportSnapshot snapshot;
    uint64_t trackCounter = 1;
    std::string trackObjectPath = "/org/mpris/MediaPlayer2/Track/1";
    int64_t basePositionUs = 0;
    int64_t durationUs = 0;
    std::chrono::steady_clock::time_point positionTimestamp = std::chrono::steady_clock::now();
    bool propsDirty = false;
    bool seekedPending = false;
    int64_t seekedPositionUs = 0;
    std::function<void(AppCommand, const CommandArgs&)> dispatchAction;

    // libdbus symbols
    dbus_bool_t (*dbus_threads_init_default)() = nullptr;
    void (*dbus_error_init)(DBusError* error) = nullptr;
    void (*dbus_error_free)(DBusError* error) = nullptr;
    dbus_bool_t (*dbus_error_is_set)(const DBusError* error) = nullptr;
    void* (*dbus_bus_get_private)(int type, DBusError* error) = nullptr;
    int (*dbus_bus_request_name)(void* connection, const char* name, unsigned int flags, DBusError* error) = nullptr;
    int (*dbus_bus_release_name)(void* connection, const char* name, DBusError* error) = nullptr;
    void (*dbus_connection_set_exit_on_disconnect)(void* connection, dbus_bool_t exitOnDisconnect) = nullptr;
    void (*dbus_connection_close)(void* connection) = nullptr;
    void (*dbus_connection_unref)(void* connection) = nullptr;
    void (*dbus_connection_flush)(void* connection) = nullptr;
    dbus_bool_t (*dbus_connection_send)(void* connection, void* message, uint32_t* serial) = nullptr;
    dbus_bool_t (*dbus_connection_has_messages_to_send)(void* connection) = nullptr;
    dbus_bool_t (*dbus_connection_register_object_path)(void* connection, const char* path,
                                                        const DBusObjectPathVTable* vtable, void* userData) = nullptr;
    dbus_bool_t (*dbus_connection_unregister_object_path)(void* connection, const char* path) = nullptr;
    dbus_bool_t (*dbus_connection_set_watch_functions)(void* connection, DBusAddWatchFn addFn,
                                                       DBusRemoveWatchFn removeFn, DBusToggleWatchFn toggleFn,
                                                       void* data, DBusFreeDataFn freeFn) = nullptr;
    void (*dbus_connection_set_wakeup_main_function)(void* connection, DBusWakeupMainFn wakeupFn,
                                                     void* data, DBusFreeDataFn freeFn) = nullptr;
    int (*dbus_connection_get_dispatch_status)(void* connection) = nullptr;
    int (*dbus_connection_dispatch)(void* connection) = nullptr;
    int (*dbus_watch_get_unix_fd)(void* watch) = nullptr;
    unsigned int (*dbus_watch_get_flags)(void* watch) = nullptr;
    dbus_bool_t (*dbus_watch_get_enabled)(void* watch) = nullptr;
    dbus_bool_t (*dbus_watch_handle)(void* watch, unsigned int flags) = nullptr;
    dbus_bool_t (*dbus_message_is_method_call)(void* message, const char* iface, const char* method) = nullptr;
    void* (*dbus_message_new_method_return)(void* methodCall) = nullptr;
    void* (*dbus_message_new_error)(void* replyTo, const char* errorName, const char* errorMessage) = nullptr;
    void* (*dbus_message_new_signal)(const char* path, const char* iface, const char* name) = nullptr;
    void (*dbus_message_unref)(void* message) = nullptr;
    dbus_bool_t (*dbus_message_iter_init)(void* message, DBusMessageIter* iter) = nullptr;
    dbus_bool_t (*dbus_message_iter_next)(DBusMessageIter* iter) = nullptr;
    int (*dbus_message_iter_get_arg_type)(DBusMessageIter* iter) = nullptr;
    void (*dbus_message_iter_get_basic)(DBusMessageIter* iter, void* value) = nullptr;
    void (*dbus_message_iter_recurse)(DBusMessageIter* iter, DBusMessageIter* sub) = nullptr;
    void (*dbus_message_iter_init_append)(void* message, DBusMessageIter* iter) = nullptr;
    dbus_bool_t (*dbus_message_iter_append_basic)(DBusMessageIter* iter, int type, const void* value) = nullptr;
    dbus_bool_t (*dbus_message_iter_open_container)(DBusMessageIter* iter, int type,
                                                      const char* containedSig, DBusMessageIter* sub) = nullptr;
    dbus_bool_t (*dbus_message_iter_close_container)(DBusMessageIter* iter, DBusMessageIter* sub) = nullptr;

    template <typename Fn>
    bool LoadSym(Fn& out, const char* name) {
        out = reinterpret_cast<Fn>(::dlsym(lib, name));
        return out != nullptr;
    }

    void WakeWorker() {
        int wfd = wakeWriteFd.load(std::memory_order_acquire);
        if (wfd >= 0) {
            uint8_t b = 1;
            ssize_t unused = ::write(wfd, &b, sizeof(b));
            (void)unused;
        }
    }

    static dbus_bool_t OnAddWatch(void* watch, void* data) {
        auto* self = static_cast<DbusMprisRuntime*>(data);
        if (!self || !watch) return 0;
        {
            std::lock_guard<std::mutex> lk(self->watchMutex);
            if (std::find(self->watches.begin(), self->watches.end(), watch) == self->watches.end()) {
                self->watches.push_back(watch);
            }
        }
        self->WakeWorker();
        return 1;
    }

    static void OnRemoveWatch(void* watch, void* data) {
        auto* self = static_cast<DbusMprisRuntime*>(data);
        if (!self || !watch) return;
        std::lock_guard<std::mutex> lk(self->watchMutex);
        self->watches.erase(std::remove(self->watches.begin(), self->watches.end(), watch), self->watches.end());
    }

    static void OnToggleWatch(void* /*watch*/, void* data) {
        auto* self = static_cast<DbusMprisRuntime*>(data);
        if (self) self->WakeWorker();
    }

    static void OnWakeupMain(void* data) {
        auto* self = static_cast<DbusMprisRuntime*>(data);
        if (self) self->WakeWorker();
    }

    static int OnObjectMessage(void* connection, void* message, void* userData) {
        auto* self = static_cast<DbusMprisRuntime*>(userData);
        if (!self || !connection || !message) return kDbusHandlerResultNotYetHandled;
        return self->HandleMessage(connection, message);
    }

    int64_t ComputeLivePositionUsLocked(std::chrono::steady_clock::time_point now) const {
        if (snapshot.playbackState == PlaybackState::Closed) return 0;
        int64_t pos = basePositionUs;
        if (snapshot.playbackState == PlaybackState::Playing) {
            auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - positionTimestamp).count();
            if (elapsedUs > 0) pos += elapsedUs;
        }
        if (pos < 0) pos = 0;
        if (durationUs > 0 && pos > durationUs) pos = durationUs;
        return pos;
    }

    void AppendBasicVariant(DBusMessageIter* iter, int dbusType, const char* sig, const void* valPtr) {
        DBusMessageIter var{};
        dbus_message_iter_open_container(iter, kDbusTypeVariant, sig, &var);
        dbus_message_iter_append_basic(&var, dbusType, valPtr);
        dbus_message_iter_close_container(iter, &var);
    }

    void AppendBoolVariant(DBusMessageIter* iter, bool val) {
        dbus_bool_t b = val ? 1U : 0U;
        AppendBasicVariant(iter, kDbusTypeBoolean, "b", &b);
    }
    void AppendDoubleVariant(DBusMessageIter* iter, double val) {
        AppendBasicVariant(iter, kDbusTypeDouble, "d", &val);
    }
    void AppendInt64Variant(DBusMessageIter* iter, int64_t val) {
        AppendBasicVariant(iter, kDbusTypeInt64, "x", &val);
    }
    void AppendStringVariant(DBusMessageIter* iter, const char* val) {
        const char* safe = val ? val : "";
        AppendBasicVariant(iter, kDbusTypeString, "s", &safe);
    }
    void AppendStringArrayVariant(DBusMessageIter* iter, const std::vector<const char*>& items) {
        DBusMessageIter var{}, arr{};
        dbus_message_iter_open_container(iter, kDbusTypeVariant, "as", &var);
        dbus_message_iter_open_container(&var, kDbusTypeArray, "s", &arr);
        for (const char* s : items) {
            const char* safe = s ? s : "";
            dbus_message_iter_append_basic(&arr, kDbusTypeString, &safe);
        }
        dbus_message_iter_close_container(&var, &arr);
        dbus_message_iter_close_container(iter, &var);
    }

    template <typename ValueWriter>
    void AppendDictEntry(DBusMessageIter* dict, const char* key, ValueWriter&& writeValue) {
        DBusMessageIter entry{};
        dbus_message_iter_open_container(dict, kDbusTypeDictEntry, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, kDbusTypeString, &key);
        writeValue(&entry);
        dbus_message_iter_close_container(dict, &entry);
    }

    void AppendMetadataVariant(DBusMessageIter* iter, const MediaTransportSnapshot& snap,
                               const std::string& trackPath, int64_t durUs) {
        DBusMessageIter var{}, metaArr{};
        dbus_message_iter_open_container(iter, kDbusTypeVariant, "a{sv}", &var);
        dbus_message_iter_open_container(&var, kDbusTypeArray, "{sv}", &metaArr);
        AppendDictEntry(&metaArr, "mpris:trackid", [&](DBusMessageIter* e) {
            const char* p = (snap.playbackState == PlaybackState::Closed)
                                ? "/org/mpris/MediaPlayer2/TrackList/NoTrack"
                                : trackPath.c_str();
            AppendBasicVariant(e, kDbusTypeObjectPath, "o", &p);
        });
        if (snap.playbackState != PlaybackState::Closed) {
            if (durUs > 0) {
                AppendDictEntry(&metaArr, "mpris:length", [&](DBusMessageIter* e) { AppendInt64Variant(e, durUs); });
            }
            if (!snap.title.empty()) {
                std::string u8Title = LinuxWideToUtf8(snap.title);
                AppendDictEntry(&metaArr, "xesam:title", [&](DBusMessageIter* e) { AppendStringVariant(e, u8Title.c_str()); });
            }
            if (!snap.artist.empty()) {
                std::string u8Artist = LinuxWideToUtf8(snap.artist);
                AppendDictEntry(&metaArr, "xesam:artist", [&](DBusMessageIter* e) { AppendStringArrayVariant(e, {u8Artist.c_str()}); });
            }
        }
        dbus_message_iter_close_container(&var, &metaArr);
        dbus_message_iter_close_container(iter, &var);
    }

    bool WriteRootProperty(DBusMessageIter* out, const std::string& prop) {
        static const std::vector<const char*> kMimes = {
            "video/mp4", "video/x-matroska", "video/webm", "video/quicktime",
            "video/x-msvideo", "audio/mpeg", "audio/flac", "audio/ogg"
        };
        if (prop == "CanQuit") AppendBoolVariant(out, true);
        else if (prop == "CanRaise" || prop == "HasTrackList") AppendBoolVariant(out, false);
        else if (prop == "Identity") AppendStringVariant(out, "Aperture");
        else if (prop == "DesktopEntry") AppendStringVariant(out, "aperture");
        else if (prop == "SupportedUriSchemes") AppendStringArrayVariant(out, {"file"});
        else if (prop == "SupportedMimeTypes") AppendStringArrayVariant(out, kMimes);
        else return false;
        return true;
    }

    bool WritePlayerProperty(DBusMessageIter* out, const std::string& prop,
                             const MediaTransportSnapshot& snap, const std::string& trackPath,
                             int64_t durUs, int64_t livePosUs) {
        if (prop == "PlaybackStatus") AppendStringVariant(out, PlaybackStateToMprisStatus(snap.playbackState));
        else if (prop == "LoopStatus") AppendStringVariant(out, "None");
        else if (prop == "Rate" || prop == "Volume" || prop == "MinimumRate" || prop == "MaximumRate") AppendDoubleVariant(out, 1.0);
        else if (prop == "Shuffle") AppendBoolVariant(out, false);
        else if (prop == "Metadata") AppendMetadataVariant(out, snap, trackPath, durUs);
        else if (prop == "Position") AppendInt64Variant(out, livePosUs);
        else if (prop == "CanGoNext") AppendBoolVariant(out, snap.isNextEnabled);
        else if (prop == "CanGoPrevious") AppendBoolVariant(out, snap.isPreviousEnabled);
        else if (prop == "CanPlay" || prop == "CanPause") AppendBoolVariant(out, snap.playbackState != PlaybackState::Closed);
        else if (prop == "CanSeek") AppendBoolVariant(out, snap.isSeekEnabled);
        else if (prop == "CanControl") AppendBoolVariant(out, true);
        else return false;
        return true;
    }

    void AppendPlayerPropertiesDict(DBusMessageIter* dict, const MediaTransportSnapshot& snap,
                                    const std::string& trackPath, int64_t durUs, int64_t livePosUs, bool includePosition) {
        static constexpr const char* kKeys[] = {
            "PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position",
            "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl"
        };
        for (const char* key : kKeys) {
            if (!includePosition && std::strcmp(key, "Position") == 0) continue;
            AppendDictEntry(dict, key, [&](DBusMessageIter* e) {
                WritePlayerProperty(e, key, snap, trackPath, durUs, livePosUs);
            });
        }
    }

    void AppendRootPropertiesDict(DBusMessageIter* dict) {
        static constexpr const char* kKeys[] = {
            "CanQuit", "CanRaise", "HasTrackList", "Identity", "DesktopEntry", "SupportedUriSchemes", "SupportedMimeTypes"
        };
        for (const char* key : kKeys) {
            AppendDictEntry(dict, key, [&](DBusMessageIter* e) { WriteRootProperty(e, key); });
        }
    }

    void SendSimpleReply(void* connection, void* msg) {
        void* reply = dbus_message_new_method_return(msg);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
    }

    void SendErrorReply(void* connection, void* msg, const char* errName, const char* errMsg) {
        void* reply = dbus_message_new_error(msg, errName, errMsg);
        if (reply) {
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
        }
    }

    void QueuePlayerCommand(AppCommand cmd, const CommandArgs& args = {}) {
        std::function<void(AppCommand, const CommandArgs&)> fn;
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            fn = dispatchAction;
        }
        if (fn) fn(cmd, args);
    }

    int HandlePropertiesCall(void* connection, void* msg) {
        if (dbus_message_is_method_call(msg, kIfaceProperties, "Get")) {
            DBusMessageIter it{};
            if (!dbus_message_iter_init(msg, &it) || dbus_message_iter_get_arg_type(&it) != kDbusTypeString) {
                SendErrorReply(connection, msg, "org.freedesktop.DBus.Error.InvalidArgs", "Missing interface name");
                return kDbusHandlerResultHandled;
            }
            const char* iface = nullptr;
            dbus_message_iter_get_basic(&it, &iface);
            if (!dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != kDbusTypeString) {
                SendErrorReply(connection, msg, "org.freedesktop.DBus.Error.InvalidArgs", "Missing property name");
                return kDbusHandlerResultHandled;
            }
            const char* prop = nullptr;
            dbus_message_iter_get_basic(&it, &prop);
            std::string ifaceStr = iface ? iface : "";
            std::string propStr = prop ? prop : "";

            void* reply = dbus_message_new_method_return(msg);
            if (!reply) return kDbusHandlerResultHandled;
            DBusMessageIter out{};
            dbus_message_iter_init_append(reply, &out);

            bool ok = false;
            if (ifaceStr == kIfaceMprisRoot) {
                ok = WriteRootProperty(&out, propStr);
            } else if (ifaceStr == kIfaceMprisPlayer) {
                MediaTransportSnapshot snapCopy;
                std::string trackCopy;
                int64_t durCopy = 0;
                int64_t posCopy = 0;
                {
                    std::lock_guard<std::mutex> lk(stateMutex);
                    snapCopy = snapshot;
                    trackCopy = trackObjectPath;
                    durCopy = durationUs;
                    posCopy = ComputeLivePositionUsLocked(std::chrono::steady_clock::now());
                }
                ok = WritePlayerProperty(&out, propStr, snapCopy, trackCopy, durCopy, posCopy);
            }
            if (ok) {
                dbus_connection_send(connection, reply, nullptr);
                dbus_message_unref(reply);
                return kDbusHandlerResultHandled;
            }
            dbus_message_unref(reply);
            SendErrorReply(connection, msg, "org.freedesktop.DBus.Error.UnknownProperty", "Unknown property");
            return kDbusHandlerResultHandled;
        }

        if (dbus_message_is_method_call(msg, kIfaceProperties, "GetAll")) {
            DBusMessageIter it{};
            const char* iface = nullptr;
            if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == kDbusTypeString) {
                dbus_message_iter_get_basic(&it, &iface);
            }
            std::string ifaceStr = iface ? iface : "";
            void* reply = dbus_message_new_method_return(msg);
            if (!reply) return kDbusHandlerResultHandled;
            DBusMessageIter out{}, dict{};
            dbus_message_iter_init_append(reply, &out);
            dbus_message_iter_open_container(&out, kDbusTypeArray, "{sv}", &dict);
            if (ifaceStr == kIfaceMprisRoot) {
                AppendRootPropertiesDict(&dict);
            } else if (ifaceStr == kIfaceMprisPlayer) {
                MediaTransportSnapshot snapCopy;
                std::string trackCopy;
                int64_t durCopy = 0;
                int64_t posCopy = 0;
                {
                    std::lock_guard<std::mutex> lk(stateMutex);
                    snapCopy = snapshot;
                    trackCopy = trackObjectPath;
                    durCopy = durationUs;
                    posCopy = ComputeLivePositionUsLocked(std::chrono::steady_clock::now());
                }
                AppendPlayerPropertiesDict(&dict, snapCopy, trackCopy, durCopy, posCopy, true);
            }
            dbus_message_iter_close_container(&out, &dict);
            dbus_connection_send(connection, reply, nullptr);
            dbus_message_unref(reply);
            return kDbusHandlerResultHandled;
        }

        if (dbus_message_is_method_call(msg, kIfaceProperties, "Set")) {
            SendSimpleReply(connection, msg);
            return kDbusHandlerResultHandled;
        }

        return kDbusHandlerResultNotYetHandled;
    }

    int HandleMessage(void* connection, void* msg) {
        if (dbus_message_is_method_call(msg, kIfaceIntrospectable, "Introspect")) {
            void* reply = dbus_message_new_method_return(msg);
            if (reply) {
                DBusMessageIter out{};
                dbus_message_iter_init_append(reply, &out);
                const char* xml = kIntrospectXml;
                dbus_message_iter_append_basic(&out, kDbusTypeString, &xml);
                dbus_connection_send(connection, reply, nullptr);
                dbus_message_unref(reply);
            }
            return kDbusHandlerResultHandled;
        }

        if (dbus_message_is_method_call(msg, kIfaceProperties, "Get") ||
            dbus_message_is_method_call(msg, kIfaceProperties, "GetAll") ||
            dbus_message_is_method_call(msg, kIfaceProperties, "Set")) {
            return HandlePropertiesCall(connection, msg);
        }

        static constexpr struct {
            const char* iface;
            const char* method;
            AppCommand cmd;
        } kSimpleMethods[] = {
            {kIfaceMprisRoot, "Raise", AppCommand::None},
            {kIfaceMprisRoot, "Quit", AppCommand::ClosePlayer},
            {kIfaceMprisPlayer, "Play", AppCommand::Play},
            {kIfaceMprisPlayer, "Pause", AppCommand::Pause},
            {kIfaceMprisPlayer, "PlayPause", AppCommand::TogglePlay},
            {kIfaceMprisPlayer, "Stop", AppCommand::ClosePlayer},
            {kIfaceMprisPlayer, "Next", AppCommand::PlayNext},
            {kIfaceMprisPlayer, "Previous", AppCommand::PlayPrevious},
            {kIfaceMprisPlayer, "OpenUri", AppCommand::None},
        };
        for (const auto& m : kSimpleMethods) {
            if (dbus_message_is_method_call(msg, m.iface, m.method)) {
                if (m.cmd != AppCommand::None) QueuePlayerCommand(m.cmd);
                SendSimpleReply(connection, msg);
                return kDbusHandlerResultHandled;
            }
        }
        if (dbus_message_is_method_call(msg, kIfaceMprisPlayer, "Seek")) {
            DBusMessageIter it{};
            int64_t offsetUs = 0;
            if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == kDbusTypeInt64) {
                dbus_message_iter_get_basic(&it, &offsetUs);
                int64_t targetUs = 0;
                bool canSeek = false;
                {
                    std::lock_guard<std::mutex> lk(stateMutex);
                    canSeek = snapshot.isSeekEnabled;
                    int64_t curUs = ComputeLivePositionUsLocked(std::chrono::steady_clock::now());
                    targetUs = std::max<int64_t>(0, curUs + offsetUs);
                    if (durationUs > 0 && targetUs > durationUs) targetUs = durationUs;
                }
                if (canSeek) {
                    CommandArgs args{};
                    args.doubleVal = static_cast<double>(targetUs) / 1000000.0;
                    QueuePlayerCommand(AppCommand::SeekTo, args);
                }
            }
            SendSimpleReply(connection, msg);
            return kDbusHandlerResultHandled;
        }
        if (dbus_message_is_method_call(msg, kIfaceMprisPlayer, "SetPosition")) {
            DBusMessageIter it{};
            const char* trackId = nullptr;
            int64_t posUs = 0;
            if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == kDbusTypeObjectPath) {
                dbus_message_iter_get_basic(&it, &trackId);
                if (dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == kDbusTypeInt64) {
                    dbus_message_iter_get_basic(&it, &posUs);
                    bool accept = false;
                    int64_t targetUs = std::max<int64_t>(0, posUs);
                    {
                        std::lock_guard<std::mutex> lk(stateMutex);
                        if (snapshot.isSeekEnabled && trackId &&
                            (trackObjectPath == trackId ||
                             std::strncmp(trackId, "/org/mpris/MediaPlayer2/Track/", 30) == 0)) {
                            accept = true;
                            if (durationUs > 0 && targetUs > durationUs) targetUs = durationUs;
                        }
                    }
                    if (accept) {
                        CommandArgs args{};
                        args.doubleVal = static_cast<double>(targetUs) / 1000000.0;
                        QueuePlayerCommand(AppCommand::SeekTo, args);
                    }
                }
            }
            SendSimpleReply(connection, msg);
            return kDbusHandlerResultHandled;
        }

        return kDbusHandlerResultNotYetHandled;
    }

    void EmitPendingSignals() {
        MediaTransportSnapshot snapCopy;
        std::string trackCopy;
        int64_t durCopy = 0;
        bool sendProps = false;
        bool sendSeeked = false;
        int64_t seekPosUs = 0;
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            if (propsDirty) {
                snapCopy = snapshot;
                trackCopy = trackObjectPath;
                durCopy = durationUs;
                sendProps = true;
                propsDirty = false;
            }
            if (seekedPending) {
                seekPosUs = seekedPositionUs;
                sendSeeked = true;
                seekedPending = false;
            }
        }
        if (sendProps && conn) {
            void* sig = dbus_message_new_signal(kMprisObjectPath, kIfaceProperties, "PropertiesChanged");
            if (sig) {
                DBusMessageIter out{}, changedDict{}, invalidArr{};
                const char* ifaceName = kIfaceMprisPlayer;
                dbus_message_iter_init_append(sig, &out);
                dbus_message_iter_append_basic(&out, kDbusTypeString, &ifaceName);
                dbus_message_iter_open_container(&out, kDbusTypeArray, "{sv}", &changedDict);
                AppendPlayerPropertiesDict(&changedDict, snapCopy, trackCopy, durCopy, 0, false);
                dbus_message_iter_close_container(&out, &changedDict);
                dbus_message_iter_open_container(&out, kDbusTypeArray, "s", &invalidArr);
                dbus_message_iter_close_container(&out, &invalidArr);
                dbus_connection_send(conn, sig, nullptr);
                dbus_message_unref(sig);
            }
        }
        if (sendSeeked && conn) {
            void* sig = dbus_message_new_signal(kMprisObjectPath, kIfaceMprisPlayer, "Seeked");
            if (sig) {
                DBusMessageIter out{};
                dbus_message_iter_init_append(sig, &out);
                dbus_message_iter_append_basic(&out, kDbusTypeInt64, &seekPosUs);
                dbus_connection_send(conn, sig, nullptr);
                dbus_message_unref(sig);
            }
        }
        if ((sendProps || sendSeeked) && conn && dbus_connection_flush) {
            dbus_connection_flush(conn);
        }
    }

    void WorkerLoop() {
        int rfd = wakeReadFd.load(std::memory_order_acquire);
        while (!stopping.load(std::memory_order_acquire)) {
            EmitPendingSignals();
            if (conn) {
                while (!stopping.load(std::memory_order_acquire) &&
                       dbus_connection_get_dispatch_status(conn) == kDbusDispatchDataRemains) {
                    dbus_connection_dispatch(conn);
                }
                if (dbus_connection_has_messages_to_send && dbus_connection_has_messages_to_send(conn)) {
                    dbus_connection_flush(conn);
                }
            }
            if (stopping.load(std::memory_order_acquire)) break;

            struct WatchSlot {
                void* watch;
                int fd;
            };
            std::vector<struct pollfd> pfds;
            std::vector<WatchSlot> slots;
            if (rfd >= 0) {
                pfds.push_back({rfd, POLLIN, 0});
            }
            {
                std::lock_guard<std::mutex> lk(watchMutex);
                for (void* w : watches) {
                    if (!dbus_watch_get_enabled(w)) continue;
                    int wfd = dbus_watch_get_unix_fd(w);
                    if (wfd < 0) continue;
                    unsigned int flags = dbus_watch_get_flags(w);
                    short ev = 0;
                    if (flags & kDbusWatchReadable) ev |= POLLIN;
                    if (flags & kDbusWatchWritable) ev |= POLLOUT;
                    if (ev != 0) {
                        pfds.push_back({wfd, ev, 0});
                        slots.push_back({w, wfd});
                    }
                }
            }
            if (pfds.empty()) break;

            int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), -1);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            size_t baseIdx = (rfd >= 0) ? 1 : 0;
            if (rfd >= 0 && (pfds[0].revents & POLLIN)) {
                uint8_t buf[64];
                while (::read(rfd, buf, sizeof(buf)) > 0) {}
            }
            for (size_t i = 0; i < slots.size(); ++i) {
                short rev = pfds[baseIdx + i].revents;
                if (rev == 0) continue;
                unsigned int wflags = 0;
                if (rev & POLLIN) wflags |= kDbusWatchReadable;
                if (rev & POLLOUT) wflags |= kDbusWatchWritable;
                if (rev & POLLERR) wflags |= kDbusWatchError;
                if (rev & POLLHUP) wflags |= kDbusWatchHangup;
                if (wflags != 0) {
                    dbus_watch_handle(slots[i].watch, wflags);
                }
            }
        }
    }

    void Initialize() {
        if (active.load(std::memory_order_acquire)) return;
        if (!lib) {
            for (const char* name : {"libdbus-1.so.3", "libdbus-1.so"}) {
                lib = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
                if (lib) break;
            }
            if (!lib) return;
        }
        bool ok =
            LoadSym(dbus_threads_init_default, "dbus_threads_init_default") &&
            LoadSym(dbus_error_init, "dbus_error_init") &&
            LoadSym(dbus_error_free, "dbus_error_free") &&
            LoadSym(dbus_error_is_set, "dbus_error_is_set") &&
            LoadSym(dbus_bus_get_private, "dbus_bus_get_private") &&
            LoadSym(dbus_bus_request_name, "dbus_bus_request_name") &&
            LoadSym(dbus_bus_release_name, "dbus_bus_release_name") &&
            LoadSym(dbus_connection_set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect") &&
            LoadSym(dbus_connection_close, "dbus_connection_close") &&
            LoadSym(dbus_connection_unref, "dbus_connection_unref") &&
            LoadSym(dbus_connection_flush, "dbus_connection_flush") &&
            LoadSym(dbus_connection_send, "dbus_connection_send") &&
            LoadSym(dbus_connection_has_messages_to_send, "dbus_connection_has_messages_to_send") &&
            LoadSym(dbus_connection_register_object_path, "dbus_connection_register_object_path") &&
            LoadSym(dbus_connection_unregister_object_path, "dbus_connection_unregister_object_path") &&
            LoadSym(dbus_connection_set_watch_functions, "dbus_connection_set_watch_functions") &&
            LoadSym(dbus_connection_set_wakeup_main_function, "dbus_connection_set_wakeup_main_function") &&
            LoadSym(dbus_connection_get_dispatch_status, "dbus_connection_get_dispatch_status") &&
            LoadSym(dbus_connection_dispatch, "dbus_connection_dispatch") &&
            LoadSym(dbus_watch_get_unix_fd, "dbus_watch_get_unix_fd") &&
            LoadSym(dbus_watch_get_flags, "dbus_watch_get_flags") &&
            LoadSym(dbus_watch_get_enabled, "dbus_watch_get_enabled") &&
            LoadSym(dbus_watch_handle, "dbus_watch_handle") &&
            LoadSym(dbus_message_is_method_call, "dbus_message_is_method_call") &&
            LoadSym(dbus_message_new_method_return, "dbus_message_new_method_return") &&
            LoadSym(dbus_message_new_error, "dbus_message_new_error") &&
            LoadSym(dbus_message_new_signal, "dbus_message_new_signal") &&
            LoadSym(dbus_message_unref, "dbus_message_unref") &&
            LoadSym(dbus_message_iter_init, "dbus_message_iter_init") &&
            LoadSym(dbus_message_iter_next, "dbus_message_iter_next") &&
            LoadSym(dbus_message_iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
            LoadSym(dbus_message_iter_get_basic, "dbus_message_iter_get_basic") &&
            LoadSym(dbus_message_iter_recurse, "dbus_message_iter_recurse") &&
            LoadSym(dbus_message_iter_init_append, "dbus_message_iter_init_append") &&
            LoadSym(dbus_message_iter_append_basic, "dbus_message_iter_append_basic") &&
            LoadSym(dbus_message_iter_open_container, "dbus_message_iter_open_container") &&
            LoadSym(dbus_message_iter_close_container, "dbus_message_iter_close_container");
        if (!ok) return;

        dbus_threads_init_default();

        int pfds[2] = {-1, -1};
        if (!CreateWakePipe(pfds)) return;

        DBusError err{};
        dbus_error_init(&err);
        conn = dbus_bus_get_private(0 /*DBUS_BUS_SESSION*/, &err);
        if (!conn || dbus_error_is_set(&err)) {
            if (conn) {
                dbus_connection_close(conn);
                dbus_connection_unref(conn);
                conn = nullptr;
            }
            dbus_error_free(&err);
            ::close(pfds[0]);
            ::close(pfds[1]);
            return;
        }
        dbus_connection_set_exit_on_disconnect(conn, 0);

        DBusObjectPathVTable vtable{};
        vtable.message_function = &OnObjectMessage;
        dbus_connection_register_object_path(conn, kMprisObjectPath, &vtable, this);
        dbus_bus_request_name(conn, kMprisBusName, 4 /*DBUS_NAME_FLAG_DO_NOT_QUEUE*/, &err);
        dbus_error_free(&err);

        wakeReadFd.store(pfds[0], std::memory_order_release);
        wakeWriteFd.store(pfds[1], std::memory_order_release);
        stopping.store(false, std::memory_order_release);

        dbus_connection_set_watch_functions(conn, &OnAddWatch, &OnRemoveWatch, &OnToggleWatch, this, nullptr);
        dbus_connection_set_wakeup_main_function(conn, &OnWakeupMain, this, nullptr);
        dbus_connection_flush(conn);

        active.store(true, std::memory_order_release);
        workerThread = std::thread(&DbusMprisRuntime::WorkerLoop, this);
    }

    void Shutdown() {
        stopping.store(true, std::memory_order_release);
        active.store(false, std::memory_order_release);
        WakeWorker();
        if (workerThread.joinable()) {
            workerThread.join();
        }
        int rfd = wakeReadFd.exchange(-1, std::memory_order_acq_rel);
        int wfd = wakeWriteFd.exchange(-1, std::memory_order_acq_rel);
        if (rfd >= 0) ::close(rfd);
        if (wfd >= 0) ::close(wfd);
        {
            std::lock_guard<std::mutex> lk(watchMutex);
            watches.clear();
        }
        if (conn) {
            dbus_connection_set_watch_functions(conn, nullptr, nullptr, nullptr, nullptr, nullptr);
            dbus_connection_set_wakeup_main_function(conn, nullptr, nullptr, nullptr);
            dbus_connection_unregister_object_path(conn, kMprisObjectPath);
            DBusError err{};
            dbus_error_init(&err);
            dbus_bus_release_name(conn, kMprisBusName, &err);
            dbus_error_free(&err);
            dbus_connection_close(conn);
            dbus_connection_unref(conn);
            conn = nullptr;
        }
    }

    void SetDispatcher(std::function<void(AppCommand, const CommandArgs&)> fn) {
        std::lock_guard<std::mutex> lk(stateMutex);
        dispatchAction = std::move(fn);
    }

    void UpdateSnapshot(const MediaTransportSnapshot& snap) {
        if (!active.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            if (snap.title != snapshot.title || snap.artist != snapshot.artist ||
                std::abs(snap.durationSec - snapshot.durationSec) > 0.5) {
                ++trackCounter;
                trackObjectPath = "/org/mpris/MediaPlayer2/Track/" + std::to_string(trackCounter);
            }
            snapshot = snap;
            durationUs = (snap.durationSec > 0.0 && std::isfinite(snap.durationSec))
                             ? static_cast<int64_t>(std::llround(snap.durationSec * 1000000.0))
                             : 0;
            basePositionUs = (snap.positionSec >= 0.0 && std::isfinite(snap.positionSec))
                                 ? static_cast<int64_t>(std::llround(snap.positionSec * 1000000.0))
                                 : 0;
            positionTimestamp = std::chrono::steady_clock::now();
            propsDirty = true;
        }
        WakeWorker();
    }

    void UpdateTimeline(const MediaTransportTimeline& timeline) {
        if (!active.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            auto now = std::chrono::steady_clock::now();
            int64_t predictedUs = ComputeLivePositionUsLocked(now);
            int64_t newPosUs = (timeline.positionSec >= 0.0 && std::isfinite(timeline.positionSec))
                                   ? static_cast<int64_t>(std::llround(timeline.positionSec * 1000000.0))
                                   : 0;
            int64_t newDurUs = (timeline.endTimeSec > 0.0 && std::isfinite(timeline.endTimeSec))
                                   ? static_cast<int64_t>(std::llround(timeline.endTimeSec * 1000000.0))
                                   : 0;
            basePositionUs = newPosUs;
            durationUs = newDurUs;
            positionTimestamp = now;
            if (std::abs(newPosUs - predictedUs) >= 1500000LL) {
                seekedPositionUs = newPosUs;
                seekedPending = true;
            }
        }
        WakeWorker();
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
    callbacksDisabled.store(false, std::memory_order_release);
    GetMpris().Initialize();
    RegisterEventHandlers();
    isInitialized = true;
    isEnabled = GetMpris().active.load(std::memory_order_acquire);
    return true;
}

bool MediaTransport::EnsureInitialized(HWND hWnd) {
    if (isInitialized) return true;
    return Initialize(hWnd);
}

void MediaTransport::Shutdown() {
    SetCallbacksDisabled(true);
    UnregisterEventHandlers();
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
    GetMpris().UpdateSnapshot(emptySnapshot);
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

void MediaTransport::RegisterEventHandlers() {
    GetMpris().SetDispatcher([this](AppCommand cmd, const CommandArgs& args) {
        this->QueueAction(cmd, args);
    });
}

void MediaTransport::UnregisterEventHandlers() {
    GetMpris().SetDispatcher({});
}

void MediaTransport::ApplySnapshotToSmtc(const MediaTransportSnapshot& snapshot) {
    GetMpris().UpdateSnapshot(snapshot);
}

void MediaTransport::ApplyTimelineToSmtc(const MediaTransportTimeline& timeline) {
    GetMpris().UpdateTimeline(timeline);
}

} // namespace Aperture

#endif // !defined(_WIN32)
