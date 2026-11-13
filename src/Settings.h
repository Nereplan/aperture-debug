#pragma once

#include "Common.h"
#include "Json.h"
#include <string>
#include <vector>
#include <map>

namespace Aperture {

struct LocaleInfo;
struct ThemePalette {
    std::string id, displayName;
    D2D1_COLOR_F bg;
    D2D1_COLOR_F cardBg;
    D2D1_COLOR_F iconBg;
    D2D1_COLOR_F border;
    D2D1_COLOR_F trackBg;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentDark;
    D2D1_COLOR_F accentSubtle;
    D2D1_COLOR_F text;
    D2D1_COLOR_F textMuted;
    D2D1_COLOR_F red;
    // Semantic tokens for elevated/floating/modal/scrim/focus states
    D2D1_COLOR_F surfaceElevated = { 0, 0, 0, 1 };
    D2D1_COLOR_F surfaceFloating = { 0, 0, 0, 1 };
    D2D1_COLOR_F surfaceModal = { 0, 0, 0, 1 };
    D2D1_COLOR_F scrim = { 0, 0, 0, 0.65f };
    D2D1_COLOR_F focusRing = { 0, 0, 0, 1 };
    D2D1_COLOR_F borderSubtle = { 1, 1, 1, 0.08f };
    D2D1_COLOR_F surfaceHover = { 1, 1, 1, 0.04f };
    D2D1_COLOR_F surfaceSelected = { 1, 1, 1, 0.08f };
    float cardRadius = 8.0f, iconRadius = 4.0f, menuRadius = 6.0f;
};

struct ShortcutBinding { std::string actionId, displayName; UINT vkCode = 0; bool ctrl = false, shift = false, alt = false; };


struct SubtitleStyleProfile {
    std::string name;
    std::string fontSize = "20px";
    std::string colorHex = "#cad3f5";
    std::string bgStyle = "vlc-outline";
    std::string position = "bottom";
    int safeAreaX = 0; // Horizontal safe area percentage [0..30]
    int safeAreaY = 0; // Vertical safe area percentage [0..30]
    JsonValue unknownFields = JsonValue::Object();
};

class Settings {
public:
    static constexpr int kCurrentSettingsSchemaVersion = 16;
    static constexpr size_t kMaxSubtitleProfiles = 32, kMaxProfileNameLength = 64;
    static bool MigrateSchema(JsonValue& root, int fromVersion, int targetVersion = kCurrentSettingsSchemaVersion);
    // --- Appearance ---
    std::string themePreset = "default";
    std::string accentColorHex = ""; // empty = theme accent
    std::string uiFontFamily = "Inter";
    std::string appLanguage = "en-US";
    // --- Playback ---
    bool autoplay = true, rememberPos = true, restoreSession = true, loop = false;
    bool shuffleQueue = false;
    bool alwaysOnTop = false;
    bool hwAccel = true;
    double defaultSpeed = 1.0, arrowSeekStepSec = 5.0;
    double largeSeekStepSec = 10.0;  // Large jump step in seconds (J/L keys), clamped 1.0-600.0
    double mediumSeekStepSec = 30.0; // Medium jump step in seconds (Shift+Arrow keys), clamped 1.0-600.0
    int ctrlSeekFrames = 1;
    int networkForwardCacheMB = 8; // Network stream demuxer readahead cache, clamped 8-512 MB
    int networkBackCacheMB = 0;    // Network stream backward demuxer cache, clamped 0-256 MB
    bool customMpvConfigEnabled = false;
    std::string customMpvConfigPath = "mpv.conf";
    bool globalHotkeysEnabled = false; // System-wide media hotkeys (play/pause, next, previous, mute)
    bool trayIconEnabled = false;      // Show the Aperture icon in the system notification area
    bool minimizeToTray = false;       // Hide to the tray instead of the taskbar when minimized (requires trayIconEnabled)
    std::string screenshotFormat = "png"; // "png" or "jpg"
    bool jumpListEnabled = false, hoverPreviewEnabled = true, reduceMotionEnabled = false;
    std::string experienceMode = "simple";          // "simple", "advanced", or "custom" (FeatureGate)
    std::vector<std::string> customEnabledFeatures; // FeatureGate feature keys enabled in "custom" mode
    // --- Color & HDR Management ---
    std::string hdrMode = "auto";               // "auto", "passthrough", "tone-map", "off"
    std::string toneMappingAlgorithm = "auto";  // "auto", "spline", "bt.2390", "bt.2446a", "hable", "mobius", "reinhard", "clip", "linear", "gamma"
    int targetPeak = 0;                         // 0 = auto (display peak), or 100..10000 nits
    std::string gamutMapping = "auto";          // "auto", "clip", "warn", "linear", "darken", "desaturate"
    std::string iccProfilePath = "";            // Root-relative path to ICC profile (e.g. "icc/display.icc")
    // --- Audio & Subtitles ---
    std::string audioLang = "eng";
    std::string subLang = "eng";
    std::string lastAudioTrackTitle = "";
    std::string lastSubtitleTrackTitle = "";
    bool subtitlesEnabled = true;
    std::string subFontSize = "20px";
    std::string subColorHex = "#cad3f5";
    std::string subBgStyle = "vlc-outline"; // "vlc-outline", "translucent", "solid", "clean"
    std::string subPosition = "bottom";      // "bottom", "elevated", "top"
    int subSafeAreaX = 0;                    // Horizontal safe area percentage [0..30]
    int subSafeAreaY = 0;                    // Vertical safe area percentage [0..30]
    bool subForcedOnly = false;              // Render only forced subtitle tracks
    std::string subCodepage = "auto";        // "auto" or a subtitle character-set override (e.g. "utf-8", "windows-1254")
    std::string subFontFamily = "Inter";     // Subtitle font family (mpv sub-font)
    int audioDelayMs = 0, subDelayMs = 0;
    // --- Subtitle Style Profiles ---
    std::string activeSubtitleProfile = "";
    std::vector<SubtitleStyleProfile> subtitleProfiles;
    // --- Dual Subtitles ---
    std::string secondarySubLang = "";
    std::string lastSecondarySubtitleTrackTitle = "";
    bool secondarySubtitlesEnabled = false;
    std::string secondarySubFontSize = "18px";
    std::string secondarySubColorHex = "#ffff00";
    std::string secondarySubPosition = "top";        // "top", "bottom", "elevated"
    int secondarySubDelayMs = 0;
    // --- Online Subtitles ---
    bool onlineSubtitlesEnabled = false;
    std::string onlineSubtitleEndpoint = "";
    // --- Surround Audio, Night Mode, Balance & Channel Map ---
    std::string audioDevice = "auto";
    std::string audioLayout = "auto-safe";         // "auto-safe", "stereo", "5.1", "7.1", "auto"
    std::string audioChannelMix = "auto";          // "auto", "none", "downmix-stereo", "upmix-surround"
    bool audioNormalizeDownmix = true;
    std::string nightMode = "off";                 // "off", "light", "medium", "strong"
    double audioBalance = 0.0;                     // -1.0 to 1.0
    std::string audioChannelMap = "auto";          // "auto", "stereo", "mono-left", "mono-right", "swap"
    std::string replayGainMode = "off";            // "off", "track", "album"
    double replayGainPreampDb = 0.0;               // -15.0 to +15.0 dB
    std::string deinterlaceMode = "off";           // "off", "bwdif", "bwdif_field", "yadif"
    bool audioPassthrough = false;                 // S/PDIF / HDMI bitstream passthrough
    bool pitchCorrection = true;                   // Keep pitch constant when changing speed
    // --- Storage ---
    std::string storageMode = "source";
    // Playlist presentation preference. Keep the persisted representation stable and human-readable so unknown values can safely fall back
    // to the Web/native default list view.
    std::string playlistViewMode = "list"; // "list" or "grid"
    std::string libraryGroupMode = "flat"; // "flat", "folder", "series"
    std::string librarySortField = "auto"; // "auto", "manual", "title", "dateAdded", "lastPlayed", "duration", "progress"
    std::string librarySortDirection = "ascending"; // "ascending" or "descending"
    bool manualMediaOrder = false;
    std::vector<std::wstring> customFolderOrder;
    bool smartSeriesDetection = false, diagnosticsLogging = false;
    // --- Updates (Manual by default, opt-in automatic checks) ---
    bool autoUpdateCheckEnabled = false;
    std::string updateChannel = "stable";
    std::string updateCheckUrl = "";
    // --- Cache Budgets (in Megabytes) ---
    // Defaults match CacheBudgetConfig: 160 total == 64+64+16+16 class sum.
    int cacheTotalBudgetMB = 160, cacheThumbnailBudgetMB = 64, cacheShaderBudgetMB = 64, cacheSubtitleBudgetMB = 16;
    int cacheGenericBudgetMB = 16;
    // --- Volume & State ---
    double volume = 1.0; // 0.0 to 1.0
    bool muted = false;
    std::string lastAspectRatio = "auto"; // "auto", "16:9", "4:3", "21:9", "fill", "cover", or custom "W:H"/ratio
    int videoRotate = 0;                  // 0-359 degrees clockwise
    int brightness = 0;                   // -100 to 100
    int contrast = 0;                     // -100 to 100
    int saturation = 0;                   // -100 to 100
    int gamma = 0;                        // -100 to 100
    int hue = 0;                          // -100 to 100
    // --- Window State ---
    int windowX = -1, windowY = -1, windowWidth = 1280, windowHeight = 780;
    bool hasWindowPlacement = false;
    bool windowMaximized = false;
    int pipX = -1, pipY = -1, pipWidth = 0, pipHeight = 0;
    // Shortcuts
    std::map<std::string, ShortcutBinding> shortcuts;
    // Locale-keyed cache for GetShortcutActionListRef.
    mutable std::vector<std::pair<std::string, std::string>> shortcutActionListCache;
    mutable std::string shortcutActionListCacheLocale;
    // Runtime Theme Cache
    ThemePalette currentTheme;
    Settings();
    StorageStatus Load();
    StorageStatus Save();
    bool WasRecoveredFromBackup() const { return recoveredFromBackup; }
    int GetLoadedSchemaVersion() const { return loadedSchemaVersion; }
    StorageStatus GetLastLoadStatus() const { return lastLoadStatus; }
    CacheBudgetConfig GetCacheBudgets() const {
        CacheBudgetConfig cfg;
        cfg.totalCeilingBytes = static_cast<uint64_t>(std::max(0, cacheTotalBudgetMB)) * 1024ULL * 1024ULL;
        cfg.thumbnailBudgetBytes = static_cast<uint64_t>(std::max(0, cacheThumbnailBudgetMB)) * 1024ULL * 1024ULL;
        cfg.shaderBudgetBytes = static_cast<uint64_t>(std::max(0, cacheShaderBudgetMB)) * 1024ULL * 1024ULL;
        cfg.subtitleBudgetBytes = static_cast<uint64_t>(std::max(0, cacheSubtitleBudgetMB)) * 1024ULL * 1024ULL;
        cfg.genericBudgetBytes = static_cast<uint64_t>(std::max(0, cacheGenericBudgetMB)) * 1024ULL * 1024ULL;
        return cfg;
    }
    void ResetToDefaults();
    void UpdateThemeColors();
    ThemePalette GetThemePalette(const std::string& id) const;
    const std::vector<ThemePalette>& GetAllThemes() const;
    const std::vector<std::string>& GetAvailableFonts() const;
    std::vector<LocaleInfo> GetAvailableLanguages() const;
    float GetSubFontSizePoints() const;
    D2D1_COLOR_F GetSubTextColor() const;
    // The list is rebuilt only when the active locale changes; hot callers
    // (hover handlers, per-frame tooltip refresh) borrow the cached vector.
    const std::vector<std::pair<std::string, std::string>>& GetShortcutActionListRef() const;
    static std::wstring FormatShortcutKey(const ShortcutBinding& b);
    static std::string VKToString(UINT vkCode);
    static UINT StringToVK(const std::string& str);
    void ResetShortcuts();
    // --- Color & HDR Normalization & Containment ---
    static std::string NormalizeHdrMode(const std::string& mode);
    static std::string NormalizeToneMapping(const std::string& tm);
    static int NormalizeTargetPeak(int peak);
    static std::string NormalizeGamutMapping(const std::string& gm);
    static bool IsRootContainedIccPath(const std::string& path, const std::wstring& exeDir = GetExeDir());
    static std::string NormalizeIccPath(const std::string& path, const std::wstring& exeDir = GetExeDir());
    // --- Custom MPV Configuration Containment & Recovery ---
    static bool IsRootContainedMpvConfigPath(const std::string& path, const std::wstring& exeDir = GetExeDir());
    static std::string NormalizeMpvConfigPath(const std::string& path, const std::wstring& exeDir = GetExeDir());
    static std::wstring ResolveMpvConfigPath(const std::string& path, const std::wstring& exeDir = GetExeDir());
    static std::wstring GetMpvConfigLastKnownGoodPath(const std::wstring& configPath);
    static StorageStatus SaveMpvConfigLastKnownGood(const std::wstring& configPath, const std::string& content);
    static StorageStatus LoadMpvConfigLastKnownGood(const std::wstring& configPath, std::string& outContent);
    static StorageStatus RevertMpvConfigToLastKnownGood(const std::wstring& configPath);
    static StorageStatus ResetMpvConfigToDefault(const std::wstring& configPath);
    // --- Surround Audio, Night Mode, Balance & Channel Map Normalization ---
    static std::string NormalizeAudioDevice(const std::string& dev);
    static std::string NormalizeAudioLayout(const std::string& layout);
    static std::string NormalizeAudioChannelMix(const std::string& mix);
    static std::string NormalizeNightMode(const std::string& nm);
    static double NormalizeAudioBalance(double balance);
    static std::string NormalizeAudioChannelMap(const std::string& map);
    static std::string NormalizeReplayGainMode(const std::string& mode);
    static double NormalizeReplayGainPreampDb(double db);
    static std::string NormalizeDeinterlaceMode(const std::string& mode);
    // --- Subtitle Style Profile Validation & Normalization ---
    static bool IsValidProfileName(const std::string& name);
    static std::string SanitizeProfileName(const std::string& name);
    static std::string NormalizeProfileFontSize(const std::string& size);
    static std::string NormalizeProfileColor(const std::string& hex);
    static std::string NormalizeProfileBgStyle(const std::string& style);
    static std::string NormalizeProfilePosition(const std::string& pos);
    static int NormalizeSafeAreaPercent(int percent);
    const SubtitleStyleProfile* FindSubtitleProfile(const std::string& name) const;
    SubtitleStyleProfile* FindSubtitleProfile(const std::string& name);
    bool AddOrUpdateSubtitleProfile(const SubtitleStyleProfile& profile);
    bool RenameSubtitleProfile(const std::string& oldName, const std::string& newName);
    bool DeleteSubtitleProfile(const std::string& name);
    bool ApplySubtitleProfile(const std::string& name);
    void ResetSubtitleStyleToDefault();
    SubtitleStyleProfile GetCurrentSubtitleStyleAsProfile(const std::string& name = "") const;
private:
    JsonValue persistedFields;
    JsonValue persistedShortcutFields;
    std::string lastSavedJson;
    StorageStatus lastSaveStatus = StorageStatus::Ok();
    StorageStatus lastLoadStatus = StorageStatus::Ok();
    bool recoveredFromBackup = false;
    bool savesAllowed = true;
    int loadedSchemaVersion = 0;
};

} // namespace Aperture
