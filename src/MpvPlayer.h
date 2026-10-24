#pragma once

#include "Common.h"
#include "AudioConfig.h"
#include "ThumbnailCodec.h"
#include "MpvApi.h"
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>

namespace Aperture {

// Maximum playback volume as a ratio of the mpv base level (1.0 == 100%).
inline constexpr double kMaxVolumeRatio = 1.3;

// Custom aspect-ratio strings accepted beyond the fixed modes ("auto", "16:9",
// "4:3", "21:9", "fill", "cover"): "W:H" with decimal W,H > 0, or a single
// decimal ratio > 0 (e.g. "2.35:1", "1.85").
inline bool IsValidCustomAspectString(const std::string& value) {
    if (value.empty()) return false;
    auto parsePositive = [](const std::string& token) {
        if (token.empty() || token.find_first_not_of("0123456789.") != std::string::npos) return 0.0;
        size_t consumed = 0;
        double parsed = 0.0;
        try { parsed = std::stod(token, &consumed); } catch (...) { return 0.0; }
        if (consumed != token.size() || !std::isfinite(parsed) || parsed <= 0.0) return 0.0;
        return parsed;
    };
    const size_t colon = value.find(':');
    if (colon == std::string::npos) return parsePositive(value) > 0.0;
    return parsePositive(value.substr(0, colon)) > 0.0 && parsePositive(value.substr(colon + 1)) > 0.0;
}

struct MpvTrackInfo {
    int id = 0;
    std::string type; // "audio", "sub", "video"
    std::wstring title, lang, codec;
    bool isSelected = false, isSecondarySelected = false, isDefault = false, isExternal = false;
    bool isForced = false;
    int channels = 0;
};

struct MpvChapterInfo { int index = 0; std::wstring title; double time = 0.0; };

struct ColorPropertyAction { std::string property; std::string value; };

struct ColorPropertyResult {
    std::string property, value;
    bool success = false;
    int mpvErrorCode = 0;
    std::string message;
};

struct ColorManagementPlan {
    std::string requestedHdrMode, requestedToneMapping;
    int requestedTargetPeak = 0;
    std::string requestedGamutMapping, requestedIccProfile, normalizedHdrMode, normalizedToneMapping;
    int normalizedTargetPeak = 0;
    std::string normalizedGamutMapping, normalizedIccProfile;
    std::wstring resolvedIccPath;
    std::vector<ColorPropertyAction> actions;
};

struct SubtitleSafeAreaPlan {
    int requestedSafeAreaX = 0, requestedSafeAreaY = 0, normalizedSafeAreaX = 0, normalizedSafeAreaY = 0;
    int marginX = 0, marginY = 0;
    std::vector<std::pair<std::string, std::string>> actions;
};
struct CustomMpvOption { std::string name; std::string value; int lineNumber = 0; };

struct CustomMpvParseResult {
    bool success = true;
    std::vector<CustomMpvOption> options;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    int totalLines = 0;
};

struct CustomMpvApplyResult {
    bool success = true, recovered = false, disabled = false;
    std::vector<std::string> appliedOptions;
    std::vector<std::string> rejectedOptions;
    std::vector<std::string> failedOptions;
    std::string errorMessage;
};
struct PlayerDiagnostics {
    std::wstring filePath, fileName;
    double currentTime = 0.0, duration = 0.0, speed = 1.0, volume = 1.0;
    bool isPlaying = false, isPaused = true, isMuted = false, isLoop = false;
    bool isEnded = false;
    uint32_t videoWidth = 0, videoHeight = 0;
    std::wstring resolutionStr, pixelFormat, videoCodec, audioCodec;
    std::wstring hwDec;
    double videoFps = 0.0, displayFps = 0.0;
    // Frame counters are nonnegative; -1 means mpv could not provide a value.
    int64_t droppedFrames = -1, voDroppedFrames = -1, mistimedFrames = -1;
    double videoBitrate = 0.0; // kbps
    double audioBitrate = 0.0; // kbps
    double cacheDuration = 0.0;
    int64_t cacheUsedBytes = 0;
    double avSync = 0.0;
    int64_t audioSampleRate = 0, audioChannels = 0;
    std::wstring audioTrackTitle, subTrackTitle;
    int activeSubSafeAreaX = 0, activeSubSafeAreaY = 0;
    // --- Audio & Surround Diagnostics ---
    std::wstring activeAudioDevice, activeAudioDeviceDescription, activeAudioLayout, activeAudioChannelMix;
    std::wstring activeNightMode;
    double activeAudioBalance = 0.0;
    std::wstring activeAudioChannelMap;
    bool audioDeviceFallback = false;
    std::wstring colorPrimaries, colorTransfer;
    double sigPeak = 0.0, maxCll = 0.0, maxFall = 0.0;
    bool isHdrSource = false;
    std::wstring hdrType = L"SDR";
    std::wstring activeOutputMode = L"SDR Native";
    std::wstring activeToneMapping, activeIccProfile;
};

class MpvPlayer : private MpvApiOwner {
public:
    using MpvApiOwner::GetInitializationDiagnostics;
    using MpvApiOwner::SetInitializationDiagnosticSink;
    MpvPlayer();
    ~MpvPlayer();
    bool Initialize(HWND hParentWnd, HWND hVideoChildWnd);
    void Shutdown();
    bool Open(const std::wstring& filePath, double startSec = 0.0);
    void Close();
    void Play();
    void Pause();
    void TogglePlay();
    void Stop();
    void Seek(double timeSec, bool exact = true);
    void SeekRelative(double deltaSec);
    void SetABLoopA(double timeSec);
    void SetABLoopB(double timeSec);
    void ClearABLoop();
    void StepFrame(int frameCount);
    void SetVolume(double volume0to1);
    double GetVolume() const;
    void SetMuted(bool mute);
    bool IsMuted() const;
    void SetPlaybackRate(double rate);
    double GetPlaybackRate() const;
    void SetLoop(bool loop);
    bool GetLoop() const;
    void SetAspectRatio(const std::string& mode);
    // Returns true when the requested end state was applied ("off" always
    // succeeds); false when a requested deinterlace filter could not be added
    // (e.g. a lean libmpv build without bwdif/yadif).
    bool SetDeinterlace(const std::string& mode);
    void SetAudioDelay(double delaySec);
    void SetSubDelay(double delaySec);
    void SetVideoRotate(int degrees);
    int GetVideoRotate() const;
    void SetBrightness(int brightness);
    int GetBrightness() const;
    void SetContrast(int contrast);
    int GetContrast() const;
    void SetSaturation(int saturation);
    int GetSaturation() const;
    void SetGamma(int gamma);
    int GetGamma() const;
    void SetHue(int hue);
    int GetHue() const;
    void ResetVideoAdjustments();
    // Zoom & pan are session state (mpv runtime properties); they are never
    // persisted and reset on every file open. video-zoom is logarithmic:
    // the on-screen multiplier is exp(zoom).
    void AdjustVideoZoom(double delta);
    void NudgeVideoPan(double dx, double dy);
    void ResetVideoZoomPan();
    double GetVideoZoom() const;
    // Runtime playback preferences. These are safe to call before a file is
    // loaded; they take effect immediately when libmpv accepts the property.
    void SetHardwareAcceleration(bool enabled);
    void SetPreferredAudioLanguage(const std::string& language);
    void SetPreferredSubtitleLanguage(const std::string& language);
    void SetSubtitleStyle(const std::string& fontSize, const std::string& colorHex, const std::string& backgroundStyle,
                          const std::string& position, int safeAreaX = 0, int safeAreaY = 0,
                          const std::string& fontFamily = "Inter", const std::string& codepage = "auto", bool forcedOnly = false,
                          const std::string& secondaryFontSize = "", const std::string& secondaryColorHex = "",
                          const std::string& secondaryPosition = "");
    static SubtitleSafeAreaPlan BuildSubtitleSafeAreaPlan(int safeAreaX, int safeAreaY);
    static ColorManagementPlan BuildColorManagementPlan(const std::string& hdrMode, const std::string& toneMapping, int targetPeak,
                                                        const std::string& gamutMapping, const std::string& iccProfilePath,
                                                        const std::wstring& exeDir = GetExeDir());
    std::vector<ColorPropertyResult> SetColorPreferences(const std::string& hdrMode, const std::string& toneMapping, int targetPeak,
                                                         const std::string& gamutMapping, const std::string& iccProfilePath);
    ColorManagementPlan GetLastColorPlan() const;
    // Custom MPV configuration loading, validation & runtime recovery
    static bool IsCustomMpvOptionSafe(const std::string& name, const std::string& value, std::string* outReason = nullptr);
    static CustomMpvParseResult ParseCustomMpvConfig(const std::string& text);
    CustomMpvApplyResult ApplyCustomMpvOptions(const std::vector<CustomMpvOption>& options);
    CustomMpvApplyResult LoadAndApplyCustomMpvConfig(const std::wstring& configPath = L"");
    void SetCustomMpvConfigEnabled(bool enabled) { customMpvConfigEnabled = enabled; }
    void SetCustomMpvConfigPath(const std::string& path) { customMpvConfigPath = path; }
    // Audio device enumeration, surround layouts, and dynamic range compression
    std::vector<AudioDeviceInfo> GetAudioDevices() const;
    void RefreshAudioDevices();
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap,
        const std::vector<AudioDeviceInfo>& availableDevices = {}, const std::string& replayGainMode = "off",
        double replayGainPreampDb = 0.0, bool audioPassthrough = false, bool pitchCorrection = true );
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, const std::vector<AudioDeviceInfo>& availableDevices = {} );
    std::vector<AudioPropertyResult> SetAudioPreferences( const std::string& device, const std::string& layout,
        const std::string& channelMix, bool normalizeDownmix, const std::string& nightMode, double balance = 0.0,
        const std::string& channelMap = "auto", const std::string& replayGainMode = "off",
        double replayGainPreampDb = 0.0, bool audioPassthrough = false, bool pitchCorrection = true );
    std::vector<AudioPropertyResult> SetAudioBalance(double balance);
    std::vector<AudioPropertyResult> SetAudioChannelMap(const std::string& channelMap);
    void SelectAudioTrack(int trackId);
    void SelectSubtitleTrack(int trackId);
    void SelectSecondarySubtitleTrack(int trackId);
    void CycleAudioTrack();
    void CycleSubtitleTrack();
    std::wstring GetCurrentAudioTrackTitle() const;
    std::wstring GetCurrentSubtitleTrackTitle() const;
    int GetCurrentSubtitleTrackId() const;
    int GetCurrentSecondarySubtitleTrackId() const;
    void SetSubtitleVisibility(bool visible);
    void SetSecondarySubtitleVisibility(bool visible);
    bool GetSubtitleVisibility() const;
    bool GetSecondarySubtitleVisibility() const;
    bool LoadExternalSubtitle(const std::wstring& subPath, bool selectAsSecondary = false);
    StorageStatus TakeScreenshot(const std::wstring& targetFilePath);
    double GetCurrentTime() const;
    double GetDuration() const;
    bool IsPlaying() const;
    bool IsLoaded() const;
    std::wstring GetLastError() const;
    std::wstring GetCurrentFilePath() const;
    std::vector<MpvTrackInfo> GetAudioTracks() const;
    std::vector<MpvTrackInfo> GetSubtitleTracks() const;
    std::vector<MpvChapterInfo> GetChapters() const;
    // Lock-free change counter for the chapter list; lets frame-rate callers
    // cache the vector without touching stateMutex every frame.
    uint64_t GetChaptersVersion() const { return chaptersVersion.load(std::memory_order_acquire); }
    // Same pattern for the audio/subtitle track lists (bumped only when a
    // refresh observes an actual change).
    uint64_t GetTracksVersion() const { return tracksVersion.load(std::memory_order_acquire); }
    int GetCurrentChapter() const;
    void NextChapter();
    void PreviousChapter();
    void SetChapter(int chapterIndex);
    std::wstring GetVideoCodec() const;
    std::wstring GetAudioCodec() const;
    std::wstring GetResolutionString() const;
    double GetCacheDuration() const;
    PlayerDiagnostics GetDiagnostics() const;
    // Callback registration is synchronized with the libmpv event thread.
    void SetOnEnded(std::function<void()> callback);
    void SetOnTimeUpdated(std::function<void(double)> callback);
    void SetOnDurationChanged(std::function<void(double)> callback);
    void SetOnPlayStateChanged(std::function<void(bool)> callback);
    void SetOnError(std::function<void(const std::wstring&)> callback);
    void SetOnWarning(std::function<void(const std::wstring&)> callback);
    void SetOnTracksChanged(std::function<void()> callback);
    void SetOnChaptersChanged(std::function<void()> callback);
    void ClearCallbacks();
    void SetVideoBounds(int x, int y, int w, int h);
    void SetVideoVisible(bool visible);
    HMODULE GetMpvDll() const { return hMpvDll; }

private:
    friend struct MpvTrackTestAccess;
    std::wstring GetCodec(const char* primary, const char* fallback) const;
    void EventLoop();
    // Normalize both eof-reached (used with keep-open) and END_FILE/EOF into
    // one transition. The ended gate makes duplicate mpv notifications safe.
    void HandleEofTransition();
    void RefreshTracks();
    void RefreshChapters();
    void SetImageAdjustment(int& state, int value, const char* property, const char* operation);
    enum class ErrorSeverity { Recoverable, Fatal };
    void ReportError(const char* operation, int errorCode, ErrorSeverity severity = ErrorSeverity::Recoverable);
    void ReportErrorMessage(const std::wstring& message, ErrorSeverity severity = ErrorSeverity::Recoverable);
    HWND hParent = NULL;
    HWND hVideoWnd = NULL;
    mpv_handle* mpv = nullptr;
    std::wstring currentFilePath;
    std::atomic<bool> isInitialized{ false };
    std::atomic<bool> isRunning{ false };
    std::atomic<bool> ended{ false };
    std::atomic<bool> loop{ false };
    std::thread eventThread;
    mutable std::mutex stateMutex;
    mutable std::mutex mpvMutex;
    mutable std::mutex callbackMutex;
    std::function<void()> onEnded;
    std::function<void(double)> onTimeUpdated;
    std::function<void(double)> onDurationChanged;
    std::function<void(bool)> onPlayStateChanged;
    std::function<void(const std::wstring&)> onError;
    std::function<void(const std::wstring&)> onWarning;
    std::function<void()> onTracksChanged;
    std::function<void()> onChaptersChanged;
    template <typename Callback>
    Callback CopyCallback(const Callback& callback) const { std::lock_guard<std::mutex> lock(callbackMutex); return callback; }
    double currentTime = 0.0, duration = 0.0, volume = 1.0, speed = 1.0;
    bool isPaused = true, isMuted = false;
    uint32_t videoWidth = 0, videoHeight = 0;
    std::wstring lastError;
    std::vector<MpvTrackInfo> audioTracks;
    std::vector<MpvTrackInfo> subTracks;
    mutable double cachedCacheDurationSec = 0.0;
    mutable ULONGLONG lastCacheDurationQueryMs = 0;
    std::vector<MpvChapterInfo> chapters;
    std::atomic<uint64_t> chaptersVersion{ 0 };
    std::atomic<uint64_t> tracksVersion{ 0 };
    int currentChapter = -1, lastSubSafeAreaX = 0, lastSubSafeAreaY = 0;
    ColorManagementPlan lastColorPlan;
    std::vector<ColorPropertyResult> lastColorResults;
    mutable std::vector<AudioDeviceInfo> audioDevices;
    AudioManagementPlan lastAudioPlan;
    std::vector<AudioPropertyResult> lastAudioResults;
    int videoRotate = 0, brightness = 0, contrast = 0, saturation = 0;
    int gamma = 0, hue = 0;
    double videoZoomCur = 0.0, videoPanXCur = 0.0, videoPanYCur = 0.0;
    std::string lastDeinterlaceMode = "off";
    bool customMpvConfigEnabled = false;
    std::string customMpvConfigPath = "mpv.conf";

};

// =========================================================================
// Hub Thumbnail Cache and Hover Preview
// =========================================================================

struct PreviewThumbnailFrame {
    std::wstring filePath;
    double timestampSec = 0.0;
    int width = 0, height = 0;
    std::vector<uint8_t> bgraPixels;
};

// Hub keys are path-only: shell thumbnails can have different dimensions,
// and cards scale the same representative image on draw.
inline std::wstring MakeHubThumbnailCacheKey(const std::wstring& filePath, int width, int height) {
    (void)width; (void)height;
    const auto input = Utf16ToUtf8(filePath) + "|hub";
    const uint64_t hash = Fnv1a64(input.data(), input.size());
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring key = L"hub_0000000000000000.bin";
    for (size_t i = 0; i < 16; ++i) key[4 + i] = hex[(hash >> (60 - i * 4)) & 15];
    return key;
}

// Hub tier persistence (same directory and on-disk format, hub_ key prefix).
bool SaveHubThumbnailToDiskCache( const PreviewThumbnailFrame& frame, const CacheBudgetConfig& config = GetGlobalCacheBudgets(),
    const std::wstring& root = GetExeDir() );

bool LoadHubThumbnailFromDiskCache( const std::wstring& filePath, int width, int height, PreviewThumbnailFrame& outFrame,
    const std::wstring& root = GetExeDir(), bool migrateLegacy = true );

// Prune entries whose media is no longer live, including obsolete hub keys.
size_t PruneOrphanedHubThumbnailDiskCache( const std::vector<std::wstring>& liveFilePaths, const std::wstring& root = GetExeDir() );

// High-performance background video preview renderer using libmpv render context
class MpvPreviewPlayer : private MpvApiOwner {
public:
    using MpvApiOwner::GetInitializationDiagnostics;
    using MpvApiOwner::SetInitializationDiagnosticSink;
    MpvPreviewPlayer();
    ~MpvPreviewPlayer();
    bool Initialize(ID2D1DeviceContext* pD2DContext, HMODULE hDll);
    void Shutdown();
    // Media Card hover preview
    void StartPreview(const std::wstring& filePath, double startSec);
    void StopPreview();
    // Requested pixel size, adopted when the next card preview starts.
    void SetPreviewOutputSize(int width, int height);
    bool IsActive() const { return isActive; }
    std::wstring GetCurrentPreviewPath() const;
    ID2D1Bitmap* GetPreviewBitmap() const { return previewBitmap.Get(); }
    bool UpdateFrame(ID2D1DeviceContext* pD2DContext);
    void BindDeviceContext(ID2D1DeviceContext* context);

private:
    friend struct MpvPreviewTestAccess;
    static void OnRenderUpdate(void* ctx);
    void StopMpvLocked();
    void DrainPreviewEventsLocked();
    void EnsureBufferDimensionsLocked(int width, int height);
    void BindDeviceContextLocked(ID2D1DeviceContext* context);
    bool UploadBitmapLocked(ComPtr<ID2D1Bitmap>& bitmap, ID2D1DeviceContext* context,
                            UINT width, UINT height, const uint8_t* pixels);
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;
    std::wstring currentPreviewPath;
    std::atomic<bool> isActive{ false };
    std::atomic<bool> frameReady{ false };
    std::vector<uint8_t> pixelBuffer;
    // Keep card playback sharp while bounding its CPU render buffer.
    static constexpr int kFallbackBufferWidth = 320, kFallbackBufferHeight = 180;
    static constexpr int kMinPreviewBufferWidth = 640, kMaxPreviewBufferWidth = 960;
    int requestedPreviewWidth = 960, requestedPreviewHeight = 540;
    int bufferWidth = kFallbackBufferWidth, bufferHeight = kFallbackBufferHeight;
    ComPtr<ID2D1Bitmap> previewBitmap;
    ComPtr<ID2D1Device> bitmapDevice;
    mutable std::mutex lifecycleMutex;

};

// Dedicated hub thumbnail extractor for files the Windows shell cannot thumbnail (notably MKV, which has no inbox Explorer handler). It
// owns a separate mpv instance tuned purely for fast single-frame grabs — keyframe seeks, skipped loop filtering, tiny demuxer window — so
// extraction never contends with hover previews or playback decoding, and can run in parallel with them. The App drives it
// while idle: Begin once, Pump every tick (needs no D2D device), TakeResult when ready.
class MpvHubExtractor : private MpvApiOwner {
public:
    using MpvApiOwner::GetInitializationDiagnostics;
    using MpvApiOwner::SetInitializationDiagnosticSink;
    MpvHubExtractor();
    ~MpvHubExtractor();
    bool Begin(const std::wstring& filePath, double startSec);
    void Cancel();
    bool IsActive() const;
    const std::wstring& GetPath() const;
    // Pumps load/decode/render; returns true when it made progress.
    bool Pump();
    bool TakeResult(PreviewThumbnailFrame& outFrame);
    bool IsPrivateMode() const { return isPrivateMode.load(); }
    void SetPrivateMode(bool enabled) { isPrivateMode.store(enabled); }
    void Shutdown();
    // True while the lazy mpv handle exists (idle-teardown decision input).
    bool IsInitialized() const;
    static constexpr int kExtractWidth = 320, kExtractHeight = 180;

private:
    static void OnRenderUpdate(void* ctx);
    bool EnsureInitialized();
    void TeardownLocked(); // caller holds mutex (Begin/Shutdown paths)
    void StopMpvLocked();
    void StartSaveWorker();
    void StopSaveWorker();
    void SaveWorkerThread();
    void QueueSave(PreviewThumbnailFrame frame, std::wstring cacheKey);
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;
    std::atomic<bool> frameReady{ false };
    std::atomic<bool> isPrivateMode{ false };
    std::vector<uint8_t> pixelBuffer;
    bool active = false, resultReady = false;
    std::wstring extractPath;
    double extractStartSec = 0.0;
    long long extractStartTickMs = 0;
    static constexpr long long kExtractTimeoutMs = 6000;
    PreviewThumbnailFrame resultFrame;
    mutable std::mutex mutex;
    std::thread saveWorker;
    std::mutex saveMutex;
    std::condition_variable saveCv;
    struct PendingSave { PreviewThumbnailFrame frame; std::wstring cacheKey; };
    std::deque<PendingSave> saveQueue;
    bool saveStopping = false;
    static constexpr size_t kMaxPendingSaves = 8;

};

} // namespace Aperture
