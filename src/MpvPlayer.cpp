#include "ThumbnailMigration.h"
#include "MpvPlayer.h"
#include "AudioConfig.h"
#include "Settings.h"
#include <shlwapi.h>
#include <sstream>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace Aperture {

namespace {

std::string TrimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string LowerAscii(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

// mpv accepts a comma-separated language preference list. Normalize the settings representation so values such as " ENG, jpn " are
// deterministic, while an empty/default value restores mpv's normal language selection.
std::string NormalizeLanguageList(const std::string& value) {
    std::string normalized;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        std::string token = LowerAscii(TrimAscii(value.substr(start, end - start)));
        if (!token.empty() && token != "default" && token != "auto" && token != "none") {
            if (!normalized.empty()) normalized += ',';
            normalized += token;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return normalized;
}

bool IsHexDigitString(const std::string& value) {
    for (char ch : value) { if (!std::isxdigit(static_cast<unsigned char>(ch))) return false; }
    return true;
}

std::string NormalizeSubtitleColor(const std::string& value) {
    std::string color = LowerAscii(TrimAscii(value));
    if (color.size() == 4 && color[0] == '#' && IsHexDigitString(color.substr(1)))
        return std::string("#") + color[1] + color[1] + color[2] + color[2] + color[3] + color[3];
    if ((color.size() == 7 || color.size() == 9) && color[0] == '#' && IsHexDigitString(color.substr(1))) { return color; }
    return "#ffffff";
}

std::string NormalizeSubtitleFontSize(const std::string& value) {
    std::string text = LowerAscii(TrimAscii(value));
    if (text.size() > 2 && text.substr(text.size() - 2) == "px") text.resize(text.size() - 2);
    else if (text.size() > 2 && text.substr(text.size() - 2) == "pt") text.resize(text.size() - 2);
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    const double size = end && end != text.c_str() && *end == '\0' && std::isfinite(parsed) ? std::clamp(parsed, 8.0, 96.0) : 20.0;
    std::ostringstream result;
    result << std::fixed << std::setprecision(1) << size;
    std::string normalized = result.str();
    while (!normalized.empty() && normalized.back() == '0') normalized.pop_back();
    if (!normalized.empty() && normalized.back() == '.') normalized.pop_back();
    return normalized;
}

const char* NormalizeSubtitlePosition(const std::string& value) {
    const std::string position = LowerAscii(TrimAscii(value));
    if (position == "top") return "8";
    if (position == "elevated") return "84";
    return "100";
}

} // namespace

MpvPlayer::MpvPlayer() {}

MpvPlayer::~MpvPlayer() { Shutdown(); }

void MpvPlayer::ReportError(const char* operation, int errorCode, ErrorSeverity severity) {
    std::string message = operation ? operation : "mpv operation";
    if (fn_mpv_error_string && errorCode < 0) {
        const char* detail = fn_mpv_error_string(errorCode);
        if (detail && *detail) { message += ": "; message += detail; }
    }
    std::wstring wideMessage = Utf8ToUtf16(message);
    ReportErrorMessage(wideMessage, severity);
}

void MpvPlayer::ReportErrorMessage(const std::wstring& message, ErrorSeverity severity) {
    { std::lock_guard<std::mutex> lock(stateMutex); lastError = message; }
    auto callback = CopyCallback(severity == ErrorSeverity::Fatal ? onError : onWarning);
    if (callback) callback(message);
}
void MpvPlayer::SetOnEnded(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onEnded = std::move(callback);
}

void MpvPlayer::SetOnTimeUpdated(std::function<void(double)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onTimeUpdated = std::move(callback);
}

void MpvPlayer::SetOnDurationChanged(std::function<void(double)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onDurationChanged = std::move(callback);
}

void MpvPlayer::SetOnPlayStateChanged(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onPlayStateChanged = std::move(callback);
}

void MpvPlayer::SetOnError(std::function<void(const std::wstring&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onError = std::move(callback);
}
void MpvPlayer::SetOnWarning(std::function<void(const std::wstring&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onWarning = std::move(callback);
}

void MpvPlayer::SetOnTracksChanged(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onTracksChanged = std::move(callback);
}

void MpvPlayer::SetOnChaptersChanged(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onChaptersChanged = std::move(callback);
}

void MpvPlayer::ClearCallbacks() {
    std::lock_guard<std::mutex> lock(callbackMutex);
    onEnded = nullptr;
    onTimeUpdated = nullptr;
    onDurationChanged = nullptr;
    onPlayStateChanged = nullptr;
    onError = nullptr;
    onWarning = nullptr;
    onTracksChanged = nullptr;
    onChaptersChanged = nullptr;
}
bool MpvPlayer::Initialize(HWND hParentWnd, HWND hChildVideoWnd) {
    if (isInitialized || mpv || eventThread.joinable()) Shutdown();
    hParent = hParentWnd;
    hVideoWnd = hChildVideoWnd;
    std::wstring exeDir = GetExeDir();
    if (!LoadMpvApi(MpvApiProfile::Playback, "Playback")) return false;
    mpv = fn_mpv_create();
    if (!mpv) { RecordInitializationFailure("mpv_create", MPV_ERROR_NOMEM); ResetMpvApi(); return false; }
    // Attach to child HWND for direct hardware-accelerated video rendering
    int64_t wid = (int64_t)hVideoWnd;
    SetMpvWindow(mpv, wid);
    // Disable mpv default keyboard bindings so Win32/Direct2D receives all key events
    SetMpvOption(mpv, "input-default-bindings", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-vo-keyboard", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-media-keys", "no", MpvOptionRequirement::Essential);
    // mpv's own OSD (track-change toasts, seek feedback) duplicates Aperture's OSD
    // badge; silence it so notifications come from exactly one surface.
    SetMpvOption(mpv, "osd-level", "0");
    // No yt-dlp hook: Aperture plays direct URLs only (network streams are validated http/https/rtsp/rtmp), so never invoke the bundled
    // Lua hook path for unhandled inputs.
    SetMpvOption(mpv, "load-scripts", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "scripts", "", MpvOptionRequirement::Essential);
    // Builds without Lua omit ytdl; retain the checked disable on builds that expose it.
    SetMpvOption(mpv, "ytdl", "no");
    // High performance playback settings: universal hardware decoding & EAC3/AC3 audio
    SetMpvOption(mpv, "gpu-context", "d3d11", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "gpu-api", "d3d11", MpvOptionRequirement::Essential);
    // Use D3D11VA direct surfaces. The safe/auto modes can select the copy path on Windows, retaining a second full-resolution system
    // memory frame queue alongside the GPU decoder surfaces.
    SetMpvOption(mpv, "hwdec", "d3d11va", MpvOptionRequirement::Optional, "no");
    SetMpvOption(mpv, "vo", "gpu", MpvOptionRequirement::Essential);
    // HEVC Main 10 frames are large (P010 is ~4 MiB at 1080p). mpv's default extra-surface cushion is unnecessary for this local player
    // and directly inflates both D3D11 video memory and mapped pages.
    SetMpvOption(mpv, "hwdec-extra-frames", "0");
    // Hardware submission needs no slice-worker pool on our direct D3D11 path.
    // Keep software-decoder threading independent. Extra hardware threads retain
    // additional full-resolution decoder surfaces, especially costly at 4K.
    SetMpvOption(mpv, "hwdec-threads", "1");
    // Avoid keeping an extra frame ahead of the display clock. This is a local-file player and the lower latency is preferable to
    // another full-resolution frame in the presentation pipeline.
    SetMpvOption(mpv, "video-latency-hacks", "yes");
    // Audio-clock pacing submits at the video's cadence instead of refreshing
    // the renderer at every display tick. Keep decoder/cache limits independent
    // of pacing; display-vdrop was substantially more expensive on high-Hz displays.
    SetMpvOption(mpv, "video-sync", "audio");
    SetMpvOption(mpv, "framedrop", "vo");
    SetMpvOption(mpv, "video-timing-offset", "0");
    SetMpvOption(mpv, "vd-lavc-dr", "yes");
    // Frame-accurate seeking for every seek type. mpv's default makes relative seeks keyframe-fast, which snaps quick skip steps onto
    // the same keyframe and can land past a subtitle cue that should be on screen, both reported as seek imprecision.
    SetMpvOption(mpv, "hr-seek", "always");
    SetMpvOption(mpv, "force-window", "yes", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "keep-open", "yes");
    SetMpvOption(mpv, "sub-auto", "fuzzy");
    // Bound libass' rendered subtitle bitmap cache. Entries are recreated on demand, so this retains subtitle correctness without
    // allowing a long ASS track to grow an unbounded glyph/bitmap working set.
    SetMpvOption(mpv, "sub-bitmap-max-size", "1");
    SetMpvOption(mpv, "audio-channels", "stereo");
    SetMpvOption(mpv, "audio-pitch-correction", "yes");
    SetMpvOption(mpv, "sub-font", "Inter");
    SetMpvOption(mpv, "sub-font-size", "46");
    SetMpvOption(mpv, "sub-border-size", "3.0");
    SetMpvOption(mpv, "sub-shadow-offset", "1.5");
    SetMpvOption(mpv, "sub-color", "#FFFFFF");
    SetMpvOption(mpv, "sub-border-color", "#000000");
    SetMpvOption(mpv, "secondary-sub-pos", "10");
    SetMpvOption(mpv, "secondary-sub-visibility", "yes");
    SetMpvOption(mpv, "secondary-sid", "no");
    // This is a local-file player, so keep readahead small.  A large
    // forward/backward demuxer cache is redundant with the OS file cache and becomes especially expensive when seeking through HEVC.
    SetMpvOption(mpv, "demuxer-max-bytes", "8MiB");
    SetMpvOption(mpv, "demuxer-max-back-bytes", "0");
    // Let libass resolve the shipped private fonts (Inter, Space Grotesk, JetBrains Mono) for subtitles without a system-wide install.
    std::string subFontsDir = Utf16ToUtf8(exeDir + L"\\fonts");
    SetMpvOption(mpv, "sub-fonts-dir", subFontsDir.c_str());
    // Persist compiled GPU shaders under the app's root-local cache directory
    // so later playback starts skip recompiling pipeline shaders.
    std::wstring shaderCacheDirW = GetCacheDir(CacheClass::Shaders);
    std::string shaderCacheDir = Utf16ToUtf8(shaderCacheDirW);
    SetMpvOption(mpv, "gpu-shader-cache-dir", shaderCacheDir.c_str());
    if (!EssentialOptionsAccepted()) { Shutdown(); return false; }
    const int initializeResult = fn_mpv_initialize(mpv);
    if (initializeResult < 0) {
        RecordInitializationFailure("mpv_initialize", initializeResult);
        fn_mpv_destroy ? fn_mpv_destroy(mpv) : fn_mpv_terminate_destroy(mpv);
        mpv = nullptr;
        ResetMpvApi();
        return false;
    }
    // Observe properties
    if (fn_mpv_observe_property) {
        fn_mpv_observe_property(mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
        fn_mpv_observe_property(mpv, 2, "duration", MPV_FORMAT_DOUBLE);
        fn_mpv_observe_property(mpv, 3, "pause", MPV_FORMAT_FLAG);
        fn_mpv_observe_property(mpv, 4, "volume", MPV_FORMAT_DOUBLE);
        fn_mpv_observe_property(mpv, 5, "mute", MPV_FORMAT_FLAG);
        fn_mpv_observe_property(mpv, 6, "speed", MPV_FORMAT_DOUBLE);
        fn_mpv_observe_property(mpv, 7, "eof-reached", MPV_FORMAT_FLAG);
        fn_mpv_observe_property(mpv, 8, "dwidth", MPV_FORMAT_INT64);
        fn_mpv_observe_property(mpv, 9, "dheight", MPV_FORMAT_INT64);
        fn_mpv_observe_property(mpv, 10, "track-list", MPV_FORMAT_NONE);
        fn_mpv_observe_property(mpv, 11, "chapter", MPV_FORMAT_INT64);
        fn_mpv_observe_property(mpv, 12, "chapter-list", MPV_FORMAT_NONE);
        fn_mpv_observe_property(mpv, 13, "audio-device-list", MPV_FORMAT_NONE);
    }
    isInitialized = true;
    isRunning = true;
    ended = false;
    { std::lock_guard<std::mutex> lock(stateMutex); lastError.clear(); }
    eventThread = std::thread(&MpvPlayer::EventLoop, this);
    if (customMpvConfigEnabled) {
        std::wstring confPath = Settings::ResolveMpvConfigPath(customMpvConfigPath);
        CustomMpvApplyResult res = LoadAndApplyCustomMpvConfig(confPath);
        if (!res.success) {
            customMpvConfigEnabled = false;
            ReportErrorMessage(Utf8ToUtf16("Custom MPV configuration failed to apply and was automatically disabled: " + res.errorMessage));
        }
    }
    return true;
}

void MpvPlayer::Shutdown() {
    isRunning = false;
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (mpv) {
            int ret = -1;
            if (fn_mpv_command_string) { ret = fn_mpv_command_string(mpv, "stop"); } else if (fn_mpv_command) {
                const char* cmd[] = { "stop", nullptr };
                ret = fn_mpv_command(mpv, cmd);
            }
            // A stop command can fail while a file is already idle. It should not prevent the event thread from being joined and the handle
            // from being destroyed.
            (void)ret;
            // mpv_wait_event() runs without mpvMutex. Wake it before joining;
            // the handle and DLL remain alive until the event thread exits.
            if (fn_mpv_wakeup) fn_mpv_wakeup(mpv);
        }
    }
    if (eventThread.joinable()) { eventThread.join(); }
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (mpv) {
            if (fn_mpv_terminate_destroy) fn_mpv_terminate_destroy(mpv);
            else if (fn_mpv_destroy) fn_mpv_destroy(mpv);
            mpv = nullptr;
        }
    }
    ResetMpvApi();
    isInitialized = false;
    ended = true;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        currentFilePath.clear();
        currentTime = 0.0;
        duration = 0.0;
        isPaused = true;
        videoWidth = 0;
        videoHeight = 0;
        audioTracks.clear();
        subTracks.clear();
        chapters.clear();
        chaptersVersion.fetch_add(1, std::memory_order_release);
        tracksVersion.fetch_add(1, std::memory_order_release);
        currentChapter = -1;
        lastError.clear();
    }
}

void MpvPlayer::HandleEofTransition() {
    // keep-open=yes intentionally retains the current file, so MPV_EVENT_END_FILE is not guaranteed to arrive. eof-reached is the
    // lifecycle signal for that state; END_FILE/EOF remains supported for
    // configurations where mpv unloads the file. Claim the EOF edge before either branch. eof-reached and MPV_EVENT_END_FILE can both
    // describe the same transition.
    if (ended.exchange(true, std::memory_order_acq_rel)) return;
    if (loop.load(std::memory_order_acquire)) {
        // Keep loop-current owned by MpvPlayer and suppress the host callback.
        Seek(0.0);
        Play();
        return;
    }
    { std::lock_guard<std::mutex> lock(stateMutex); isPaused = true; }
    auto callback = CopyCallback(onEnded);
    if (callback) callback();
}

void MpvPlayer::EventLoop() {
    while (isRunning && mpv && fn_mpv_wait_event) {
        // Use libmpv's wakeup mechanism when available so idle playback does
        // not poll 20 times per second. Keep a bounded wait for older DLLs.
        mpv_event* event = fn_mpv_wait_event(mpv, fn_mpv_wakeup ? -1.0 : 0.05);
        if (!event || event->event_id == MPV_EVENT_NONE) continue;
        if (event->event_id == MPV_EVENT_SHUTDOWN) { isRunning = false; break; }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property* prop = (mpv_event_property*)event->data;
            if (!prop || !prop->name) continue;
            if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                double t = *(double*)prop->data;
                if (!std::isfinite(t)) t = 0.0;
                t = std::max(0.0, t);
                { std::lock_guard<std::mutex> lock(stateMutex); currentTime = t; }
                auto callback = CopyCallback(onTimeUpdated);
                if (callback) callback(t);
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                double d = *(double*)prop->data;
                if (!std::isfinite(d) || d < 0.0) d = 0.0;
                { std::lock_guard<std::mutex> lock(stateMutex); duration = d; }
                auto callback = CopyCallback(onDurationChanged);
                if (callback) callback(d);
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG && prop->data) {
                int p = *(int*)prop->data;
                bool changed = false;
                { std::lock_guard<std::mutex> lock(stateMutex); changed = isPaused != (p != 0); isPaused = (p != 0); }
                if (changed) { auto callback = CopyCallback(onPlayStateChanged); if (callback) callback(p == 0); }
            }
            else if (strcmp(prop->name, "volume") == 0 && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                double v = *(double*)prop->data;
                if (!std::isfinite(v)) v = 0.0;
                std::lock_guard<std::mutex> lock(stateMutex);
                volume = std::clamp(v / 100.0, 0.0, 1.0);
            } else if (strcmp(prop->name, "mute") == 0 && prop->format == MPV_FORMAT_FLAG && prop->data) {
                int m = *(int*)prop->data;
                std::lock_guard<std::mutex> lock(stateMutex);
                isMuted = (m != 0);
            } else if (strcmp(prop->name, "speed") == 0 && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
                double s = *(double*)prop->data;
                if (!std::isfinite(s) || s <= 0.0) s = 1.0;
                std::lock_guard<std::mutex> lock(stateMutex);
                speed = s;
            } else if (strcmp(prop->name, "dwidth") == 0 && prop->format == MPV_FORMAT_INT64 && prop->data) {
                int64_t width = *(int64_t*)prop->data;
                std::lock_guard<std::mutex> lock(stateMutex);
                videoWidth = width > 0 ? (uint32_t)std::min<int64_t>(width, std::numeric_limits<uint32_t>::max()) : 0;
            } else if (strcmp(prop->name, "dheight") == 0 && prop->format == MPV_FORMAT_INT64 && prop->data) {
                int64_t height = *(int64_t*)prop->data;
                std::lock_guard<std::mutex> lock(stateMutex);
                videoHeight = height > 0 ? (uint32_t)std::min<int64_t>(height, std::numeric_limits<uint32_t>::max()) : 0;
            }
            else if (strcmp(prop->name, "track-list") == 0) { RefreshTracks(); }
            else if (strcmp(prop->name, "chapter") == 0 && prop->format == MPV_FORMAT_INT64 && prop->data) {
                int64_t chap = *(int64_t*)prop->data;
                bool changed = false;
                { std::lock_guard<std::mutex> lock(stateMutex); changed = (currentChapter != (int)chap); currentChapter = (int)chap; }
                if (changed) { auto callback = CopyCallback(onChaptersChanged); if (callback) callback(); }
            }
            else if (strcmp(prop->name, "chapter-list") == 0) { RefreshChapters(); }
            else if (strcmp(prop->name, "audio-device-list") == 0) { RefreshAudioDevices(); }
            else if (strcmp(prop->name, "eof-reached") == 0 && prop->format == MPV_FORMAT_FLAG && prop->data) {
                const bool eofReached = *(int*)prop->data != 0;
                if (eofReached) { HandleEofTransition(); } else {
                    // A seek-back/replay clears eof-reached. Re-arm the one-shot transition so a later EOF is delivered again.
                    ended.store(false, std::memory_order_release);
                }
            }
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            ended = false;
            { std::lock_guard<std::mutex> lock(stateMutex); lastError.clear(); }
            RefreshTracks();
            RefreshChapters();
            RefreshAudioDevices();
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file* end = static_cast<mpv_event_end_file*>(event->data);
            const mpv_end_file_reason reason = end ? end->reason : MPV_END_FILE_REASON_EOF;
            if (reason == MPV_END_FILE_REASON_ERROR) {
                { std::lock_guard<std::mutex> lock(stateMutex); isPaused = true; }
                ReportError("Playback error", end ? end->error : -1, ErrorSeverity::Fatal);
                continue;
            }
            if (reason == MPV_END_FILE_REASON_EOF) HandleEofTransition();
        }
    }
}

void MpvPlayer::RefreshTracks() {
    std::vector<MpvTrackInfo> newAudio;
    std::vector<MpvTrackInfo> newSub;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_get_property_string || !fn_mpv_get_property) return;
    int64_t trackCount = 0, secondarySid = 0;
    fn_mpv_get_property(mpv, "secondary-sid", MPV_FORMAT_INT64, &secondarySid);
    if (fn_mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &trackCount) >= 0) {
        for (int64_t i = 0; i < trackCount; ++i) {
            std::string prefix = "track-list/" + std::to_string(i) + "/";
            char* type = fn_mpv_get_property_string(mpv, (prefix + "type").c_str());
            if (!type) continue;
            int64_t id = 0;
            fn_mpv_get_property(mpv, (prefix + "id").c_str(), MPV_FORMAT_INT64, &id);
            int selected = 0;
            fn_mpv_get_property(mpv, (prefix + "selected").c_str(), MPV_FORMAT_FLAG, &selected);
            int defTrack = 0;
            fn_mpv_get_property(mpv, (prefix + "default").c_str(), MPV_FORMAT_FLAG, &defTrack);
            int external = 0;
            fn_mpv_get_property(mpv, (prefix + "external").c_str(), MPV_FORMAT_FLAG, &external);
            int forced = 0;
            fn_mpv_get_property(mpv, (prefix + "forced").c_str(), MPV_FORMAT_FLAG, &forced);
            char* title = fn_mpv_get_property_string(mpv, (prefix + "title").c_str());
            char* lang = fn_mpv_get_property_string(mpv, (prefix + "lang").c_str());
            char* codec = fn_mpv_get_property_string(mpv, (prefix + "codec").c_str());
            char* codecDesc = fn_mpv_get_property_string(mpv, (prefix + "codec-desc").c_str());
            int64_t channels = 0;
            fn_mpv_get_property(mpv, (prefix + "demux-channel-count").c_str(), MPV_FORMAT_INT64, &channels);
            MpvTrackInfo info;
            info.id = (int)id;
            info.type = type;
            info.isSelected = (selected != 0);
            info.isSecondarySelected = (id > 0 && id == secondarySid);
            info.isDefault = (defTrack != 0);
            info.isExternal = (external != 0);
            info.isForced = (forced != 0);
            info.channels = channels > 0 ? (int)std::min<int64_t>(channels, std::numeric_limits<int>::max()) : 0;
            info.title = title ? Utf8ToUtf16(title) : L"";
            info.lang = lang ? Utf8ToUtf16(lang) : L"";
            info.codec = codecDesc ? Utf8ToUtf16(codecDesc) : (codec ? Utf8ToUtf16(codec) : L"");
            char* externalFilename = info.isExternal ? fn_mpv_get_property_string(mpv, (prefix + "external-filename").c_str()) : nullptr;
            if (info.title.empty()) {
                if (info.isExternal && externalFilename && *externalFilename) {
                    const std::wstring externalPath = Utf8ToUtf16(externalFilename);
                    info.title = L"External: " + GetFileNameFromPath(externalPath);
                } else if (!info.lang.empty()) info.title = info.lang;
                else info.title = (info.type == "audio") ? (L"Audio Track " + std::to_wstring(id)) : (L"Subtitle " + std::to_wstring(id));
            }
            if (strcmp(type, "audio") == 0 && !info.codec.empty()) { info.title += L" (" + info.codec + L")"; }
            if (strcmp(type, "audio") == 0) { newAudio.push_back(info); } else if (strcmp(type, "sub") == 0) { newSub.push_back(info); }
            if (fn_mpv_free) {
                if (type) fn_mpv_free(type);
                if (title) fn_mpv_free(title);
                if (lang) fn_mpv_free(lang);
                if (codec) fn_mpv_free(codec);
                if (codecDesc) fn_mpv_free(codecDesc);
                if (externalFilename) fn_mpv_free(externalFilename);
            }
        }
    }
    }
    // Subtitle menus should read like a player UI, not a codec inspector.
    // Keep the human label alone when it is already unique; only add the
    // smallest useful qualifier when two tracks would otherwise look alike.
    for (size_t i = 0; i < newSub.size(); ++i) {
        bool duplicateBase = false;
        for (size_t j = 0; j < newSub.size(); ++j) {
            if (i != j && newSub[i].title == newSub[j].title) { duplicateBase = true; break; }
        }
        if (!duplicateBase) continue;
        if (!newSub[i].lang.empty() && newSub[i].title != newSub[i].lang) {
            newSub[i].title += L" (" + newSub[i].lang + L")";
        } else if (newSub[i].isExternal) { newSub[i].title += L" (External)"; } else if (newSub[i].isForced) {
            newSub[i].title += L" (Forced)";
        } else if (!newSub[i].codec.empty()) { newSub[i].title += L" (" + newSub[i].codec + L")"; }
    }
    // If the minimal qualifier still collides, the track id is the final, deterministic discriminator. This should be rare and keeps normal
    // subtitle menus free of technical detail.
    for (size_t i = 0; i < newSub.size(); ++i) {
        bool stillDuplicate = false;
        for (size_t j = 0; j < newSub.size(); ++j) {
            if (i != j && newSub[i].title == newSub[j].title) { stillDuplicate = true; break; }
        }
        if (stillDuplicate) { newSub[i].title += L" · Track " + std::to_wstring(newSub[i].id); }
    }
    bool changed = false;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto sameTracks = [](const std::vector<MpvTrackInfo>& a, const std::vector<MpvTrackInfo>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                const MpvTrackInfo& x = a[i];
                const MpvTrackInfo& y = b[i];
                if (x.id != y.id || x.type != y.type || x.title != y.title || x.lang != y.lang ||
                    x.codec != y.codec || x.isSelected != y.isSelected || x.isDefault != y.isDefault ||
                    x.isExternal != y.isExternal || x.isForced != y.isForced || x.channels != y.channels) {
                    return false;
                }
            }
            return true;
        };
        changed = !sameTracks(audioTracks, newAudio) || !sameTracks(subTracks, newSub);
        audioTracks = newAudio;
        subTracks = newSub;
        if (changed) { callback = CopyCallback(onTracksChanged); tracksVersion.fetch_add(1, std::memory_order_release); }
    }
    if (callback) { callback(); }
}

void MpvPlayer::RefreshChapters() {
    std::vector<MpvChapterInfo> newChapters;
    int newCurrentChapter = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_get_property) return;
        int64_t chapterCount = 0;
        if (fn_mpv_get_property(mpv, "chapter-list/count", MPV_FORMAT_INT64, &chapterCount) >= 0 && chapterCount > 0) {
            int64_t boundedCount = std::min<int64_t>(chapterCount, 1000);
            newChapters.reserve(static_cast<size_t>(boundedCount));
            for (int64_t i = 0; i < boundedCount; ++i) {
                std::string prefix = "chapter-list/" + std::to_string(i) + "/";
                double chTime = 0.0;
                if (fn_mpv_get_property(mpv, (prefix + "time").c_str(), MPV_FORMAT_DOUBLE, &chTime) < 0) { chTime = 0.0; }
                if (!std::isfinite(chTime) || chTime < 0.0) { chTime = 0.0; }
                char* title = fn_mpv_get_property_string ? fn_mpv_get_property_string(mpv, (prefix + "title").c_str()) : nullptr;
                std::wstring titleW;
                if (title && *title) {
                    titleW = Utf8ToUtf16(title);
                    if (titleW.length() > 128) { titleW = titleW.substr(0, 125) + L"..."; }
                } else { titleW = L"Chapter " + std::to_wstring(i + 1); }
                if (fn_mpv_free && title) { fn_mpv_free(title); }
                MpvChapterInfo info;
                info.index = static_cast<int>(i);
                info.title = titleW;
                info.time = chTime;
                newChapters.push_back(info);
            }
        }
        int64_t curChap = -1;
        if (fn_mpv_get_property(mpv, "chapter", MPV_FORMAT_INT64, &curChap) >= 0) {
            if (curChap >= 0 && curChap < static_cast<int64_t>(newChapters.size())) {
                newCurrentChapter = static_cast<int>(curChap);
            } else { newCurrentChapter = -1; }
        }
    }
    bool changed = false;
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto sameChapters = [](const std::vector<MpvChapterInfo>& a, const std::vector<MpvChapterInfo>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (a[i].index != b[i].index || a[i].title != b[i].title || std::abs(a[i].time - b[i].time) > 0.001) { return false; }
            }
            return true;
        };
        changed = !sameChapters(chapters, newChapters) || (currentChapter != newCurrentChapter);
        chapters = std::move(newChapters);
        currentChapter = newCurrentChapter;
        if (changed) { callback = CopyCallback(onChaptersChanged); chaptersVersion.fetch_add(1, std::memory_order_release); }
    }
    if (callback) { callback(); }
}

bool MpvPlayer::Open(const std::wstring& filePath, double startSec) {
    if (filePath.empty()) return false;
    if (!std::isfinite(startSec) || startSec < 0.0) startSec = 0.0;
    std::string utf8Path = Utf16ToUtf8(filePath);
    std::string startOption;
    if (startSec > 0.0) {
        std::ostringstream start;
        start << "start=" << std::fixed << std::setprecision(3) << startSec;
        startOption = start.str();
    }
    ended = true; // suppress a late END_FILE from the file being replaced
    int ret = -1;
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv || !fn_mpv_command) return false;
        if (!startOption.empty()) {
            const char* cmd[] = { "loadfile", utf8Path.c_str(), "replace", "-1", startOption.c_str(), nullptr };
            ret = fn_mpv_command(mpv, cmd);
        } else {
            const char* cmd[] = { "loadfile", utf8Path.c_str(), "replace", nullptr };
            ret = fn_mpv_command(mpv, cmd);
        }
    }
    if (ret < 0) { ReportError("Unable to open media", ret, ErrorSeverity::Fatal); return false; }
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        currentFilePath = filePath;
        currentTime = startSec;
        duration = 0.0;
        isPaused = true;
        videoWidth = 0;
        videoHeight = 0;
        audioTracks.clear();
        subTracks.clear();
        tracksVersion.fetch_add(1, std::memory_order_release);
        lastError.clear();
    }
    Play();
    return true;
}

void MpvPlayer::Close() {
    int ret = 0;
    bool commandIssued = false;
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (mpv) {
            if (fn_mpv_command_string) { commandIssued = true; ret = fn_mpv_command_string(mpv, "stop"); } else if (fn_mpv_command) {
                const char* cmd[] = { "stop", nullptr };
                commandIssued = true;
                ret = fn_mpv_command(mpv, cmd);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        currentFilePath.clear();
        currentTime = 0.0;
        duration = 0.0;
        isPaused = true;
        videoWidth = 0;
        videoHeight = 0;
        audioTracks.clear();
        subTracks.clear();
        chapters.clear();
        chaptersVersion.fetch_add(1, std::memory_order_release);
        tracksVersion.fetch_add(1, std::memory_order_release);
        currentChapter = -1;
        lastError.clear();
    }
    ended = true;
    if (commandIssued && ret < 0) ReportError("Unable to close media", ret);
}

void MpvPlayer::Play() {
    bool changed = false;
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        int p = 0;
        if (fn_mpv_set_property) {
            ret = fn_mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &p);
        } else if (fn_mpv_set_property_string) { ret = fn_mpv_set_property_string(mpv, "pause", "no"); }
        if (ret >= 0) { std::lock_guard<std::mutex> stateLock(stateMutex); changed = isPaused; isPaused = false; }
    }
    if (ret < 0) { ReportError("Unable to play media", ret); return; }
    if (changed) { auto callback = CopyCallback(onPlayStateChanged); if (callback) callback(true); }
}

void MpvPlayer::Pause() {
    bool changed = false;
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        int p = 1;
        if (fn_mpv_set_property) {
            ret = fn_mpv_set_property(mpv, "pause", MPV_FORMAT_FLAG, &p);
        } else if (fn_mpv_set_property_string) { ret = fn_mpv_set_property_string(mpv, "pause", "yes"); }
        if (ret >= 0) { std::lock_guard<std::mutex> stateLock(stateMutex); changed = !isPaused; isPaused = true; }
    }
    if (ret < 0) { ReportError("Unable to pause media", ret); return; }
    if (changed) { auto callback = CopyCallback(onPlayStateChanged); if (callback) callback(false); }
}

void MpvPlayer::TogglePlay() {
    bool currentlyPlaying = IsPlaying();
    if (currentlyPlaying) { Pause(); } else { Play(); }
}

void MpvPlayer::Stop() { Close(); }

void MpvPlayer::Seek(double timeSec, bool exact) {
    if (!std::isfinite(timeSec)) return;
    timeSec = std::max(0.0, timeSec);
    double knownDuration = GetDuration();
    if (knownDuration > 0.0) timeSec = std::min(timeSec, knownDuration);
    std::ostringstream value;
    value << std::fixed << std::setprecision(3) << timeSec;
    std::string position = value.str();
    int ret = -1;
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv || !fn_mpv_command) return;
        const char* cmd[] = { "seek", position.c_str(), exact ? "absolute+exact" : "absolute+keyframes", nullptr };
        ret = fn_mpv_command(mpv, cmd);
        if (ret < 0) {
            const char* fallbackCmd[] = { "seek", position.c_str(), "absolute", nullptr };
            ret = fn_mpv_command(mpv, fallbackCmd);
        }
    }
    if (ret < 0) { ReportError("Unable to seek", ret); return; }
    ended = false;
    // Adopt the requested position immediately. mpv seeks are asynchronous;
    // without this the UI timeline and the subtitle lookup keep running on the pre-seek time until the next observed time-pos event
    // arrives, which hides a subtitle line when seeking into the middle of it.
    { std::lock_guard<std::mutex> lock(stateMutex); currentTime = timeSec; }
}

void MpvPlayer::SeekRelative(double deltaSec) {
    if (!std::isfinite(deltaSec)) return;
    // Step from mpv's live position (which already reflects any pending seek
    // target) so rapid successive steps accumulate exactly and in order. The
    // observed-property cache is only a fallback; Seek() then adopts the new
    // target immediately.
    double base = GetCurrentTime();
    {
        std::lock_guard<std::mutex> lock(mpvMutex);
        double live = 0.0;
        if (mpv && fn_mpv_get_property && fn_mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &live) >= 0 && std::isfinite(live))
            base = std::max(0.0, live);
    }
    Seek(base + deltaSec);
}
void MpvPlayer::SetABLoopA(double timeSec) {
    if (!std::isfinite(timeSec) || timeSec < 0.0) return;
    std::ostringstream value;
    value << std::fixed << std::setprecision(3) << timeSec;
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (mpv && fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "ab-loop-a", value.str().c_str());
}

void MpvPlayer::SetABLoopB(double timeSec) {
    if (!std::isfinite(timeSec) || timeSec < 0.0) return;
    std::ostringstream value;
    value << std::fixed << std::setprecision(3) << timeSec;
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (mpv && fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "ab-loop-b", value.str().c_str());
}

void MpvPlayer::ClearABLoop() {
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (!mpv || !fn_mpv_set_property_string) return;
    fn_mpv_set_property_string(mpv, "ab-loop-a", "no");
    fn_mpv_set_property_string(mpv, "ab-loop-b", "no");
}
void MpvPlayer::StepFrame(int frameCount) {
    if (frameCount == 0) return;
    const char* command = frameCount > 0 ? "frame-step" : "frame-back-step";
    const int count = frameCount == std::numeric_limits<int>::min() ? 1000 : std::min(std::abs(frameCount), 1000);
    for (int i = 0; i < count; ++i) {
        int ret = -1;
        {
            std::lock_guard<std::mutex> lock(mpvMutex);
            if (!mpv) return;
            if (fn_mpv_command_string) { ret = fn_mpv_command_string(mpv, command); } else if (fn_mpv_command) {
                const char* args[] = { command, nullptr };
                ret = fn_mpv_command(mpv, args);
            }
        }
        if (ret < 0) { ReportError("Unable to step frame", ret); return; }
    }
    ended = false;
}

void MpvPlayer::SetVolume(double volume0to1) {
    if (!std::isfinite(volume0to1)) volume0to1 = 0.0;
    double normalized = std::clamp(volume0to1, 0.0, 1.0);
    double v = normalized * 100.0;
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) { std::lock_guard<std::mutex> lock(stateMutex); volume = normalized; return; }
        if (fn_mpv_set_property) { ret = fn_mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &v); } else if (fn_mpv_set_property_string) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << v;
            ret = fn_mpv_set_property_string(mpv, "volume", value.str().c_str());
        }
    }
    if (ret < 0) { ReportError("Unable to set volume", ret); return; }
    std::lock_guard<std::mutex> lock(stateMutex);
    volume = normalized;
}

double MpvPlayer::GetVolume() const { std::lock_guard<std::mutex> lock(stateMutex); return volume; }

void MpvPlayer::SetMuted(bool mute) {
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) { std::lock_guard<std::mutex> lock(stateMutex); isMuted = mute; return; }
        if (fn_mpv_set_property_string) {
            ret = fn_mpv_set_property_string(mpv, "mute", mute ? "yes" : "no");
        } else if (fn_mpv_set_property) { int value = mute ? 1 : 0; ret = fn_mpv_set_property(mpv, "mute", MPV_FORMAT_FLAG, &value); }
    }
    if (ret < 0) { ReportError("Unable to set mute", ret); return; }
    std::lock_guard<std::mutex> lock(stateMutex);
    isMuted = mute;
}

bool MpvPlayer::IsMuted() const { std::lock_guard<std::mutex> lock(stateMutex); return isMuted; }

void MpvPlayer::SetPlaybackRate(double rate) {
    if (!std::isfinite(rate)) rate = 1.0;
    double s = std::clamp(rate, 0.25, 4.0);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) { std::lock_guard<std::mutex> lock(stateMutex); speed = s; return; }
        if (fn_mpv_set_property) { ret = fn_mpv_set_property(mpv, "speed", MPV_FORMAT_DOUBLE, &s); } else if (fn_mpv_set_property_string) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << s;
            ret = fn_mpv_set_property_string(mpv, "speed", value.str().c_str());
        }
    }
    if (ret < 0) { ReportError("Unable to set playback speed", ret); return; }
    std::lock_guard<std::mutex> lock(stateMutex);
    speed = s;
}

double MpvPlayer::GetPlaybackRate() const { std::lock_guard<std::mutex> lock(stateMutex); return speed; }

void MpvPlayer::SetLoop(bool lp) { loop = lp; }

bool MpvPlayer::GetLoop() const { return loop; }

void MpvPlayer::SetAspectRatio(const std::string& mode) {
    int ret = 0;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        auto set = [&](const char* property, const char* value) { if (ret >= 0) ret = fn_mpv_set_property_string(mpv, property, value); };
        if (mode == "fill") {
            set("video-aspect-override", "-1");
            set("keepaspect", "no");
            set("video-unscaled", "no");
            set("panscan", "0");
        } else if (mode == "cover") {
            set("video-aspect-override", "-1");
            set("keepaspect", "yes");
            set("video-unscaled", "no");
            set("panscan", "1");
        } else {
            set("video-aspect-override", (mode == "16:9" || mode == "4:3" || mode == "21:9") ? mode.c_str() : "-1");
            set("keepaspect", "yes");
            set("video-unscaled", "no");
            set("panscan", "0");
        }
    }
    if (ret < 0) ReportError("Unable to set aspect ratio", ret);
}

void MpvPlayer::SetVideoRotate(int degrees) {
    int normalized = ((degrees % 360) + 360) % 360;
    if (normalized != 0 && normalized != 90 && normalized != 180 && normalized != 270) { normalized = 0; }
    videoRotate = normalized;
    int ret = 0;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        if (fn_mpv_set_property) {
            int64_t val = normalized;
            ret = fn_mpv_set_property(mpv, "video-rotate", MPV_FORMAT_INT64, &val);
        } else if (fn_mpv_set_property_string) {
            ret = fn_mpv_set_property_string(mpv, "video-rotate", std::to_string(normalized).c_str());
        }
    }
    if (ret < 0) ReportError("Unable to set video-rotate", ret);
}

int MpvPlayer::GetVideoRotate() const { return videoRotate; }

void MpvPlayer::SetImageAdjustment(int& state, int val, const char* property, const char* operation) {
    val = std::clamp(val, -100, 100);
    state = val;
    int ret = 0;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        if (fn_mpv_set_property) {
            int64_t v = val;
            ret = fn_mpv_set_property(mpv, property, MPV_FORMAT_INT64, &v);
        } else if (fn_mpv_set_property_string) { ret = fn_mpv_set_property_string(mpv, property, std::to_string(val).c_str()); }
    }
    if (ret < 0) ReportError(operation, ret);
}

void MpvPlayer::SetBrightness(int val) { SetImageAdjustment(brightness, val, "brightness", "Unable to set brightness"); }

int MpvPlayer::GetBrightness() const { return brightness; }

void MpvPlayer::SetContrast(int val) { SetImageAdjustment(contrast, val, "contrast", "Unable to set contrast"); }

int MpvPlayer::GetContrast() const { return contrast; }

void MpvPlayer::SetSaturation(int val) { SetImageAdjustment(saturation, val, "saturation", "Unable to set saturation"); }

int MpvPlayer::GetSaturation() const { return saturation; }

void MpvPlayer::SetGamma(int val) { SetImageAdjustment(gamma, val, "gamma", "Unable to set gamma"); }

int MpvPlayer::GetGamma() const { return gamma; }

void MpvPlayer::SetHue(int val) { SetImageAdjustment(hue, val, "hue", "Unable to set hue"); }

int MpvPlayer::GetHue() const { return hue; }

void MpvPlayer::ResetVideoAdjustments() {
    SetVideoRotate(0);
    SetBrightness(0);
    SetContrast(0);
    SetSaturation(0);
    SetGamma(0);
    SetHue(0);
}

void MpvPlayer::SetAudioDelay(double delaySec) {
    if (!std::isfinite(delaySec)) delaySec = 0.0;
    delaySec = std::clamp(delaySec, -5.0, 5.0);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        if (fn_mpv_set_property) {
            ret = fn_mpv_set_property(mpv, "audio-delay", MPV_FORMAT_DOUBLE, &delaySec);
        } else if (fn_mpv_set_property_string) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << delaySec;
            std::string delay = value.str();
            ret = fn_mpv_set_property_string(mpv, "audio-delay", delay.c_str());
        }
    }
    if (ret < 0) ReportError("Unable to set audio delay", ret);
}

void MpvPlayer::SetSubDelay(double delaySec) {
    if (!std::isfinite(delaySec)) delaySec = 0.0;
    delaySec = std::clamp(delaySec, -10.0, 10.0);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        if (fn_mpv_set_property) {
            ret = fn_mpv_set_property(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &delaySec);
        } else if (fn_mpv_set_property_string) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3) << delaySec;
            std::string delay = value.str();
            ret = fn_mpv_set_property_string(mpv, "sub-delay", delay.c_str());
        }
    }
    if (ret < 0) ReportError("Unable to set subtitle delay", ret);
}

void MpvPlayer::SetHardwareAcceleration(bool enabled) {
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "hwdec", enabled ? "d3d11va" : "no");
    }
    if (ret < 0) ReportError("Unable to set hardware acceleration", ret);
}

void MpvPlayer::SetPreferredAudioLanguage(const std::string& language) {
    const std::string normalized = NormalizeLanguageList(language);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "alang", normalized.c_str());
    }
    if (ret < 0) ReportError("Unable to set preferred audio language", ret);
}

void MpvPlayer::SetPreferredSubtitleLanguage(const std::string& language) {
    const std::string normalized = NormalizeLanguageList(language);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "slang", normalized.c_str());
    }
    if (ret < 0) ReportError("Unable to set preferred subtitle language", ret);
}

void MpvPlayer::SetSubtitleStyle(const std::string& fontSize, const std::string& colorHex, const std::string& backgroundStyle,
                                 const std::string& position, int safeAreaX, int safeAreaY) {
    lastSubSafeAreaX = Settings::NormalizeSafeAreaPercent(safeAreaX);
    lastSubSafeAreaY = Settings::NormalizeSafeAreaPercent(safeAreaY);
    SubtitleSafeAreaPlan safePlan = BuildSubtitleSafeAreaPlan(safeAreaX, safeAreaY);
    const std::string normalizedFontSize = NormalizeSubtitleFontSize(fontSize);
    const std::string normalizedColor = NormalizeSubtitleColor(colorHex);
    const std::string style = LowerAscii(TrimAscii(backgroundStyle));
    const char* backColor = "#00000000";
    const char* borderStyle = "outline-and-shadow";
    const char* borderSize = "0";
    const char* shadowOffset = "1.5";
    // Keep these mappings aligned with Settings::subBgStyle. mpv has no
    // single "background style" property; the visual styles are composed from
    // libass background, border, and shadow properties.
    if (style == "vlc-outline") {
        backColor = "#00000000";
        borderSize = "3";
        shadowOffset = "1.5";
    } else if (style == "translucent") {
        backColor = "#73000000";
        borderStyle = "background-box";
        borderSize = "0";
        shadowOffset = "0";
    } else if (style == "solid") {
        backColor = "#d9000000";
        borderStyle = "background-box";
        borderSize = "0";
        shadowOffset = "0";
    } else if (style == "clean") {
        backColor = "#00000000";
        borderSize = "0";
        shadowOffset = "1.5";
    }
    const char* subPosition = NormalizeSubtitlePosition(position);
    int ret = 0;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        auto set = [&](const char* property, const char* value) { if (ret >= 0) ret = fn_mpv_set_property_string(mpv, property, value); };
        set("sub-font-size", normalizedFontSize.c_str());
        set("sub-color", normalizedColor.c_str());
        set("sub-back-color", backColor);
        set("sub-border-style", borderStyle);
        set("sub-border-color", "#000000");
        set("sub-border-size", borderSize);
        set("sub-shadow-color", "#000000");
        set("sub-shadow-offset", shadowOffset);
        set("sub-pos", subPosition);
        for (const auto& action : safePlan.actions) { set(action.first.c_str(), action.second.c_str()); }
    }
    if (ret < 0) ReportError("Unable to set subtitle style", ret);
}

SubtitleSafeAreaPlan MpvPlayer::BuildSubtitleSafeAreaPlan(int safeAreaX, int safeAreaY) {
    SubtitleSafeAreaPlan plan;
    plan.requestedSafeAreaX = safeAreaX;
    plan.requestedSafeAreaY = safeAreaY;
    plan.normalizedSafeAreaX = Settings::NormalizeSafeAreaPercent(safeAreaX);
    plan.normalizedSafeAreaY = Settings::NormalizeSafeAreaPercent(safeAreaY);
    // Baseline 720p subtitle virtual layout (1280x720) in mpv
    plan.marginX = static_cast<int>(std::round((plan.normalizedSafeAreaX / 100.0) * 1280.0));
    plan.marginY = static_cast<int>(std::round((plan.normalizedSafeAreaY / 100.0) * 720.0));
    plan.actions.push_back({ "sub-margin-x", std::to_string(plan.marginX) });
    plan.actions.push_back({ "sub-margin-y", std::to_string(plan.marginY) });
    plan.actions.push_back({ "sub-use-margins", "yes" });
    return plan;
}

ColorManagementPlan MpvPlayer::BuildColorManagementPlan( const std::string& hdrMode, const std::string& toneMapping, int targetPeak,
    const std::string& gamutMapping, const std::string& iccProfilePath, const std::wstring& exeDir ) {
    ColorManagementPlan plan;
    plan.requestedHdrMode = hdrMode;
    plan.requestedToneMapping = toneMapping;
    plan.requestedTargetPeak = targetPeak;
    plan.requestedGamutMapping = gamutMapping;
    plan.requestedIccProfile = iccProfilePath;
    plan.normalizedHdrMode = Settings::NormalizeHdrMode(hdrMode);
    plan.normalizedToneMapping = Settings::NormalizeToneMapping(toneMapping);
    plan.normalizedTargetPeak = Settings::NormalizeTargetPeak(targetPeak);
    plan.normalizedGamutMapping = Settings::NormalizeGamutMapping(gamutMapping);
    plan.normalizedIccProfile = Settings::NormalizeIccPath(iccProfilePath, exeDir);
    if (!plan.normalizedIccProfile.empty()) {
        std::wstring baseDir = exeDir;
        while (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/')) { baseDir.pop_back(); }
        std::wstring resolved = baseDir + L"\\" + Utf8ToUtf16(plan.normalizedIccProfile);
        wchar_t buf[32768] = { 0 };
        if (GetFullPathNameW(resolved.c_str(), 32768, buf, nullptr)) {
            plan.resolvedIccPath = buf;
        } else { plan.resolvedIccPath = resolved; }
    }
    // Generate property actions
    if (plan.normalizedHdrMode == "passthrough") {
        plan.actions.push_back({ "target-colorspace-hint", "yes" });
        plan.actions.push_back({ "tone-mapping", "auto" });
        plan.actions.push_back({ "target-peak", plan.normalizedTargetPeak > 0 ? std::to_string(plan.normalizedTargetPeak) : "auto" });
        plan.actions.push_back({ "hdr-compute-peak", "auto" });
    } else if (plan.normalizedHdrMode == "tone-map") {
        plan.actions.push_back({ "target-colorspace-hint", "no" });
        plan.actions.push_back({ "tone-mapping", plan.normalizedToneMapping == "auto" ? "bt.2390" : plan.normalizedToneMapping });
        plan.actions.push_back({ "target-peak", plan.normalizedTargetPeak > 0 ? std::to_string(plan.normalizedTargetPeak) : "auto" });
        plan.actions.push_back({ "hdr-compute-peak", "yes" });
    } else if (plan.normalizedHdrMode == "off") {
        plan.actions.push_back({ "target-colorspace-hint", "no" });
        plan.actions.push_back({ "tone-mapping", "clip" });
        plan.actions.push_back({ "target-peak", "auto" });
        plan.actions.push_back({ "hdr-compute-peak", "no" });
    } else { // "auto"
        plan.actions.push_back({ "target-colorspace-hint", "yes" });
        plan.actions.push_back({ "tone-mapping", plan.normalizedToneMapping == "auto" ? "auto" : plan.normalizedToneMapping });
        plan.actions.push_back({ "target-peak", plan.normalizedTargetPeak > 0 ? std::to_string(plan.normalizedTargetPeak) : "auto" });
        plan.actions.push_back({ "hdr-compute-peak", "yes" });
    }
    // Gamut mapping
    if (plan.normalizedGamutMapping != "auto") {
        plan.actions.push_back({ "gamut-mapping-mode", plan.normalizedGamutMapping });
    } else {
        plan.actions.push_back({ "gamut-mapping-mode", "auto" });
    }
    // ICC profile
    if (!plan.resolvedIccPath.empty() && FileExists(plan.resolvedIccPath)) {
        plan.actions.push_back({ "icc-profile", Utf16ToUtf8(plan.resolvedIccPath) });
        plan.actions.push_back({ "icc-profile-auto", "no" });
    } else {
        plan.actions.push_back({ "icc-profile", "" });
        plan.actions.push_back({ "icc-profile-auto", "no" });
    }
    return plan;
}

std::vector<ColorPropertyResult> MpvPlayer::SetColorPreferences( const std::string& hdrMode, const std::string& toneMapping, int targetPeak,
    const std::string& gamutMapping, const std::string& iccProfilePath ) {
    ColorManagementPlan plan = BuildColorManagementPlan(hdrMode, toneMapping, targetPeak, gamutMapping, iccProfilePath, GetExeDir());
    std::vector<ColorPropertyResult> results;
    { std::lock_guard<std::mutex> lock(stateMutex); lastColorPlan = plan; lastColorResults.clear(); }
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_set_property_string) {
        for (const auto &act : plan.actions) { results.push_back({act.property, act.value, false, -1, "mpv not initialized"}); }
        std::lock_guard<std::mutex> lock(stateMutex);
        lastColorResults = results;
        return results;
    }
    for (const auto& act : plan.actions) {
        int ret = fn_mpv_set_property_string(mpv, act.property.c_str(), act.value.c_str());
        ColorPropertyResult res;
        res.property = act.property;
        res.value = act.value;
        res.success = (ret >= 0);
        res.mpvErrorCode = ret;
        if (ret < 0 && fn_mpv_error_string) {
            const char* errStr = fn_mpv_error_string(ret);
            res.message = errStr ? errStr : "Unknown error";
        }
        results.push_back(res);
    }
    { std::lock_guard<std::mutex> lock(stateMutex); lastColorResults = results; }
    return results;
}

ColorManagementPlan MpvPlayer::GetLastColorPlan() const { std::lock_guard<std::mutex> lock(stateMutex); return lastColorPlan; }

bool MpvPlayer::IsCustomMpvOptionSafe(const std::string& rawName, const std::string& rawValue, std::string* outReason) {
    std::string name = TrimAscii(rawName);
    std::string value = TrimAscii(rawValue);
    // Strip leading '--' if present
    while (name.size() >= 2 && name[0] == '-' && name[1] == '-') { name = name.substr(2); }
    name = TrimAscii(name);
    if (name.empty()) { if (outReason) *outReason = "Option name is empty"; return false; }
    // Check name characters: must be ASCII alphanumeric, '-', or '_'
    for (char ch : name) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') {
            if (outReason) *outReason = "Option name contains invalid characters: " + name;
            return false;
        }
    }
    if (name.size() > 64) { if (outReason) *outReason = "Option name exceeds maximum length (64 characters): " + name; return false; }
    // Value bounds and character checks
    if (value.size() > 512) {
        if (outReason) *outReason = "Option value exceeds maximum length (512 characters) for " + name;
        return false;
    }
    for (char ch : value) {
        if (ch == '\0' || ch == '\r' || ch == '\n' || ch == '`' || ch == '$' || ch == ';' || ch == '|' || ch == '&') {
            if (outReason) *outReason = "Option value contains forbidden/unsafe characters for " + name;
            return false;
        }
    }
    // Reject path traversal / root escape attempts in values
    if (value.find("..\\") != std::string::npos || value.find("../") != std::string::npos ||
        value.find(":\\") != std::string::npos || value.find("://") != std::string::npos) {
        if (outReason) *outReason = "Option value contains path traversal or external URL for " + name;
        return false;
    }
    std::string lowerName = LowerAscii(name);
    // If name starts with "no-", extract base name for checking
    std::string baseName = lowerName;
    if (baseName.size() > 3 && baseName.substr(0, 3) == "no-") { baseName = baseName.substr(3); }
    // Denylist of unsafe option names & prefixes
    static const char* kUnsafeExactNames[] = {
        // Scripting / IPC / code execution
        "script", "scripts", "script-opts", "script-opts-append", "script-opts-add", "script-opts-remove", "script-opts-set",
        "script-opts-clear", "lua", "js", "javascript", "load-scripts", "ipc", "input-ipc-server", "input-ipc-client", "input-test",
        "input-keylist", "input-bindings", "input-conf", "input-builtin-bindings", "input-terminal", "terminal", "osc",
        // Config / profile / recursion
        "include", "config", "config-dir", "load-config", "use-filedir-conf", "reset-on-next-file", "profile", "show-profile",
        // Storage / file dumps / filesystem paths / directory overrides
        "log-file", "dump-stats", "record-file", "stream-dump", "o", "of", "ovc", "ovac", "ofopts", "audio-file", "sub-file",
        "external-file", "sub-files", "audio-files", "screenshot-directory", "screenshot-template", "screenshot-tag-colorspace",
        "gpu-shader-cache-dir", "sub-fonts-dir", "fonts-dir", "directory-mode",
        // Network / proxy / protocol / TLS
        "http-proxy", "ytdl", "ytdl-format", "ytdl-raw-options", "network-timeout",
        "tls-ca-file", "tls-cert-file", "tls-key-file", "tls-verify", "cookies", "cookies-file", "user-agent", "referrer", "stream-lavf-o",
        // Host window / library / driver hijacking
        "wid", "force-window-position", "input-default-bindings", "input-vo-keyboard", "input-media-keys", "dll", "vo-null", "ao-null",
        "sub-font-provider"};
    for (const char* unsafe : kUnsafeExactNames) {
        if (lowerName == unsafe || baseName == unsafe) {
            if (outReason) *outReason = "Option '" + rawName + "' is restricted for security (scripts, config, storage, network, or host integration).";
            return false;
        }
    }
    static const char *kUnsafePrefixes[] = {"script-", "script_", "ipc-",  "ipc_",  "input-ipc",      "input_ipc", "input-",
                                            "tls-",    "tls_",    "ytdl-", "ytdl_", "screenshot-dir", "log-",      "load-"};
    for (const char* prefix : kUnsafePrefixes) {
        if (lowerName.rfind(prefix, 0) == 0 || baseName.rfind(prefix, 0) == 0) {
            if (outReason) *outReason = "Option '" + rawName + "' matches restricted prefix '" + prefix + "'";
            return false;
        }
    }
    return true;
}

CustomMpvParseResult MpvPlayer::ParseCustomMpvConfig(const std::string& text) {
    CustomMpvParseResult result;
    if (text.size() > 64 * 1024) {
        result.success = false;
        result.errors.push_back("Configuration exceeds maximum allowed file size of 64 KiB.");
        return result;
    }
    std::istringstream stream(text);
    std::string line;
    int lineNum = 0;
    while (std::getline(stream, line)) {
        lineNum++;
        result.totalLines++;
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue; // Skip comments and empty lines
        }
        if (trimmed.size() > 1024) {
            result.warnings.push_back("Line " + std::to_string(lineNum) + " exceeds maximum line length (1024) and was skipped.");
            continue;
        }
        // Parse key and value
        std::string key, val;
        size_t eqPos = trimmed.find('=');
        size_t colonPos = trimmed.find(':');
        size_t delim = std::string::npos;
        if (eqPos != std::string::npos && colonPos != std::string::npos) {
            delim = std::min(eqPos, colonPos);
        } else if (eqPos != std::string::npos) { delim = eqPos; } else if (colonPos != std::string::npos) { delim = colonPos; }
        if (delim != std::string::npos) { key = TrimAscii(trimmed.substr(0, delim)); val = TrimAscii(trimmed.substr(delim + 1)); } else {
            // Check for whitespace delimiter or boolean flag
            size_t spacePos = 0;
            while (spacePos < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[spacePos]))) { spacePos++; }
            if (spacePos < trimmed.size()) {
                key = TrimAscii(trimmed.substr(0, spacePos));
                val = TrimAscii(trimmed.substr(spacePos + 1));
            } else { key = trimmed; val = "yes"; }
        }
        // Strip quotes around value if present
        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\'')))
            val = val.substr(1, val.size() - 2);
        // Strip leading '--' from key
        while (key.size() >= 2 && key[0] == '-' && key[1] == '-') { key = key.substr(2); }
        key = TrimAscii(key);
        if (key.empty()) continue;
        std::string reason;
        if (!IsCustomMpvOptionSafe(key, val, &reason)) {
            result.warnings.push_back("Line " + std::to_string(lineNum) + " rejected: " + reason);
            continue;
        }
        if (result.options.size() >= 128) { result.warnings.push_back("Option limit (128) reached. Subsequent options ignored."); break; }
        result.options.push_back({ key, val, lineNum });
    }
    return result;
}

CustomMpvApplyResult MpvPlayer::ApplyCustomMpvOptions(const std::vector<CustomMpvOption>& options) {
    CustomMpvApplyResult result;
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_set_property_string) {
        result.success = false;
        result.errorMessage = "mpv player is not initialized";
        return result;
    }
    for (const auto& opt : options) {
        std::string reason;
        if (!IsCustomMpvOptionSafe(opt.name, opt.value, &reason)) { result.rejectedOptions.push_back(opt.name); continue; }
        int ret = fn_mpv_set_property_string(mpv, opt.name.c_str(), opt.value.c_str());
        if (ret < 0) {
            result.failedOptions.push_back(opt.name);
            result.success = false;
            std::string errDetail = fn_mpv_error_string ? fn_mpv_error_string(ret) : std::to_string(ret);
            result.errorMessage = "Failed to apply option '" + opt.name + "=" + opt.value + "': " + errDetail;
        } else { result.appliedOptions.push_back(opt.name + "=" + opt.value); }
    }
    return result;
}

CustomMpvApplyResult MpvPlayer::LoadAndApplyCustomMpvConfig(const std::wstring& configPath) {
    std::wstring targetPath = configPath;
    if (targetPath.empty()) { targetPath = Settings::ResolveMpvConfigPath(customMpvConfigPath); }
    CustomMpvApplyResult result;
    if (!FileExists(targetPath)) { Settings::ResetMpvConfigToDefault(targetPath); }
    std::string content;
    StorageStatus readStatus = ReadTextFile(targetPath, content);
    if (!readStatus.is_ok()) {
        result.success = false;
        result.errorMessage = "Failed to read configuration file: " + Utf16ToUtf8(targetPath);
        lastCustomMpvResult = result;
        return result;
    }
    CustomMpvParseResult parseRes = ParseCustomMpvConfig(content);
    if (!parseRes.success) {
        result.success = false;
        result.errorMessage = parseRes.errors.empty() ? "Parse error in configuration" : parseRes.errors.front();
        result.disabled = true;
        result.recovered = true;
        lastCustomMpvResult = result;
        return result;
    }
    for (const auto& opt : parseRes.options) {
        std::string reason;
        if (!IsCustomMpvOptionSafe(opt.name, opt.value, &reason)) { result.rejectedOptions.push_back(opt.name); }
    }
    result = ApplyCustomMpvOptions(parseRes.options);
    if (!result.success) {
        // Recovery: disable and attempt to restore from last known good
        result.disabled = true;
        result.recovered = true;
        std::string lkgText;
        if (Settings::LoadMpvConfigLastKnownGood(targetPath, lkgText).is_ok() && !lkgText.empty()) {
            CustomMpvParseResult lkgParse = ParseCustomMpvConfig(lkgText);
            if (lkgParse.success && !lkgParse.options.empty()) { ApplyCustomMpvOptions(lkgParse.options); }
        }
    } else {
        // Save current valid configuration as last known good
        Settings::SaveMpvConfigLastKnownGood(targetPath, content);
    }
    lastCustomMpvResult = result;
    return result;
}

void MpvPlayer::RefreshAudioDevices() {
    std::vector<AudioDeviceInfo> newDevices;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (mpv && fn_mpv_get_property) {
            int64_t count = 0;
            if (fn_mpv_get_property(mpv, "audio-device-list/count", MPV_FORMAT_INT64, &count) >= 0 && count > 0) {
                for (int64_t i = 0; i < count; ++i) {
                    std::string prefix = "audio-device-list/" + std::to_string(i) + "/";
                    char* name = fn_mpv_get_property_string ? fn_mpv_get_property_string(mpv, (prefix + "name").c_str()) : nullptr;
                    char* desc = fn_mpv_get_property_string ? fn_mpv_get_property_string(mpv, (prefix + "description").c_str()) : nullptr;
                    if (name) {
                        AudioDeviceInfo info;
                        info.name = name;
                        info.description = desc ? Utf8ToUtf16(desc) : Utf8ToUtf16(name);
                        info.isDefault = (info.name == "auto");
                        newDevices.push_back(info);
                    }
                    if (fn_mpv_free) { if (name) fn_mpv_free(name); if (desc) fn_mpv_free(desc); }
                }
            }
        }
    }
    if (newDevices.empty()) {
        AudioDeviceInfo autoDev;
        autoDev.name = "auto";
        autoDev.description = L"Autoselect device (Default)";
        autoDev.isDefault = true;
        newDevices.push_back(autoDev);
    }
    { std::lock_guard<std::mutex> lock(stateMutex); audioDevices = newDevices; }
}

std::vector<AudioDeviceInfo> MpvPlayer::GetAudioDevices() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (audioDevices.empty()) {
        AudioDeviceInfo autoDev;
        autoDev.name = "auto";
        autoDev.description = L"Autoselect device (Default)";
        autoDev.isDefault = true;
        return { autoDev };
    }
    return audioDevices;
}

AudioManagementPlan MpvPlayer::BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
    bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap,
    const std::vector<AudioDeviceInfo>& availableDevices
) { return AudioConfig::BuildAudioPlan(device, layout, channelMix, normalizeDownmix, nightMode, balance, channelMap, availableDevices); }

AudioManagementPlan MpvPlayer::BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
    bool normalizeDownmix, const std::string& nightMode, const std::vector<AudioDeviceInfo>& availableDevices
) { return AudioConfig::BuildAudioPlan(device, layout, channelMix, normalizeDownmix, nightMode, 0.0, "auto", availableDevices); }

std::vector<AudioPropertyResult> MpvPlayer::SetAudioPreferences( const std::string& device, const std::string& layout,
    const std::string& channelMix, bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap ) {
    std::vector<AudioDeviceInfo> currentDevices = GetAudioDevices();
    AudioManagementPlan plan = BuildAudioPlan(device, layout, channelMix, normalizeDownmix, nightMode, balance, channelMap, currentDevices);
    std::vector<AudioPropertyResult> results;
    { std::lock_guard<std::mutex> lock(stateMutex); lastAudioPlan = plan; lastAudioResults.clear(); }
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_set_property_string) {
        for (const auto &act : plan.actions) { results.push_back({act.property, act.value, false, -1, "mpv not initialized"}); }
        std::lock_guard<std::mutex> lock(stateMutex);
        lastAudioResults = results;
        return results;
    }
    for (const auto& act : plan.actions) {
        int ret = fn_mpv_set_property_string(mpv, act.property.c_str(), act.value.c_str());
        AudioPropertyResult res;
        res.property = act.property;
        res.value = act.value;
        res.success = (ret >= 0);
        res.mpvErrorCode = ret;
        if (ret < 0 && fn_mpv_error_string) {
            const char* errStr = fn_mpv_error_string(ret);
            res.message = errStr ? errStr : "Unknown error";
        }
        results.push_back(res);
    }
    { std::lock_guard<std::mutex> lock(stateMutex); lastAudioResults = results; }
    return results;
}

std::vector<AudioPropertyResult> MpvPlayer::SetAudioBalance(double balance) {
    AudioManagementPlan plan;
    { std::lock_guard<std::mutex> lock(stateMutex); plan = lastAudioPlan; }
    return SetAudioPreferences( plan.requestedDevice.empty() ? "auto" : plan.requestedDevice,
        plan.requestedLayout.empty() ? "auto-safe" : plan.requestedLayout,
        plan.requestedChannelMix.empty() ? "auto" : plan.requestedChannelMix, plan.requestedNormalizeDownmix,
        plan.requestedNightMode.empty() ? "off" : plan.requestedNightMode, balance,
        plan.requestedChannelMap.empty() ? "auto" : plan.requestedChannelMap );
}

std::vector<AudioPropertyResult> MpvPlayer::SetAudioChannelMap(const std::string& channelMap) {
    AudioManagementPlan plan;
    { std::lock_guard<std::mutex> lock(stateMutex); plan = lastAudioPlan; }
    return SetAudioPreferences( plan.requestedDevice.empty() ? "auto" : plan.requestedDevice,
        plan.requestedLayout.empty() ? "auto-safe" : plan.requestedLayout,
        plan.requestedChannelMix.empty() ? "auto" : plan.requestedChannelMix, plan.requestedNormalizeDownmix,
        plan.requestedNightMode.empty() ? "off" : plan.requestedNightMode, plan.requestedBalance, channelMap );
}

AudioManagementPlan MpvPlayer::GetLastAudioPlan() const { std::lock_guard<std::mutex> lock(stateMutex); return lastAudioPlan; }

void MpvPlayer::SelectAudioTrack(int trackId) {
    std::string val = trackId <= 0 ? "no" : std::to_string(trackId);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "aid", val.c_str());
    }
    if (ret < 0) ReportError("Unable to select audio track", ret);
}

void MpvPlayer::SelectSubtitleTrack(int trackId) {
    std::string val = trackId <= 0 ? "no" : std::to_string(trackId);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "sid", val.c_str());
    }
    if (ret < 0) ReportError("Unable to select subtitle track", ret);
}

void MpvPlayer::SetSubtitleVisibility(bool visible) {
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "sub-visibility", visible ? "yes" : "no");
    }
    if (ret < 0) ReportError("Unable to set subtitle visibility", ret);
}

bool MpvPlayer::GetSubtitleVisibility() const {
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_get_property_string || !fn_mpv_free) return true;
    char* val = fn_mpv_get_property_string(mpv, "sub-visibility");
    if (!val) return true;
    bool vis = (std::strcmp(val, "yes") == 0 || std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0);
    fn_mpv_free(val);
    return vis;
}

void MpvPlayer::CycleAudioTrack() {
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv) return;
        if (fn_mpv_command_string) { ret = fn_mpv_command_string(mpv, "cycle audio"); } else if (fn_mpv_command) {
            const char* cmd[] = { "cycle", "audio", nullptr };
            ret = fn_mpv_command(mpv, cmd);
        }
    }
    if (ret < 0) { ReportError("Unable to cycle audio tracks", ret); return; }
    RefreshTracks();
}

void MpvPlayer::CycleSubtitleTrack() {
    int targetId = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!subTracks.empty()) {
            int selectedIndex = -1;
            for (size_t i = 0; i < subTracks.size(); ++i) {
                if (subTracks[i].isSelected) { selectedIndex = static_cast<int>(i); break; }
            }
            if (selectedIndex < 0) { targetId = subTracks[0].id; } else if (static_cast<size_t>(selectedIndex + 1) < subTracks.size()) {
                targetId = subTracks[selectedIndex + 1].id;
            } else { targetId = 0; }
        }
    }
    std::string val = targetId <= 0 ? "no" : std::to_string(targetId);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "sid", val.c_str());
    }
    if (ret < 0) { ReportError("Unable to select subtitle track", ret); return; }
    RefreshTracks();
}

std::wstring MpvPlayer::GetCurrentAudioTrackTitle() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    for (const auto& t : audioTracks) { if (t.isSelected) return t.title; }
    return audioTracks.empty() ? L"None" : audioTracks[0].title;
}

std::wstring MpvPlayer::GetCurrentSubtitleTrackTitle() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    for (const auto& t : subTracks) { if (t.isSelected) return t.title; }
    return L"Off";
}

int MpvPlayer::GetCurrentSubtitleTrackId() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    for (const auto& t : subTracks) { if (t.isSelected) return t.id; }
    return 0;
}

void MpvPlayer::SelectSecondarySubtitleTrack(int trackId) {
    std::string val = trackId <= 0 ? "no" : std::to_string(trackId);
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "secondary-sid", val.c_str());
    }
    if (ret < 0) {
        // Clear disabled fallback when unsupported or rejected
        {
            std::lock_guard<std::mutex> apiLock(mpvMutex);
            if (mpv && fn_mpv_set_property_string) { fn_mpv_set_property_string(mpv, "secondary-sid", "no"); }
        }
        ReportError("Unable to select secondary subtitle track", ret);
    }
    RefreshTracks();
}

int MpvPlayer::GetCurrentSecondarySubtitleTrackId() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    for (const auto& t : subTracks) { if (t.isSecondarySelected) return t.id; }
    return 0;
}

void MpvPlayer::SetSecondarySubtitleVisibility(bool visible) {
    int ret = -1;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (!mpv || !fn_mpv_set_property_string) return;
        ret = fn_mpv_set_property_string(mpv, "secondary-sub-visibility", visible ? "yes" : "no");
    }
    if (ret < 0) ReportError("Unable to set secondary subtitle visibility", ret);
}

bool MpvPlayer::GetSecondarySubtitleVisibility() const {
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_get_property_string || !fn_mpv_free) return true;
    char* val = fn_mpv_get_property_string(mpv, "secondary-sub-visibility");
    if (!val) return true;
    bool vis = (std::strcmp(val, "yes") == 0 || std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0);
    fn_mpv_free(val);
    return vis;
}

bool MpvPlayer::LoadExternalSubtitle(const std::wstring& subPath, bool selectAsSecondary) {
    if (subPath.empty()) { ReportErrorMessage(L"Unable to load external subtitle: path is empty"); return false; }
    if (!FileExists(subPath)) { ReportErrorMessage(L"Unable to load external subtitle: file not found: " + subPath); return false; }
    std::string utf8 = Utf16ToUtf8(subPath);
    int ret = -1;
    bool available = false;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (mpv && fn_mpv_command) {
            available = true;
            const char* flag = selectAsSecondary ? "auto" : "select";
            const char* cmd[] = { "sub-add", utf8.c_str(), flag, nullptr };
            ret = fn_mpv_command(mpv, cmd);
        }
    }
    if (!available) { ReportErrorMessage(L"Unable to load external subtitle: player is not initialized"); return false; }
    if (ret < 0) { ReportError("Unable to load external subtitle", ret); return false; }
    RefreshTracks();
    if (selectAsSecondary) {
        int targetId = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!subTracks.empty()) { targetId = subTracks.back().id; }
        }
        if (targetId > 0) { SelectSecondarySubtitleTrack(targetId); }
    }
    return true;
}
StorageStatus MpvPlayer::TakeScreenshot(const std::wstring& targetFilePath) {
    if (targetFilePath.empty())
        return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, targetFilePath, "Target screenshot path is empty");
    std::wstring curPath;
    { std::lock_guard<std::mutex> lock(stateMutex); curPath = currentFilePath; }
    if (curPath.empty())
        return StorageStatus::Error(StorageErrorCode::EmptyData, ERROR_FILE_NOT_FOUND, targetFilePath, "No media loaded for screenshot capture");
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (!mpv || (!fn_mpv_command && !fn_mpv_command_string))
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, ERROR_NOT_READY, targetFilePath, "Player is not initialized or mpv is unavailable");
    std::string utf8Path = Utf16ToUtf8(targetFilePath);
    int ret = -1;
    if (fn_mpv_command) {
        const char* cmd[] = { "screenshot-to-file", utf8Path.c_str(), "video", nullptr };
        ret = fn_mpv_command(mpv, cmd);
    } else if (fn_mpv_command_string) {
        std::string cmdStr = "screenshot-to-file \"" + utf8Path + "\" video";
        ret = fn_mpv_command_string(mpv, cmdStr.c_str());
    }
    if (ret < 0) {
        const char* errStr = fn_mpv_error_string ? fn_mpv_error_string(ret) : "Unknown mpv error";
        return StorageStatus::Error(StorageErrorCode::FileWriteFailed, (DWORD)std::abs(ret), targetFilePath, std::string("mpv screenshot capture failed: ") + errStr);
    }
    return StorageStatus::Ok(targetFilePath);
}

double MpvPlayer::GetCurrentTime() const { std::lock_guard<std::mutex> lock(stateMutex); return currentTime; }

double MpvPlayer::GetDuration() const { std::lock_guard<std::mutex> lock(stateMutex); return duration; }

bool MpvPlayer::IsPlaying() const { std::lock_guard<std::mutex> lock(stateMutex); return !isPaused; }

bool MpvPlayer::IsLoaded() const { std::lock_guard<std::mutex> lock(stateMutex); return !currentFilePath.empty(); }

std::wstring MpvPlayer::GetLastError() const { std::lock_guard<std::mutex> lock(stateMutex); return lastError; }

std::wstring MpvPlayer::GetCurrentFilePath() const { std::lock_guard<std::mutex> lock(stateMutex); return currentFilePath; }

std::vector<MpvTrackInfo> MpvPlayer::GetAudioTracks() const { std::lock_guard<std::mutex> lock(stateMutex); return audioTracks; }

std::vector<MpvTrackInfo> MpvPlayer::GetSubtitleTracks() const { std::lock_guard<std::mutex> lock(stateMutex); return subTracks; }

std::vector<MpvChapterInfo> MpvPlayer::GetChapters() const { std::lock_guard<std::mutex> lock(stateMutex); return chapters; }

int MpvPlayer::GetCurrentChapter() const { std::lock_guard<std::mutex> lock(stateMutex); return currentChapter; }

void MpvPlayer::NextChapter() {
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (!mpv) return;
    if (fn_mpv_command) {
        const char* cmd[] = { "add", "chapter", "1", nullptr };
        fn_mpv_command(mpv, cmd);
    } else if (fn_mpv_command_string) { fn_mpv_command_string(mpv, "add chapter 1"); }
}

void MpvPlayer::PreviousChapter() {
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (!mpv) return;
    if (fn_mpv_command) {
        const char* cmd[] = { "add", "chapter", "-1", nullptr };
        fn_mpv_command(mpv, cmd);
    } else if (fn_mpv_command_string) { fn_mpv_command_string(mpv, "add chapter -1"); }
}

void MpvPlayer::SetChapter(int chapterIndex) {
    if (chapterIndex < 0) return;
    std::lock_guard<std::mutex> lock(mpvMutex);
    if (!mpv) return;
    std::string idxStr = std::to_string(chapterIndex);
    if (fn_mpv_command) {
        const char* cmd[] = { "set", "chapter", idxStr.c_str(), nullptr };
        fn_mpv_command(mpv, cmd);
    } else if (fn_mpv_set_property) {
        int64_t idx = chapterIndex;
        fn_mpv_set_property(mpv, "chapter", MPV_FORMAT_INT64, &idx);
    } else if (fn_mpv_command_string) { std::string cmd = "set chapter " + idxStr; fn_mpv_command_string(mpv, cmd.c_str()); }
}

std::wstring MpvPlayer::GetVideoCodec() const { return GetCodec("video-codec", "video-format"); }

std::wstring MpvPlayer::GetAudioCodec() const { return GetCodec("audio-codec-name", "audio-format"); }

std::wstring MpvPlayer::GetCodec(const char* primary, const char* fallback) const {
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv || !fn_mpv_get_property_string) return L"";
    char* str = fn_mpv_get_property_string(mpv, primary);
    if (!str) str = fn_mpv_get_property_string(mpv, fallback);
    if (!str) return L"";
    std::wstring res = Utf8ToUtf16(str);
    if (fn_mpv_free) fn_mpv_free(str);
    // mpv's codec property can be a long description such as "H.265 / HEVC (High Efficiency Video Coding)". Keep the short form.
    size_t paren = res.find(L" (");
    if (paren != std::wstring::npos) res.resize(paren);
    for (auto& c : res) c = towupper(c);
    return res;
}

std::wstring MpvPlayer::GetResolutionString() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (videoWidth == 0 || videoHeight == 0) return L"";
    if (videoHeight >= 2160 || videoWidth >= 3840) return L"2160p";
    if (videoHeight >= 1440) return L"1440p";
    if (videoHeight >= 1080) return L"1080p";
    if (videoHeight >= 720) return L"720p";
    if (videoHeight >= 480) return L"480p";
    return std::to_wstring(videoWidth) + L"x" + std::to_wstring(videoHeight);
}

double MpvPlayer::GetCacheDuration() const {
    // demuxer-cache-duration is the only per-frame accessor that crosses into
    // the mpv core; the overlay needs ~1 Hz freshness, not frame rate.
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (now - lastCacheDurationQueryMs < 1000) return cachedCacheDurationSec;
    }
    double sec = 0.0;
    {
        std::lock_guard<std::mutex> apiLock(mpvMutex);
        if (mpv && fn_mpv_get_property) {
            double value = 0.0;
            if (fn_mpv_get_property(mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &value) >= 0)
                sec = std::isfinite(value) && value > 0.0 ? value : 0.0;
        }
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        lastCacheDurationQueryMs = now;
        cachedCacheDurationSec = sec;
    }
    return sec;
}

PlayerDiagnostics MpvPlayer::GetDiagnostics() const {
    PlayerDiagnostics d;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        d.filePath = currentFilePath;
        d.fileName = GetFileTitleFromPath(currentFilePath);
        d.currentTime = currentTime;
        d.duration = duration;
        d.speed = speed;
        d.volume = volume;
        d.isPaused = isPaused;
        d.isPlaying = !isPaused && !ended.load();
        d.isMuted = isMuted;
        d.isLoop = loop.load();
        d.isEnded = ended.load();
        d.videoWidth = videoWidth;
        d.videoHeight = videoHeight;
        d.audioTrackTitle = audioTracks.empty() ? L"None" : audioTracks[0].title;
        for (const auto& t : audioTracks) {
            if (t.isSelected) { d.audioTrackTitle = t.title; break; }
        }
        d.subTrackTitle = L"Off";
        for (const auto& t : subTracks) {
            if (t.isSelected) { d.subTrackTitle = t.title; break; }
        }
        d.activeSubSafeAreaX = lastSubSafeAreaX;
        d.activeSubSafeAreaY = lastSubSafeAreaY;
        d.activeSubSafeArea = std::to_wstring(lastSubSafeAreaX) + L"% H, " + std::to_wstring(lastSubSafeAreaY) + L"% V";
        d.activeAudioDevice = Utf8ToUtf16(lastAudioPlan.normalizedDevice);
        d.activeAudioLayout = Utf8ToUtf16(lastAudioPlan.normalizedLayout);
        d.activeAudioChannelMix = Utf8ToUtf16(lastAudioPlan.normalizedChannelMix);
        d.activeNightMode = Utf8ToUtf16(lastAudioPlan.normalizedNightMode);
        d.activeAudioFilters = Utf8ToUtf16(lastAudioPlan.filterString);
        d.activeAudioBalance = lastAudioPlan.normalizedBalance;
        d.activeAudioChannelMap = Utf8ToUtf16(lastAudioPlan.normalizedChannelMap);
        d.audioDeviceFallback = lastAudioPlan.deviceFellBackToAuto;
        for (const auto& dev : audioDevices) {
            if (dev.name == lastAudioPlan.normalizedDevice) { d.activeAudioDeviceDescription = dev.description; break; }
        }
        if (d.activeAudioDeviceDescription.empty())
            d.activeAudioDeviceDescription = d.activeAudioDevice.empty() ? L"auto" : d.activeAudioDevice;
    }
    d.resolutionStr = GetResolutionString();
    d.videoCodec = GetVideoCodec();
    d.audioCodec = GetAudioCodec();
    std::lock_guard<std::mutex> apiLock(mpvMutex);
    if (!mpv) return d;
    // Keep fallback reads ordered and retain the snapshot value when a property is unavailable.
    const auto readString = [&](std::wstring& value, const char* key, const char* fallback = nullptr, bool keepEmpty = true) {
        if (!fn_mpv_get_property_string) return;
        char* text = fn_mpv_get_property_string(mpv, key);
        if (!text && fallback) text = fn_mpv_get_property_string(mpv, fallback);
        if (!text) return;
        if (keepEmpty || *text) value = Utf8ToUtf16(text);
        if (fn_mpv_free) fn_mpv_free(text);
    };
    const auto readDouble = [&](double& value, std::initializer_list<const char*> keys, bool positive = false, double divisor = 1.0) {
        if (!fn_mpv_get_property) return;
        double result = 0.0;
        for (const char* key : keys) {
            if (fn_mpv_get_property(mpv, key, MPV_FORMAT_DOUBLE, &result) >= 0 && std::isfinite(result) && (!positive || result > 0.0)) {
                value = result / divisor;
                return;
            }
        }
    };
    const auto readInteger = [&](int64_t& value, std::initializer_list<const char*> keys) {
        if (!fn_mpv_get_property) return;
        int64_t result = 0;
        for (const char* key : keys) {
            if (fn_mpv_get_property(mpv, key, MPV_FORMAT_INT64, &result) >= 0) { value = result; return; }
        }
    };
    readString(d.hwDec, "hwdec-current", "hwdec-active");
    readString(d.pixelFormat, "video-params/pixelformat", "video-format");
    readDouble(d.currentTime, {"time-pos"});
    readDouble(d.videoFps, {"container-fps", "estimated-vf-fps", "fps"}, true);
    readDouble(d.displayFps, {"display-fps", "estimated-display-fps"}, true);
    readInteger(d.droppedFrames, {"drop-frame-count", "decoder-frame-drop-count"});
    readInteger(d.voDroppedFrames, {"frame-drop-count", "vo-drop-frame-count"});
    readInteger(d.mistimedFrames, {"mistimed-frame-count"});
    readDouble(d.videoBitrate, {"video-bitrate"}, true, 1000.0);
    readDouble(d.audioBitrate, {"audio-bitrate"}, true, 1000.0);
    readDouble(d.cacheDuration, {"demuxer-cache-duration"}, true);
    readInteger(d.cacheUsedBytes, {"demuxer-cache-state/total-bytes"});
    readDouble(d.avSync, {"avsync"});
    readInteger(d.audioSampleRate, {"audio-params/samplerate"});
    readInteger(d.audioChannels, {"audio-params/channel-count"});
    readString(d.colorPrimaries, "video-params/primaries");
    readString(d.colorTransfer, "video-params/gamma", "video-params/colortransfer");
    readString(d.colorSpace, "video-params/colorspace");
    readString(d.colorLevels, "video-params/colorlevels");
    readString(d.activeToneMapping, "tone-mapping");
    readString(d.activeGamutMapping, "gamut-mapping-mode");
    readString(d.activeIccProfile, "icc-profile");
    readString(d.activeAudioDevice, "audio-device", nullptr, false);
    readString(d.activeAudioLayout, "audio-channels", nullptr, false);
    readString(d.activeAudioFilters, "af");
    readDouble(d.sigPeak, {"video-params/sig-peak"});
    readDouble(d.maxCll, {"video-params/max-cll"});
    readDouble(d.maxFall, {"video-params/max-fall"});
    readDouble(d.minLuma, {"video-params/min-luma"});
    readDouble(d.maxLuma, {"video-params/max-luma"});
    std::wstring trLower = d.colorTransfer;
    for (auto& c : trLower) c = (wchar_t)towlower(c);
    std::wstring primLower = d.colorPrimaries;
    for (auto& c : primLower) c = (wchar_t)towlower(c);
    bool isHdr = (trLower == L"pq" || trLower == L"smpte2084" || trLower == L"hlg" || trLower == L"arib-std-b67" ||
                  d.sigPeak > 1.05 || d.maxCll > 100.0 || primLower == L"bt.2020");
    d.isHdrSource = isHdr;
    if (trLower == L"pq" || trLower == L"smpte2084") {
        d.hdrType = L"HDR10 (PQ)";
    } else if (trLower == L"hlg" || trLower == L"arib-std-b67") { d.hdrType = L"HLG"; } else if (isHdr) {
        d.hdrType = L"HDR (BT.2020)";
    } else { d.hdrType = L"SDR"; }
    if (!isHdr) { d.activeOutputMode = L"SDR Native"; } else {
        if (lastColorPlan.normalizedHdrMode == "passthrough") {
            d.activeOutputMode = L"HDR Passthrough";
        } else if (lastColorPlan.normalizedHdrMode == "off") { d.activeOutputMode = L"SDR Clipped"; } else {
            std::wstring tm = d.activeToneMapping.empty() ? Utf8ToUtf16(lastColorPlan.normalizedToneMapping) : d.activeToneMapping;
            if (tm.empty() || tm == L"auto") tm = L"bt.2390";
            d.activeOutputMode = L"SDR Tone-Mapped (" + tm + L")";
        }
    }
    return d;
}

void MpvPlayer::SetVideoBounds(int x, int y, int w, int h) {
    if (hVideoWnd) { SetWindowPos(hVideoWnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW); }
}

void MpvPlayer::SetVideoVisible(bool visible) {
    if (hVideoWnd) { ShowWindow(hVideoWnd, visible ? SW_SHOW : SW_HIDE); }
}

// =========================================================================
// MpvPreviewPlayer Implementation
// =========================================================================
// Video Thumbnail Disk Cache Implementation
// =========================================================================
namespace {
bool WriteThumbnailBuffer(const std::wstring& key, const PreviewThumbnailFrame& frame,
                          const CacheBudgetConfig& config, const std::wstring& root) {
    if (!config.IsEnabled(CacheClass::Thumbnails)) return false;
    std::string bytes;
    return ThumbnailCodec::Encode(frame.bgraPixels, frame.width, frame.height, frame.timestampSec, bytes) &&
        AtomicWriteCacheFile(CacheClass::Thumbnails, key, bytes, config, root).is_ok();
}

bool ReadThumbnailBuffer(const std::string& bytes, const std::wstring& path, PreviewThumbnailFrame& out) {
    ThumbnailDiskHeader header{};
    std::vector<uint8_t> pixels;
    if (!ThumbnailCodec::Decode(bytes, header, pixels)) return false;
    out.filePath = path; out.timestampSec = header.timestamp;
    out.width = header.width; out.height = header.height; out.bgraPixels = std::move(pixels);
    return true;
}
} // namespace

bool SaveThumbnailToDiskCache( const PreviewThumbnailFrame& frame, const CacheBudgetConfig& config, const std::wstring& root ) {
    if (frame.filePath.empty() || frame.bgraPixels.empty() || frame.width <= 0 || frame.height <= 0) return false;
    std::wstring key = MakeThumbnailCacheKey(frame.filePath, frame.timestampSec);
    return WriteThumbnailBuffer(key, frame, config, root);
}

bool SaveHubThumbnailToDiskCache( const PreviewThumbnailFrame& frame, const CacheBudgetConfig& config, const std::wstring& root ) {
    if (frame.filePath.empty() || frame.bgraPixels.empty() || frame.width <= 0 || frame.height <= 0) return false;
    std::wstring key = MakeHubThumbnailCacheKey(frame.filePath, frame.width, frame.height);
    return WriteThumbnailBuffer(key, frame, config, root);
}

bool LoadThumbnailFromDiskCache( const std::wstring& filePath, double quantizedTimeSec, PreviewThumbnailFrame& outFrame,
    const std::wstring& root, bool migrateLegacy ) {
    if (filePath.empty()) return false;
    std::wstring key = MakeThumbnailCacheKey(filePath, quantizedTimeSec);
    std::string rawData;
    StorageStatus status = ReadCacheFile(CacheClass::Thumbnails, key, rawData, root, migrateLegacy);
    if (!status.is_ok() || rawData.size() < sizeof(ThumbnailDiskHeader)) { return false; }
    if (!ReadThumbnailBuffer(rawData, filePath, outFrame)) return false;
    if (migrateLegacy && GetGlobalCacheBudgets().IsEnabled(CacheClass::Thumbnails))
        ThumbnailMigrations().Queue(root, key, rawData);
    return true;
}

bool LoadHubThumbnailFromDiskCache( const std::wstring& filePath, int width, int height, PreviewThumbnailFrame& outFrame,
    const std::wstring& root, bool migrateLegacy ) {
    if (filePath.empty() || width <= 0 || height <= 0) return false;
    std::wstring key = MakeHubThumbnailCacheKey(filePath, width, height);
    std::string rawData;
    StorageStatus status = ReadCacheFile(CacheClass::Thumbnails, key, rawData, root, migrateLegacy);
    if (!status.is_ok() || rawData.size() < sizeof(ThumbnailDiskHeader)) { return false; }
    if (!ReadThumbnailBuffer(rawData, filePath, outFrame)) return false;
    if (migrateLegacy && GetGlobalCacheBudgets().IsEnabled(CacheClass::Thumbnails))
        ThumbnailMigrations().Queue(root, key, rawData);
    return true;
}

size_t PurgeThumbnailDiskCacheForFile( const std::wstring& filePath, double durationSec, const std::wstring& root ) {
    if (filePath.empty()) return 0;
    std::lock_guard<std::recursive_mutex> ioLock(Detail::CacheIOMutex());
    InvalidateCacheAccounting(root);
    const std::wstring dir = GetCacheDir(CacheClass::Thumbnails, root) + L"\\";
    size_t removed = 0;
    auto removeKey = [&dir, &removed](const std::wstring& key) { if (!key.empty() && DeleteFileW((dir + key).c_str())) ++removed; };
    removeKey(MakeHubThumbnailCacheKey(filePath, 0, 0));
    if (durationSec > 0.0 && std::isfinite(durationSec) && durationSec <= static_cast<double>((std::numeric_limits<int>::max)() - 1)) {
        const int lastBucket = static_cast<int>(std::ceil(durationSec));
        for (int t = 0; t <= lastBucket; ++t) { removeKey(MakeThumbnailCacheKey(filePath, static_cast<double>(t))); }
        // Quantization clamps a rounded final bucket back to a fractional duration, so the exact-duration key is possible as well.
        removeKey(MakeThumbnailCacheKey(filePath, durationSec));
    }
    return removed;
}

size_t PruneOrphanedHubThumbnailDiskCache( const std::vector<std::wstring>& liveFilePaths, const std::wstring& root ) {
    std::lock_guard<std::recursive_mutex> ioLock(Detail::CacheIOMutex());
    InvalidateCacheAccounting(root);
    const std::wstring dir = GetCacheDir(CacheClass::Thumbnails, root);
    std::unordered_set<std::wstring> liveKeys;
    liveKeys.reserve(liveFilePaths.size());
    for (const auto& path : liveFilePaths) { if (!path.empty()) liveKeys.insert(MakeHubThumbnailCacheKey(path, 0, 0)); }
    size_t removed = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((dir + L"\\hub_*.bin").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && liveKeys.find(data.cFileName) == liveKeys.end() &&
            DeleteFileW((dir + L"\\" + data.cFileName).c_str())) {
            ++removed;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return removed;
}

// =========================================================================
// MpvPreviewPlayer Implementation
// =========================================================================

MpvPreviewPlayer::MpvPreviewPlayer() {}

void MpvPreviewPlayer::StopSaveWorker() {
    { std::lock_guard<std::mutex> lock(saveMutex); saveStopping = true; }
    saveCv.notify_all();
    if (saveWorker.joinable()) { saveWorker.join(); }
    std::lock_guard<std::mutex> lock(saveMutex);
    saveQueue.clear();
}

void MpvPreviewPlayer::QueueDiskSave(const PreviewThumbnailFrame& frame) {
    if (frame.filePath.empty() || frame.bgraPixels.empty() || frame.width <= 0 || frame.height <= 0) return;
    if (isPrivateMode.load()) return;
    std::lock_guard<std::mutex> lock(saveMutex);
    if (saveStopping) return;
    if (!saveWorker.joinable()) saveWorker = std::thread(&MpvPreviewPlayer::SaveWorkerThread, this);
    // Coalesce: re-seeking the same quantized second replaces the pending
    // copy instead of queueing duplicate file writes.
    for (auto& queued : saveQueue) {
        if (queued.filePath == frame.filePath && std::abs(queued.timestampSec - frame.timestampSec) < 0.001) { queued = frame; return; }
    }
    while (saveQueue.size() >= kMaxPendingDiskSaves) { saveQueue.pop_front(); }
    saveQueue.push_back(frame);
    saveCv.notify_one();
}

void MpvPreviewPlayer::SaveWorkerThread() {
    while (true) {
        PreviewThumbnailFrame frame;
        {
            std::unique_lock<std::mutex> lock(saveMutex);
            saveCv.wait(lock, [this]() { return saveStopping || !saveQueue.empty(); });
            if (saveStopping && saveQueue.empty()) break;
            if (saveQueue.empty()) continue;
            frame = std::move(saveQueue.front());
            saveQueue.pop_front();
        }
        if (!isPrivateMode.load()) SaveThumbnailToDiskCache(frame);
    }
}

MpvPreviewPlayer::~MpvPreviewPlayer() { Shutdown(); }

void MpvPreviewPlayer::OnRenderUpdate(void* ctx) {
    MpvPreviewPlayer* self = static_cast<MpvPreviewPlayer*>(ctx);
    if (self) { self->frameReady = true; }
}

bool MpvPreviewPlayer::Initialize(ID2D1DeviceContext* pD2DContext, HMODULE hDll) {
    Shutdown();
    if (!LoadMpvApi(MpvApiProfile::SoftwareRenderer, "Hover preview", hDll)) return false;
    // Seekbar captures must distinguish a completed seek from a redraw of
    // the preceding frame. These exports also drain the client's event queue.
    if (!fn_mpv_wait_event || !fn_mpv_get_property) {
        RecordInitializationFailure("seek completion API", MPV_ERROR_UNSUPPORTED);
        ResetMpvApi();
        return false;
    }
    mpv = fn_mpv_create();
    if (!mpv) { RecordInitializationFailure("mpv_create", MPV_ERROR_NOMEM); ResetMpvApi(); return false; }
    SetMpvOption(mpv, "vo", "libmpv", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "hwdec", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "audio", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "mute", "yes");
    // No fixed vf scale: the software render target size is chosen per mode (small for seekbar extraction, row-sized for card hover
    // previews) and mpv scales into that target directly.
    SetMpvOption(mpv, "sid", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "sub-auto", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "vd-lavc-threads", "2");
    SetMpvOption(mpv, "framedrop", "vo");
    SetMpvOption(mpv, "video-sync", "display-desync");
    // The preview renders into a bounded software target. It needs only a
    // small local read window; leaving mpv's larger default cache here
    // creates a second high-bitrate media cache on card/timeline hover.
    SetMpvOption(mpv, "demuxer-max-bytes", "8MiB");
    SetMpvOption(mpv, "demuxer-max-back-bytes", "0");
    // The preview is displayed at normal playback speed.  untimed=yes
    // makes libmpv decode as fast as the CPU can produce frames while the
    // UI consumes one frame per tick, allowing its queued frames to grow
    // into hundreds of MiB on a 1080p source.  A hover preview also has no
    // reason to loop or retain the demuxer at EOF.
    SetMpvOption(mpv, "loop-file", "no");
    SetMpvOption(mpv, "keep-open", "no");
    SetMpvOption(mpv, "untimed", "no");
    SetMpvOption(mpv, "hr-seek", "yes");
    SetMpvOption(mpv, "video-timing-offset", "0");
    SetMpvOption(mpv, "input-default-bindings", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-vo-keyboard", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-media-keys", "no", MpvOptionRequirement::Essential);
    // Direct URLs only: never invoke the Lua hook path.
    SetMpvOption(mpv, "load-scripts", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "scripts", "", MpvOptionRequirement::Essential);
    // Builds without Lua omit ytdl; retain the checked disable on builds that expose it.
    SetMpvOption(mpv, "ytdl", "no");
    if (!EssentialOptionsAccepted()) { Shutdown(); return false; }
    const int initializeResult = fn_mpv_initialize(mpv);
    if (initializeResult < 0) {
        RecordInitializationFailure("mpv_initialize", initializeResult);
        fn_mpv_terminate_destroy(mpv);
        mpv = nullptr;
        ResetMpvApi();
        return false;
    }
    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    const int renderResult = fn_mpv_render_context_create(&renderCtx, mpv, params);
    if (renderResult < 0) {
        RecordInitializationFailure("software render context", renderResult);
        fn_mpv_terminate_destroy(mpv);
        mpv = nullptr;
        ResetMpvApi();
        return false;
    }
    if (fn_mpv_render_context_set_update_callback) { fn_mpv_render_context_set_update_callback(renderCtx, OnRenderUpdate, this); }
    { std::lock_guard<std::mutex> lock(saveMutex); saveStopping = false; }
    BindDeviceContext(pD2DContext);
    return true;
}

void MpvPreviewPlayer::Shutdown() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    StopSaveWorker();
    isActive = false;    isSeekbarActive = false;
    currentPreviewPath.clear();
    frameReady = false;
    hasSeekbarThumbnail = false;
    isSeekbarLoading = false;
    hasPendingSeek = false;
    seekbarCapturePending = false;
    seekbarRestarted = false;
    seekbarRequestFailed = false;
    activeSeekbarTime = -1.0;
    if (mpv) {
        if (fn_mpv_command_string) { fn_mpv_command_string(mpv, "stop"); } else if (fn_mpv_command) {
            const char* cmd[] = { "stop", nullptr };
            fn_mpv_command(mpv, cmd);
        }
    }
    if (renderCtx) {
        if (fn_mpv_render_context_set_update_callback) { fn_mpv_render_context_set_update_callback(renderCtx, nullptr, nullptr); }
        if (fn_mpv_render_context_free) { fn_mpv_render_context_free(renderCtx); }
        renderCtx = nullptr;
    }
    if (mpv && fn_mpv_terminate_destroy) { fn_mpv_terminate_destroy(mpv); mpv = nullptr; }
    previewBitmap.Reset();
    seekbarBitmap.Reset();
    bitmapDevice.Reset();
    std::vector<uint8_t>().swap(pixelBuffer);
    memoryCache.Clear();
    ResetMpvApi();
}

void MpvPreviewPlayer::SetPreviewOutputSize(int width, int height) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    if (width < kMinPreviewBufferWidth) width = kMinPreviewBufferWidth;
    if (width > kMaxPreviewBufferWidth) width = kMaxPreviewBufferWidth;
    if (height < 180) height = 180;
    if (height > 1280) height = 1280;
    // Keep even dimensions; YUV software rendering prefers it.
    requestedPreviewWidth = width & ~1;
    requestedPreviewHeight = height & ~1;
}

// Must hold lifecycleMutex. Resizes the shared software render buffer and drops the D2D bitmaps so they are recreated at the new size by
// the next UpdateFrame call (which carries the device context).
void MpvPreviewPlayer::EnsureBufferDimensionsLocked(int width, int height, ID2D1DeviceContext* pD2DContext) {
    if (width <= 0 || height <= 0) return;
    if (bufferWidth == width && bufferHeight == height && !pixelBuffer.empty()) return;
    bufferWidth = width;
    bufferHeight = height;
    bufferIsPreviewSized = (width == requestedPreviewWidth && height == requestedPreviewHeight);
    try { pixelBuffer.assign(static_cast<size_t>(bufferWidth) * static_cast<size_t>(bufferHeight) * 4u, 0); } catch (...) {
        // Fall back to the small fixed size if a large allocation fails.
        bufferWidth = kSeekbarBufferWidth;
        bufferHeight = kSeekbarBufferHeight;
        bufferIsPreviewSized = false;
        pixelBuffer.assign(static_cast<size_t>(bufferWidth) * static_cast<size_t>(bufferHeight) * 4u, 0);
    }
    previewBitmap.Reset();
    seekbarBitmap.Reset();
    BindDeviceContextLocked(pD2DContext);
}

void MpvPreviewPlayer::BindDeviceContext(ID2D1DeviceContext* context) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    BindDeviceContextLocked(context);
}
void MpvPreviewPlayer::BindDeviceContextLocked(ID2D1DeviceContext* context) {
    if (!context) return;
    ComPtr<ID2D1Device> device;
    context->GetDevice(&device);
    if (device.Get() == bitmapDevice.Get()) return;
    // Keep the previous device alive until this comparison so pointer reuse
    // cannot conceal a device generation change.
    previewBitmap.Reset(); seekbarBitmap.Reset(); bitmapDevice = device;
    hasSeekbarThumbnail = false;
    frameReady = true;
}
bool MpvPreviewPlayer::UploadBitmapLocked(ComPtr<ID2D1Bitmap>& bitmap, ID2D1DeviceContext* context,
                                         UINT width, UINT height, const uint8_t* pixels) {
    if (!context || !pixels || !width || !height) return false;
    if (bitmap) {
        const auto size = bitmap->GetPixelSize();
        if (size.width != width || size.height != height) bitmap.Reset();
    }
    if (!bitmap && FAILED(context->CreateBitmap(D2D1::SizeU(width, height),
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &bitmap))) return false;
    if (FAILED(bitmap->CopyFromMemory(nullptr, pixels, width * 4))) { bitmap.Reset(); return false; }
    return true;
}

void MpvPreviewPlayer::StartPreview(const std::wstring& filePath, double startSec) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    if (!mpv || filePath.empty()) return;
    if (isActive && !isSeekbarActive && currentPreviewPath == filePath && bufferIsPreviewSized) return;
    if (!std::isfinite(startSec) || startSec < 0.0) startSec = 0.0;
    hasPendingSeek = false;
    seekbarCapturePending = false;
    seekbarRestarted = false;
    seekbarRequestFailed = false;
    DrainPreviewEventsLocked(false);
    EnsureBufferDimensionsLocked(requestedPreviewWidth, requestedPreviewHeight, nullptr);
    currentPreviewPath = filePath;
    isActive = true;
    isSeekbarActive = false;
    frameReady = false;
    std::string pathUtf8 = Utf16ToUtf8(filePath);
    std::string startOption;
    if (startSec > 0.0) {
        std::ostringstream start;
        start << "start=" << std::fixed << std::setprecision(3) << startSec;
        startOption = start.str();
    }
    int ret = -1;
    if (fn_mpv_command) {
        if (!startOption.empty()) {
            const char* loadCmd[] = { "loadfile", pathUtf8.c_str(), "replace", "-1", startOption.c_str(), nullptr };
            ret = fn_mpv_command(mpv, loadCmd);
        } else {
            const char* loadCmd[] = { "loadfile", pathUtf8.c_str(), "replace", nullptr };
            ret = fn_mpv_command(mpv, loadCmd);
        }
    }
    if (ret >= 0) {
        if (fn_mpv_set_property_string) {
            ret = fn_mpv_set_property_string(mpv, "pause", "no");
        } else if (fn_mpv_command_string) { ret = fn_mpv_command_string(mpv, "set pause no"); }
    }
    if (ret < 0) { isActive = false; currentPreviewPath.clear(); }
}

void MpvPreviewPlayer::StopMpvLocked() {
    if (!mpv) return;
    if (fn_mpv_command_string) { fn_mpv_command_string(mpv, "stop"); } else if (fn_mpv_command) {
        const char* cmd[] = { "stop", nullptr };
        fn_mpv_command(mpv, cmd);
    }
}

void MpvPreviewPlayer::StopPreview() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    isActive = false;
    currentPreviewPath.clear();
    frameReady = false;
    hasPendingSeek = false;
    seekbarCapturePending = false;
    seekbarRestarted = false;
    seekbarRequestFailed = false;
    StopMpvLocked();
}

void MpvPreviewPlayer::DrainPreviewEventsLocked(bool acceptRestart) {
    if (!mpv || !fn_mpv_wait_event) return;
    while (auto* event = fn_mpv_wait_event(mpv, 0)) {
        if (event->event_id == MPV_EVENT_NONE) break;
        if (!acceptRestart || !seekbarCapturePending) continue;
        if (event->event_id == MPV_EVENT_SEEK || event->event_id == MPV_EVENT_START_FILE)
            seekbarRestarted = false;
        else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART)
            seekbarRestarted = true;
    }
}

bool MpvPreviewPlayer::IssueSeekbarSeekLocked(double time, uint64_t sequence, uint64_t mediaToken) {
    DrainPreviewEventsLocked(false);
    lastSeekTime = std::chrono::steady_clock::now();
    inFlightSequenceId = sequence;
    inFlightMediaToken = mediaToken;
    inFlightTime = time;
    hasPendingSeek = false;
    seekbarCapturePending = true;
    seekbarRestarted = false;
    frameReady = false;
    // A paused exact seek still decodes and presents its target frame. Keep
    // extraction paused so it cannot play on or accumulate duplicate saves.
    if (fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "pause", "yes");
    std::ostringstream seekStr;
    seekStr << std::fixed << std::setprecision(3) << time;
    const std::string seekVal = seekStr.str();
    const char* seekCmd[] = { "seek", seekVal.c_str(), "absolute+exact", nullptr };
    int ret = fn_mpv_command ? fn_mpv_command(mpv, seekCmd) : MPV_ERROR_UNSUPPORTED;
    if (ret < 0 && fn_mpv_command) {
        const char* fallbackCmd[] = { "seek", seekVal.c_str(), "absolute", nullptr };
        ret = fn_mpv_command(mpv, fallbackCmd);
    }
    if (ret < 0) { seekbarCapturePending = false; isSeekbarLoading = false; }
    return ret >= 0;
}

bool MpvPreviewPlayer::RequestSeekbarThumbnail( const std::wstring& filePath, double rawTimeSec, double durationSec, uint64_t mediaToken,
    ID2D1DeviceContext* pD2DContext ) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    BindDeviceContextLocked(pD2DContext);
    if (filePath.empty() || durationSec <= 0.0) { ResetSeekbarPreviewLocked(); return false; }
    double quantized = QuantizePreviewTime(rawTimeSec, durationSec, 1.0);
    const bool sameRequest = isSeekbarActive && requestedFilePath == filePath && requestedMediaToken == mediaToken &&
        std::abs(requestedTime - quantized) < 0.001;
    if (sameRequest && seekbarRequestFailed) return false;
    if (sameRequest && (seekbarCapturePending || hasPendingSeek ||
        (hasSeekbarThumbnail && std::abs(activeSeekbarTime - quantized) < 0.001))) return true;
    seekbarRequestFailed = false;
    uint64_t seq = ++currentSequenceId;
    requestedFilePath = filePath;
    requestedTime = quantized;
    requestedMediaToken = mediaToken;
    isSeekbarActive = true;
    // 1. Check in-memory cache
    PreviewThumbnailFrame frame;
    if (memoryCache.TryGet(filePath, quantized, frame)) {
        hasPendingSeek = false;
        seekbarCapturePending = false;
        seekbarRestarted = false;
        frameReady = false;
        if (mpv && fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "pause", "yes");
        std::lock_guard<std::mutex> lock(renderMutex);
        if (pD2DContext && !frame.bgraPixels.empty()) {
            if (!UploadBitmapLocked(seekbarBitmap, pD2DContext, frame.width, frame.height, frame.bgraPixels.data())) return false;
        }
        hasSeekbarThumbnail = true;
        isSeekbarLoading = false;
        activeSeekbarTime = quantized;
        return true;
    }
    // 2. Check disk cache
    if (LoadThumbnailFromDiskCache(filePath, quantized, frame, GetExeDir(), !isPrivateMode.load())) {
        hasPendingSeek = false;
        seekbarCapturePending = false;
        seekbarRestarted = false;
        frameReady = false;
        if (mpv && fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "pause", "yes");
        memoryCache.Put(frame);
        std::lock_guard<std::mutex> lock(renderMutex);
        if (pD2DContext && !frame.bgraPixels.empty()) {
            if (!UploadBitmapLocked(seekbarBitmap, pD2DContext, frame.width, frame.height, frame.bgraPixels.data())) return false;
        }
        hasSeekbarThumbnail = true;
        isSeekbarLoading = false;
        activeSeekbarTime = quantized;
        return true;
    }
    // 3. Cache miss - queue extraction
    hasSeekbarThumbnail = false;
    isSeekbarLoading = true;
    if (!mpv) { return false; }
    // Extracted thumbnails feed fixed-size cache entries; always extract at
    // the small fixed size even if a card hover preview left the buffer large.
    EnsureBufferDimensionsLocked(kSeekbarBufferWidth, kSeekbarBufferHeight, pD2DContext);
    auto now = std::chrono::steady_clock::now();
    bool needNewFile = (!isActive || currentPreviewPath != filePath);
    if (needNewFile) {
        currentPreviewPath = filePath;
        isActive = true;
        frameReady = false;
        inFlightSequenceId = seq;
        inFlightMediaToken = mediaToken;
        inFlightTime = quantized;
        hasPendingSeek = false;
        seekbarCapturePending = true;
        seekbarRestarted = false;
        DrainPreviewEventsLocked(false);
        lastSeekTime = now;
        std::string pathUtf8 = Utf16ToUtf8(filePath);
        std::ostringstream start;
        start << "start=" << std::fixed << std::setprecision(3) << quantized;
        std::string startOption = start.str();
        if (fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "pause", "yes");
        if (fn_mpv_command) {
            const char* loadCmd[] = { "loadfile", pathUtf8.c_str(), "replace", "-1", startOption.c_str(), nullptr };
            if (fn_mpv_command(mpv, loadCmd) < 0) {
                seekbarCapturePending = false;
                isSeekbarLoading = false;
                isActive = false;
                return false;
            }
        }
        return true;
    }
    // Same file already loaded -> debounced seek
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekTime).count();
    if (elapsedMs < 40) {
        hasPendingSeek = true;
        pendingSeekTime = quantized;
        pendingSeekSeq = seq;
        pendingSeekMediaToken = mediaToken;
    } else {
        return IssueSeekbarSeekLocked(quantized, seq, mediaToken);
    }
    return true;
}

void MpvPreviewPlayer::ResetSeekbarPreviewLocked() {
    isSeekbarActive = false;
    hasSeekbarThumbnail = false;
    isSeekbarLoading = false;
    activeSeekbarTime = -1.0;
    hasPendingSeek = false;
    seekbarCapturePending = false;
    seekbarRestarted = false;
    seekbarRequestFailed = false;
    ++currentSequenceId;
    ++requestedMediaToken;
    requestedFilePath.clear();
    // A seekbar request uses the same libmpv handle as card previews. Once the
    // request is no longer visible, stop that handle as well; otherwise the
    // old request keeps decoding and UpdateFrame keeps converting frames even though no UI path can display them.
    if (isActive) { isActive = false; currentPreviewPath.clear(); frameReady = false; StopMpvLocked(); }
}

void MpvPreviewPlayer::ResetSeekbarPreview() { std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex); ResetSeekbarPreviewLocked(); }

bool MpvPreviewPlayer::UpdateFrame(ID2D1DeviceContext* pD2DContext) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    BindDeviceContextLocked(pD2DContext);
    if (!mpv || !renderCtx || !fn_mpv_render_context_render) return false;
    if (!isActive && !isSeekbarActive) return false;
    auto now = std::chrono::steady_clock::now();
    if (hasPendingSeek && isSeekbarActive) {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekTime).count();
        if (elapsedMs >= 40) {
            IssueSeekbarSeekLocked(pendingSeekTime, pendingSeekSeq, pendingSeekMediaToken);
        }
    }
    DrainPreviewEventsLocked(true);
    if (isSeekbarActive && !seekbarCapturePending) return false;
    // Sparse/VFR media or unavailable position properties must never leave
    // the renderer spinning forever while waiting for a matching target.
    if (isSeekbarActive && !hasPendingSeek && now - lastSeekTime >= std::chrono::seconds(5)) {
        seekbarCapturePending = false;
        seekbarRestarted = false;
        seekbarRequestFailed = true;
        isSeekbarLoading = false;
        frameReady = false;
        return false;
    }
    uint64_t flags = 0;
    if (fn_mpv_render_context_update) { flags = fn_mpv_render_context_update(renderCtx); }
    // Rendering while a seek is starting lets libmpv complete its VO restart,
    // but that frame must not be cached yet. Once restarted, render again even
    // if paused playback produces no further callback.
    if ((flags & MPV_RENDER_UPDATE_FRAME) || frameReady || (seekbarCapturePending && seekbarRestarted)) {
        frameReady = false;
        std::lock_guard<std::mutex> lock(renderMutex);
        int size[2] = { bufferWidth, bufferHeight };
        char format[] = "bgr0";
        size_t stride = bufferWidth * 4;
        mpv_render_param drawParams[] = {
            { MPV_RENDER_PARAM_SW_SIZE, size }, { MPV_RENDER_PARAM_SW_FORMAT, format }, { MPV_RENDER_PARAM_SW_STRIDE, &stride },
            { MPV_RENDER_PARAM_SW_POINTER, pixelBuffer.data() }, { MPV_RENDER_PARAM_INVALID, nullptr }
        };
        if (fn_mpv_render_context_render(renderCtx, drawParams) >= 0) {
            bool didUpdate = false;
            // 1. Update Media Card Live Preview Bitmap if active
            if (isActive && !isSeekbarActive) {
                didUpdate = UploadBitmapLocked(previewBitmap, pD2DContext, bufferWidth, bufferHeight, pixelBuffer.data());
            }
            // 2. Update Seekbar Thumbnail & Caches if seekbar is active
            if (isSeekbarActive && seekbarCapturePending && seekbarRestarted && !hasPendingSeek &&
                inFlightSequenceId == currentSequenceId && inFlightMediaToken == requestedMediaToken &&
                currentPreviewPath == requestedFilePath && !currentPreviewPath.empty()) {
                int seeking = 1;
                double position = -1.0, fps = 0.0;
                if (!fn_mpv_get_property ||
                    fn_mpv_get_property(mpv, "seeking", MPV_FORMAT_FLAG, &seeking) < 0 || seeking ||
                    fn_mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &position) < 0) return false;
                fn_mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps);
                const double tolerance = std::isfinite(fps) && fps > 0 ? std::max(0.001, 1.0 / fps) + 0.001 : 0.25;
                if (!std::isfinite(position) || std::abs(position - inFlightTime) > tolerance) return false;
                PreviewThumbnailFrame frame;
                frame.filePath = currentPreviewPath;
                frame.timestampSec = inFlightTime;
                frame.width = bufferWidth;
                frame.height = bufferHeight;
                frame.bgraPixels = pixelBuffer;
                memoryCache.Put(frame);
                // Persist off the render thread: QueueDiskSave copies into a bounded background queue (skipped in private mode), so
                // directory budget scans and file I/O never hitch scrubbing.
                QueueDiskSave(frame);
                seekbarCapturePending = false;
                seekbarRestarted = false;
                isSeekbarLoading = false;
                if (fn_mpv_set_property_string) fn_mpv_set_property_string(mpv, "pause", "yes");
                if (UploadBitmapLocked(seekbarBitmap, pD2DContext, bufferWidth, bufferHeight, pixelBuffer.data())) {
                    hasSeekbarThumbnail = true;
                    activeSeekbarTime = inFlightTime;
                    didUpdate = true;
                }
            }
            return didUpdate;
        }
    }
    return false;
}

std::wstring MpvPreviewPlayer::GetCurrentPreviewPath() const {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    return currentPreviewPath;
}

// =========================================================================
// MpvHubExtractor Implementation
// =========================================================================

MpvHubExtractor::MpvHubExtractor() = default;

MpvHubExtractor::~MpvHubExtractor() { Shutdown(); }

void MpvHubExtractor::OnRenderUpdate(void* ctx) {
    MpvHubExtractor* self = static_cast<MpvHubExtractor*>(ctx);
    if (self) { self->frameReady.store(true, std::memory_order_release); }
}

void MpvHubExtractor::StopMpvLocked() {
    if (!mpv) return;
    if (fn_mpv_command_string) { fn_mpv_command_string(mpv, "stop"); } else if (fn_mpv_command) {
        const char* cmd[] = { "stop", nullptr };
        fn_mpv_command(mpv, cmd);
    }
}

bool MpvHubExtractor::EnsureInitialized() {
    if (mpv && renderCtx) return true;
    // Caller (Begin) holds mutex; use the lock-free teardown on failure.
    TeardownLocked();
    if (!LoadMpvApi(MpvApiProfile::SoftwareRenderer, "Hub extractor")) return false;
    mpv = fn_mpv_create();
    if (!mpv) { RecordInitializationFailure("mpv_create", MPV_ERROR_NOMEM); TeardownLocked(); return false; }
    // Single-frame grab tunables: keyframe seeks, skipped loop filtering, and a tiny demuxer window. Exactness is irrelevant
    // for a representative card frame; speed is everything.
    SetMpvOption(mpv, "vo", "libmpv", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "hwdec", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "audio", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "sid", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "sub-auto", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "vd-lavc-threads", "2");
    SetMpvOption(mpv, "vd-lavc-skiploopfilter", "all");
    SetMpvOption(mpv, "vd-lavc-fast", "yes");
    SetMpvOption(mpv, "hr-seek", "no");
    SetMpvOption(mpv, "demuxer-max-bytes", "4MiB");
    SetMpvOption(mpv, "demuxer-max-back-bytes", "0");
    SetMpvOption(mpv, "loop-file", "no");
    SetMpvOption(mpv, "keep-open", "no");
    SetMpvOption(mpv, "untimed", "no");
    SetMpvOption(mpv, "video-sync", "display-desync");
    SetMpvOption(mpv, "input-default-bindings", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-vo-keyboard", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "input-media-keys", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "load-scripts", "no", MpvOptionRequirement::Essential);
    SetMpvOption(mpv, "scripts", "", MpvOptionRequirement::Essential);
    // Builds without Lua omit ytdl; retain the checked disable on builds that expose it.
    SetMpvOption(mpv, "ytdl", "no");
    if (!EssentialOptionsAccepted()) { TeardownLocked(); return false; }
    const int initializeResult = fn_mpv_initialize(mpv);
    if (initializeResult < 0) { RecordInitializationFailure("mpv_initialize", initializeResult); TeardownLocked(); return false; }
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW }, { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    const int renderResult = fn_mpv_render_context_create(&renderCtx, mpv, params);
    if (renderResult < 0) { RecordInitializationFailure("software render context", renderResult); TeardownLocked(); return false; }
    if (fn_mpv_render_context_set_update_callback) { fn_mpv_render_context_set_update_callback(renderCtx, OnRenderUpdate, this); }
    pixelBuffer.resize((size_t)kExtractWidth * kExtractHeight * 4u);
    StartSaveWorker();
    return true;
}

void MpvHubExtractor::Shutdown() { std::lock_guard<std::mutex> lock(mutex); StopSaveWorker(); TeardownLocked(); }

bool MpvHubExtractor::IsInitialized() const { std::lock_guard<std::mutex> lock(mutex); return mpv != nullptr; }

void MpvHubExtractor::TeardownLocked() {
    active = false;
    resultReady = false;
    extractPath.clear();
    resultFrame = PreviewThumbnailFrame();
    frameReady.store(false, std::memory_order_release);
    if (renderCtx) {
        if (fn_mpv_render_context_set_update_callback) { fn_mpv_render_context_set_update_callback(renderCtx, nullptr, nullptr); }
        if (fn_mpv_render_context_free) { fn_mpv_render_context_free(renderCtx); }
        renderCtx = nullptr;
    }
    if (mpv && fn_mpv_terminate_destroy) { fn_mpv_terminate_destroy(mpv); mpv = nullptr; }
    ResetMpvApi();
    std::vector<uint8_t>().swap(pixelBuffer);
}

bool MpvHubExtractor::Begin(const std::wstring& filePath, double startSec) {
    std::lock_guard<std::mutex> lock(mutex);
    if (filePath.empty()) return false;
    if (active) return false;
    if (!EnsureInitialized()) return false;
    if (!std::isfinite(startSec) || startSec < 0.0) startSec = 0.0;
    extractPath = filePath;
    extractStartSec = startSec;
    extractStartTickMs = (long long)GetTickCount64();
    active = true;
    resultReady = false;
    frameReady.store(false, std::memory_order_release);
    std::string pathUtf8 = Utf16ToUtf8(filePath);
    int ret = -1;
    if (fn_mpv_command) {
        if (startSec > 0.0) {
            std::ostringstream start;
            start << "start=" << std::fixed << std::setprecision(3) << startSec;
            std::string startOption = start.str();
            const char* loadCmd[] = { "loadfile", pathUtf8.c_str(), "replace", "-1", startOption.c_str(), nullptr };
            ret = fn_mpv_command(mpv, loadCmd);
        } else {
            const char* loadCmd[] = { "loadfile", pathUtf8.c_str(), "replace", nullptr };
            ret = fn_mpv_command(mpv, loadCmd);
        }
    }
    if (ret >= 0 && fn_mpv_set_property_string) { ret = fn_mpv_set_property_string(mpv, "pause", "no"); }
    if (ret < 0) { active = false; extractPath.clear(); StopMpvLocked(); return false; }
    return true;
}

void MpvHubExtractor::Cancel() {
    std::lock_guard<std::mutex> lock(mutex);
    active = false;
    resultReady = false;
    extractPath.clear();
    resultFrame = PreviewThumbnailFrame();
    StopMpvLocked();
}

bool MpvHubExtractor::IsActive() const { std::lock_guard<std::mutex> lock(mutex); return active; }

const std::wstring& MpvHubExtractor::GetPath() const { std::lock_guard<std::mutex> lock(mutex); return extractPath; }

bool MpvHubExtractor::TakeResult(PreviewThumbnailFrame& outFrame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!resultReady) return false;
    outFrame = std::move(resultFrame);
    resultFrame = PreviewThumbnailFrame();
    resultReady = false;
    return true;
}

bool MpvHubExtractor::Pump() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!active || !mpv || !renderCtx || !fn_mpv_render_context_render) return false;
    if (((long long)GetTickCount64() - extractStartTickMs) > kExtractTimeoutMs) {
        active = false;
        extractPath.clear();
        StopMpvLocked();
        return false;
    }
    uint64_t flags = 0;
    if (fn_mpv_render_context_update) { flags = fn_mpv_render_context_update(renderCtx); }
    if (!(flags & MPV_RENDER_UPDATE_FRAME) && !frameReady.load(std::memory_order_acquire)) { return false; }
    frameReady.store(false, std::memory_order_release);
    int size[2] = { kExtractWidth, kExtractHeight };
    char format[] = "bgr0";
    size_t stride = (size_t)kExtractWidth * 4u;
    mpv_render_param drawParams[] = {
        { MPV_RENDER_PARAM_SW_SIZE, size }, { MPV_RENDER_PARAM_SW_FORMAT, format }, { MPV_RENDER_PARAM_SW_STRIDE, &stride },
        { MPV_RENDER_PARAM_SW_POINTER, pixelBuffer.data() }, { MPV_RENDER_PARAM_INVALID, nullptr }
    };
    if (fn_mpv_render_context_render(renderCtx, drawParams) < 0) { return false; }
    PreviewThumbnailFrame frame;
    frame.filePath = extractPath;
    frame.timestampSec = extractStartSec;
    frame.width = kExtractWidth;
    frame.height = kExtractHeight;
    frame.bgraPixels = pixelBuffer;
    QueueSave(std::move(frame), MakeHubThumbnailCacheKey(extractPath, kExtractWidth, kExtractHeight));
    resultFrame.filePath = extractPath;
    resultFrame.timestampSec = extractStartSec;
    resultFrame.width = kExtractWidth;
    resultFrame.height = kExtractHeight;
    resultFrame.bgraPixels = pixelBuffer;
    resultReady = true;
    active = false;
    extractPath.clear();
    StopMpvLocked();
    return true;
}

void MpvHubExtractor::StartSaveWorker() {
    std::lock_guard<std::mutex> lock(saveMutex);
    saveStopping = false;
    if (!saveWorker.joinable()) { saveWorker = std::thread(&MpvHubExtractor::SaveWorkerThread, this); }
}

void MpvHubExtractor::StopSaveWorker() {
    { std::lock_guard<std::mutex> lock(saveMutex); saveStopping = true; }
    saveCv.notify_all();
    if (saveWorker.joinable()) { saveWorker.join(); }
    std::lock_guard<std::mutex> lock(saveMutex);
    saveQueue.clear();
}

void MpvHubExtractor::QueueSave(PreviewThumbnailFrame frame, std::wstring cacheKey) {
    if (frame.filePath.empty() || frame.bgraPixels.empty() || cacheKey.empty()) return;
    if (isPrivateMode.load()) return;
    std::lock_guard<std::mutex> lock(saveMutex);
    if (saveStopping) return;
    for (auto& queued : saveQueue) {
        if (queued.cacheKey == cacheKey) { queued.frame = std::move(frame); return; }
    }
    while (saveQueue.size() >= kMaxPendingSaves) { saveQueue.pop_front(); }
    PendingSave pending;
    pending.frame = std::move(frame);
    pending.cacheKey = std::move(cacheKey);
    saveQueue.push_back(std::move(pending));
    saveCv.notify_one();
}

void MpvHubExtractor::SaveWorkerThread() {
    while (true) {
        PendingSave pending;
        {
            std::unique_lock<std::mutex> lock(saveMutex);
            saveCv.wait(lock, [this]() { return saveStopping || !saveQueue.empty(); });
            if (saveStopping && saveQueue.empty()) break;
            if (saveQueue.empty()) continue;
            pending = std::move(saveQueue.front());
            saveQueue.pop_front();
        }
        if (!isPrivateMode.load()) WriteThumbnailBuffer(pending.cacheKey, pending.frame, GetGlobalCacheBudgets(), GetExeDir());
    }
}

} // namespace Aperture
