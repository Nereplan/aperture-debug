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
#include <charconv>

namespace Aperture {

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
    int64_t droppedFrames = 0, voDroppedFrames = 0, mistimedFrames = 0;
    double videoBitrate = 0.0; // kbps
    double audioBitrate = 0.0; // kbps
    double cacheDuration = 0.0;
    int64_t cacheUsedBytes = 0;
    double avSync = 0.0;
    int64_t audioSampleRate = 0, audioChannels = 0;
    std::wstring audioTrackTitle, subTrackTitle;
    int activeSubSafeAreaX = 0, activeSubSafeAreaY = 0;
    std::wstring activeSubSafeArea;
    // --- Audio & Surround Diagnostics ---
    std::wstring activeAudioDevice, activeAudioDeviceDescription, activeAudioLayout, activeAudioChannelMix;
    std::wstring activeNightMode, activeAudioFilters;
    double activeAudioBalance = 0.0;
    std::wstring activeAudioChannelMap;
    bool audioDeviceFallback = false;
    std::wstring colorPrimaries, colorTransfer, colorSpace, colorLevels;
    double sigPeak = 0.0, maxCll = 0.0, maxFall = 0.0, minLuma = 0.0;
    double maxLuma = 0.0;
    bool isHdrSource = false;
    std::wstring hdrType = L"SDR";
    std::wstring activeOutputMode = L"SDR Native";
    std::wstring activeToneMapping, activeGamutMapping, activeIccProfile, colorDiagnosticsStr;
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
    // Runtime playback preferences. These are safe to call before a file is
    // loaded; they take effect immediately when libmpv accepts the property.
    void SetHardwareAcceleration(bool enabled);
    void SetPreferredAudioLanguage(const std::string& language);
    void SetPreferredSubtitleLanguage(const std::string& language);
    void SetSubtitleStyle(const std::string& fontSize, const std::string& colorHex, const std::string& backgroundStyle,
                          const std::string& position, int safeAreaX = 0, int safeAreaY = 0);
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
    bool IsCustomMpvConfigEnabled() const { return customMpvConfigEnabled; }
    void SetCustomMpvConfigPath(const std::string& path) { customMpvConfigPath = path; }
    std::string GetCustomMpvConfigPath() const { return customMpvConfigPath; }
    CustomMpvApplyResult GetLastCustomMpvResult() const { return lastCustomMpvResult; }
    // Audio device enumeration, surround layouts, and dynamic range compression
    std::vector<AudioDeviceInfo> GetAudioDevices() const;
    void RefreshAudioDevices();
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, double balance, const std::string& channelMap,
        const std::vector<AudioDeviceInfo>& availableDevices = {} );
    static AudioManagementPlan BuildAudioPlan( const std::string& device, const std::string& layout, const std::string& channelMix,
        bool normalizeDownmix, const std::string& nightMode, const std::vector<AudioDeviceInfo>& availableDevices = {} );
    std::vector<AudioPropertyResult> SetAudioPreferences( const std::string& device, const std::string& layout,
        const std::string& channelMix, bool normalizeDownmix, const std::string& nightMode, double balance = 0.0,
        const std::string& channelMap = "auto" );
    std::vector<AudioPropertyResult> SetAudioBalance(double balance);
    std::vector<AudioPropertyResult> SetAudioChannelMap(const std::string& channelMap);
    AudioManagementPlan GetLastAudioPlan() const;
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
    bool customMpvConfigEnabled = false;
    std::string customMpvConfigPath = "mpv.conf";
    CustomMpvApplyResult lastCustomMpvResult;

};

// =========================================================================
// Video Thumbnail Preview & Cache System (Seekbar Hover & Scrubbing)
// =========================================================================

inline double QuantizePreviewTime(double rawTimeSec, double durationSec, double quantizeStepSec = 1.0) {
    if (!std::isfinite(rawTimeSec) || !std::isfinite(durationSec) || durationSec <= 0.0 || rawTimeSec <= 0.0) { return 0.0; }
    double clamped = std::clamp(rawTimeSec, 0.0, durationSec);
    if (!std::isfinite(quantizeStepSec) || quantizeStepSec <= 0.0) { return clamped; }
    double quantized = std::round(clamped / quantizeStepSec) * quantizeStepSec;
    return std::clamp(quantized, 0.0, durationSec);
}

struct PreviewThumbnailFrame {
    std::wstring filePath;
    double timestampSec = 0.0;
    int width = 0, height = 0;
    std::vector<uint8_t> bgraPixels;
    uint64_t accessCounter = 0;
    size_t GetMemoryBytes() const { return sizeof(PreviewThumbnailFrame) + filePath.capacity() * sizeof(wchar_t) + bgraPixels.capacity(); }
};

class ThumbnailKeyBuilder {
public:
    explicit ThumbnailKeyBuilder(const std::wstring& path) {
        const auto utf8 = Utf16ToUtf8(path);
        prefix_ = (Fnv1a64(utf8.data(), utf8.size()) ^ uint64_t('|')) * 1099511628211ull;
    }
    uint64_t SeekHash(uint64_t milliseconds) const {
        char digits[20]; // uint64_t's longest decimal representation
        const auto converted = std::to_chars(digits, digits + sizeof(digits), milliseconds);
        return Append(prefix_, digits, converted.ptr);
    }
    static std::wstring SeekKey(uint64_t hash) { return Format(L"thumb_", hash); }
    std::wstring HubKey() const {
        constexpr char suffix[] = "hub";
        return Format(L"hub_", Append(prefix_, suffix, suffix + 3));
    }
private:
    static uint64_t Append(uint64_t hash, const char* begin, const char* end) {
        for (; begin != end; ++begin) hash = (hash ^ static_cast<unsigned char>(*begin)) * 1099511628211ull;
        return hash;
    }
    static std::wstring Format(const wchar_t* prefix, uint64_t hash) {
        constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring key(prefix);
        const size_t start = key.size();
        key.resize(start + 20);
        for (size_t i = 0; i < 16; ++i) key[start + i] = hex[(hash >> (60 - i * 4)) & 15];
        std::copy_n(L".bin", 4, key.begin() + start + 16);
        return key;
    }
    uint64_t prefix_ = 0;
};

inline std::wstring MakeThumbnailCacheKey(const std::wstring& filePath, double quantizedTimeSec) {
    const ThumbnailKeyBuilder key(filePath);
    return key.SeekKey(key.SeekHash(static_cast<uint64_t>(std::llround(quantizedTimeSec * 1000.0))));
}

// Hub card thumbnails share cache/thumbnails with the seekbar tier but use a distinct path-only key (no dimensions or timestamp): hub cards
// show one representative frame per file while seekbar entries are per-second.
inline std::wstring MakeHubThumbnailCacheKey(const std::wstring& filePath, int width, int height) {
    // Size-independent by design: hub thumbnails come from shell extraction at whatever size the shell returns, and cards scale on draw.
    // Including the size would fragment one file's entry across unpredictable keys and make cache purging on media removal impossible.
    // (width/height params are kept for source compatibility.)
    (void)width; (void)height;
    return ThumbnailKeyBuilder(filePath).HubKey();
}

class ThumbnailMemoryCache {
public:
    explicit ThumbnailMemoryCache(size_t maxCount = 8, size_t maxBytes = 2 * 1024 * 1024) : maxCount_(maxCount), maxBytes_(maxBytes) {}
    bool TryGet(const std::wstring& filePath, double quantizedTimeSec, PreviewThumbnailFrame& outFrame) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : entries_) {
            if (item.filePath == filePath && std::abs(item.timestampSec - quantizedTimeSec) < 0.001) {
                item.accessCounter = ++accessCounter_;
                outFrame = item;
                return true;
            }
        }
        return false;
    }
    void Put(const PreviewThumbnailFrame& frame) {
        if (frame.filePath.empty() || frame.bgraPixels.empty() || frame.width <= 0 || frame.height <= 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : entries_) {
            if (item.filePath == frame.filePath && std::abs(item.timestampSec - frame.timestampSec) < 0.001) {
                totalBytes_ -= item.GetMemoryBytes();
                item = frame;
                item.accessCounter = ++accessCounter_;
                totalBytes_ += item.GetMemoryBytes();
                EvictIfNeeded();
                return;
            }
        }
        PreviewThumbnailFrame copy = frame;
        copy.accessCounter = ++accessCounter_;
        totalBytes_ += copy.GetMemoryBytes();
        entries_.push_back(std::move(copy));
        EvictIfNeeded();
    }
    void InvalidateMedia(const std::wstring& filePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (filePath.empty()) { entries_.clear(); totalBytes_ = 0; return; }
        auto it = entries_.begin();
        while (it != entries_.end()) {
            if (it->filePath == filePath) { totalBytes_ -= it->GetMemoryBytes(); it = entries_.erase(it); } else { ++it; }
        }
    }
    void Clear() { std::lock_guard<std::mutex> lock(mutex_); entries_.clear(); totalBytes_ = 0; }
    size_t GetCount() const { std::lock_guard<std::mutex> lock(mutex_); return entries_.size(); }
    size_t GetTotalBytes() const { std::lock_guard<std::mutex> lock(mutex_); return totalBytes_; }

private:
    void EvictIfNeeded() {
        while (!entries_.empty() && (entries_.size() > maxCount_ || totalBytes_ > maxBytes_)) {
            auto oldestIt = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (it->accessCounter < oldestIt->accessCounter) { oldestIt = it; }
            }
            totalBytes_ -= oldestIt->GetMemoryBytes();
            entries_.erase(oldestIt);
        }
    }
    mutable std::mutex mutex_;
    size_t maxCount_ = 8, maxBytes_ = 2 * 1024 * 1024, totalBytes_ = 0;
    uint64_t accessCounter_ = 0;
    std::vector<PreviewThumbnailFrame> entries_;
};

bool SaveThumbnailToDiskCache( const PreviewThumbnailFrame& frame, const CacheBudgetConfig& config = GetGlobalCacheBudgets(),
    const std::wstring& root = GetExeDir() );

bool LoadThumbnailFromDiskCache( const std::wstring& filePath, double quantizedTimeSec, PreviewThumbnailFrame& outFrame,
    const std::wstring& root = GetExeDir(), bool migrateLegacy = true );

// Hub tier persistence (same directory and on-disk format, hub_ key prefix).
bool SaveHubThumbnailToDiskCache( const PreviewThumbnailFrame& frame, const CacheBudgetConfig& config = GetGlobalCacheBudgets(),
    const std::wstring& root = GetExeDir() );

bool LoadHubThumbnailFromDiskCache( const std::wstring& filePath, int width, int height, PreviewThumbnailFrame& outFrame,
    const std::wstring& root = GetExeDir(), bool migrateLegacy = true );

// Removal hygiene for the durable thumbnail tier. The path-specific purge
// handles the hub key plus every possible one-second seekbar key; the prune
// removes orphaned/legacy hub keys while preserving current live media.
size_t PurgeThumbnailDiskCacheForFile( const std::wstring& filePath, double durationSec, const std::wstring& root = GetExeDir() );

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
    // Requested pixel size for card hover previews. The render buffer is (re)allocated from this when the next preview starts. Seekbar
    // thumbnails keep using their own fixed small size.
    void SetPreviewOutputSize(int width, int height);
    bool IsActive() const { return isActive; }
    std::wstring GetCurrentPreviewPath() const;
    ID2D1Bitmap* GetPreviewBitmap() const { return previewBitmap.Get(); }
    // Seekbar Thumbnail preview
    bool RequestSeekbarThumbnail(const std::wstring& filePath, double rawTimeSec, double durationSec, uint64_t mediaToken, ID2D1DeviceContext* pD2DContext = nullptr);
    void ResetSeekbarPreview();
    bool IsSeekbarActive() const { return isSeekbarActive; }
    bool HasSeekbarThumbnail() const { return hasSeekbarThumbnail; }
    bool IsSeekbarThumbnailLoading() const { return isSeekbarLoading; }
    ThumbnailMemoryCache& GetMemoryCache() { return memoryCache; }
    bool IsPrivateMode() const { return isPrivateMode.load(); }
    void SetPrivateMode(bool enabled) { isPrivateMode.store(enabled); }
    bool UpdateFrame(ID2D1DeviceContext* pD2DContext);
    void BindDeviceContext(ID2D1DeviceContext* context);

private:
    friend struct MpvPreviewTestAccess;
    static void OnRenderUpdate(void* ctx);
    void StopMpvLocked();
    void DrainPreviewEventsLocked(bool acceptRestart);
    bool IssueSeekbarSeekLocked(double time, uint64_t sequence, uint64_t mediaToken);
    void ResetSeekbarPreviewLocked();
    void EnsureBufferDimensionsLocked(int width, int height, ID2D1DeviceContext* pD2DContext);
    void BindDeviceContextLocked(ID2D1DeviceContext* context);
    bool UploadBitmapLocked(ComPtr<ID2D1Bitmap>& bitmap, ID2D1DeviceContext* context,
                            UINT width, UINT height, const uint8_t* pixels);
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;
    std::wstring currentPreviewPath;
    std::atomic<bool> isActive{ false };
    std::atomic<bool> isSeekbarActive{ false };
    std::atomic<bool> frameReady{ false };
    // Seekbar preview state
    std::atomic<uint64_t> currentSequenceId{ 0 };
    std::atomic<uint64_t> requestedMediaToken{ 0 };
    std::wstring requestedFilePath;
    double requestedTime = 0.0;
    uint64_t inFlightSequenceId = 0, inFlightMediaToken = 0;
    double inFlightTime = 0.0;
    bool seekbarCapturePending = false, seekbarRestarted = false;
    bool seekbarRequestFailed = false;
    bool hasPendingSeek = false;
    double pendingSeekTime = 0.0;
    uint64_t pendingSeekSeq = 0, pendingSeekMediaToken = 0;
    std::chrono::steady_clock::time_point lastSeekTime{};
    std::atomic<bool> hasSeekbarThumbnail{ false };
    std::atomic<bool> isSeekbarLoading{ false };
    double activeSeekbarTime = -1.0;
    std::atomic<bool> isPrivateMode{ false };
    ThumbnailMemoryCache memoryCache;
    std::vector<uint8_t> pixelBuffer;
    // Seekbar thumbnails render at a fixed small size and feed the thumbnail caches, so their dimensions must stay stable. Card hover
    // previews allocate from requestedPreviewWidth/Height. 960x540 stays sharp on the expanded card while halving the peak preview buffer
    // versus 1280x720.
    static constexpr int kSeekbarBufferWidth = 320, kSeekbarBufferHeight = 180, kMinPreviewBufferWidth = 640, kMaxPreviewBufferWidth = 960;
    int requestedPreviewWidth = 960, requestedPreviewHeight = 540, bufferWidth = kSeekbarBufferWidth, bufferHeight = kSeekbarBufferHeight;
    bool bufferIsPreviewSized = false;
    ComPtr<ID2D1Bitmap> previewBitmap, seekbarBitmap;
    ComPtr<ID2D1Device> bitmapDevice;
    std::mutex renderMutex;
    mutable std::mutex lifecycleMutex;
    // Background disk-save queue: thumbnail persistence (directory budget scans + file I/O + flush) must never run on the render thread.
    // UpdateFrame enqueues a copy and returns; the saver thread persists it
    // under <root>\cache\thumbnails. Bounded and coalesced by key so fast scrubbing cannot pile up unbounded file writes.
    static constexpr size_t kMaxPendingDiskSaves = 8;
    void StopSaveWorker();
    void SaveWorkerThread();
    void QueueDiskSave(const PreviewThumbnailFrame& frame);
    std::thread saveWorker;
    std::mutex saveMutex;
    std::condition_variable saveCv;
    std::deque<PreviewThumbnailFrame> saveQueue;
    bool saveStopping = false;

};

// Dedicated hub thumbnail extractor for files the Windows shell cannot thumbnail (notably MKV, which has no inbox Explorer handler). It
// owns a separate mpv instance tuned purely for fast single-frame grabs — keyframe seeks, skipped loop filtering, tiny demuxer window — so
// extraction never contends with hover previews, seekbar work, or playback decoding, and can run in parallel with them. The App drives it
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
