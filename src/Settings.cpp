#include "Settings.h"
#include "JsonStoreValidation.h"
#include "AudioConfig.h"
#include "ChannelPolicy.h"
#include "Localization.h"
#include "FeatureGate.h"
#include "MpvPlayer.h" // kMaxVolumeRatio
#include <fstream>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#include <windows.h>
#include <shlwapi.h>
namespace Aperture {

namespace {
bool IsHexColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); ++i) { if (!std::isxdigit((unsigned char)s[i])) return false; }
    return true;
}

double Number(const JsonValue& v, double d) {
    if (v.is_number() && std::isfinite(v.as_number())) return v.as_number();
    if (v.is_string()) {
        char* e = nullptr;
        double n = std::strtod(v.as_string_ref().c_str(), &e);
        if (e && *e == '\0' && std::isfinite(n)) return n;
    }
    return d;
}

int Integer(const JsonValue& v, int d) {
    double n = Number(v, d);
    return n > INT_MAX ? INT_MAX : n < INT_MIN ? INT_MIN : (int)std::llround(n);
}

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

std::optional<std::wstring> ContainedRelativePath(const std::string& rawPath, const std::wstring& exeDir, bool allowEmptyOrRoot) {
    std::wstring input = Utf8ToUtf16(TrimAscii(rawPath));
    if (input.empty()) return allowEmptyOrRoot ? std::optional<std::wstring>(L"") : std::nullopt;
    for (wchar_t& ch : input) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' || ch == L'?' || ch == L'*') return std::nullopt;
        if (ch == L'/') ch = L'\\';
    }
    std::wstring baseDir = exeDir;
    while (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/')) baseDir.pop_back();
    if (baseDir.empty()) return std::nullopt;
    const std::wstring candidate = PathIsRelativeW(input.c_str()) ? baseDir + L"\\" + input : input;
    std::wstring canonicalPath(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(canonicalPath.size()), canonicalPath.data(), nullptr);
        if (written == 0) return std::nullopt;
        if (written < canonicalPath.size()) { canonicalPath.resize(written); break; }
        if (written > 32768 || canonicalPath.size() == 32768) return std::nullopt;
        canonicalPath.resize(std::min<size_t>(32768, std::max<size_t>(written, canonicalPath.size() * 2)));
    }
    if (canonicalPath.size() < baseDir.size() ||
        !std::equal(baseDir.begin(), baseDir.end(), canonicalPath.begin(),
            [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); })) return std::nullopt;
    if (canonicalPath.size() == baseDir.size()) return allowEmptyOrRoot ? std::optional<std::wstring>(L"") : std::nullopt;
    if (canonicalPath[baseDir.size()] != L'\\') return std::nullopt;
    return canonicalPath.substr(baseDir.size() + 1);
}
}

Settings::Settings() { ResetToDefaults(); }

void Settings::ResetToDefaults() {
    savesAllowed = true; // An explicit reset authorizes replacing unrecovered settings.
    persistedFields = JsonValue::Object();
    lastSavedJson.clear();
    themePreset = "default";
    accentColorHex = "";
    uiFontFamily = "Inter";
    appLanguage = "en-US";
    autoplay = true;
    rememberPos = true;
    restoreSession = true;
    loop = false;
    shuffleQueue = false;
    alwaysOnTop = false;
    hwAccel = true;
    defaultSpeed = 1.0;
    arrowSeekStepSec = 5.0;
    largeSeekStepSec = 10.0;
    ctrlSeekFrames = 1;
    customMpvConfigEnabled = false;
    customMpvConfigPath = "mpv.conf";
    globalHotkeysEnabled = false;
    trayIconEnabled = false;
    minimizeToTray = false;
    screenshotFormat = "png";
    jumpListEnabled = false;
    hoverPreviewEnabled = true;
    reduceMotionEnabled = false;
    experienceMode = "simple";
    customEnabledFeatures.clear();
    hdrMode = "auto";
    toneMappingAlgorithm = "auto";
    targetPeak = 0;
    gamutMapping = "auto";
    iccProfilePath = "";
    audioLang = "eng";
    subLang = "eng";
    subFontSize = "20px";
    subColorHex = "#cad3f5";
    subBgStyle = "vlc-outline";
    subPosition = "bottom";
    subSafeAreaX = 0;
    subSafeAreaY = 0;
    subForcedOnly = false;
    subCodepage = "auto";
    subFontFamily = "Inter";
    audioDelayMs = 0;
    subDelayMs = 0;
    activeSubtitleProfile = "";
    subtitleProfiles.clear();
    secondarySubLang = "";
    lastSecondarySubtitleTrackTitle = "";
    secondarySubtitlesEnabled = false;
    secondarySubFontSize = "18px";
    secondarySubColorHex = "#ffff00";
    secondarySubPosition = "top";
    secondarySubDelayMs = 0;
    onlineSubtitlesEnabled = false;
    onlineSubtitleEndpoint = "";
    audioDevice = "auto";
    audioLayout = "auto-safe";
    audioChannelMix = "auto";
    audioNormalizeDownmix = true;
    nightMode = "off";
    audioBalance = 0.0;
    audioChannelMap = "auto";
    replayGainMode = "off";
    replayGainPreampDb = 0.0;
    deinterlaceMode = "off";
    audioPassthrough = false;
    pitchCorrection = true;
    storageMode = "source";
    playlistViewMode = "list";
    libraryGroupMode = "flat";
    librarySortField = "auto";
    librarySortDirection = "ascending";
    manualMediaOrder = false;
    customFolderOrder.clear();
    smartSeriesDetection = false;
    diagnosticsLogging = false;
    autoUpdateCheckEnabled = false;
    updateChannel = ChannelPolicy::Instance().GetDefaultChannel();
    updateCheckUrl = "";
    volume = 1.0;
    muted = false;
    lastAspectRatio = "auto";
    videoRotate = 0;
    brightness = 0;
    contrast = 0;
    saturation = 0;
    gamma = 0;
    hue = 0;
    windowX = -1;
    windowY = -1;
    windowWidth = 1280;
    windowHeight = 780;
    windowMaximized = false;
    hasWindowPlacement = false;
    pipX = -1;
    pipY = -1;
    pipWidth = 0;
    pipHeight = 0;
    const CacheBudgetConfig cacheDefaults;
    constexpr uint64_t mib = 1024ULL * 1024ULL;
    cacheTotalBudgetMB = static_cast<int>(cacheDefaults.totalCeilingBytes / mib);
    cacheThumbnailBudgetMB = static_cast<int>(cacheDefaults.thumbnailBudgetBytes / mib);
    cacheShaderBudgetMB = static_cast<int>(cacheDefaults.shaderBudgetBytes / mib);
    cacheSubtitleBudgetMB = static_cast<int>(cacheDefaults.subtitleBudgetBytes / mib);
    cacheGenericBudgetMB = static_cast<int>(cacheDefaults.genericBudgetBytes / mib);
    SetGlobalCacheBudgets(GetCacheBudgets());
    ResetShortcuts();
    UpdateThemeColors();
}

void Settings::ResetShortcuts() {
    persistedShortcutFields = JsonValue::Object();
    shortcuts.clear();
    shortcuts["togglePlay"] = { "togglePlay", "Play / Pause Video", VK_SPACE, false, false, false };
    shortcuts["seekForward"] = { "seekForward", "Seek Forward (5s)", VK_RIGHT, false, false, false };
    shortcuts["seekBackward"] = { "seekBackward", "Seek Backward (5s)", VK_LEFT, false, false, false };
    shortcuts["seekForwardLarge"] = { "seekForwardLarge", "Seek Forward (10s)", 'L', false, false, false };
    shortcuts["seekBackwardLarge"] = { "seekBackwardLarge", "Seek Backward (10s)", 'J', false, false, false };
    shortcuts["volumeUp"] = { "volumeUp", "Volume Up", VK_UP, false, false, false };
    shortcuts["volumeDown"] = { "volumeDown", "Volume Down", VK_DOWN, false, false, false };
    shortcuts["toggleMute"] = { "toggleMute", "Mute / Unmute Audio", 'M', false, false, false };
    shortcuts["toggleFullscreen"] = { "toggleFullscreen", "Toggle Fullscreen", 'F', false, false, false };
    shortcuts["cycleSubtitles"] = { "cycleSubtitles", "Cycle Subtitle Tracks", 'S', false, false, false };
    shortcuts["cycleAudio"] = { "cycleAudio", "Cycle Audio Tracks", 'A', false, false, false };
    shortcuts["togglePiP"] = { "togglePiP", "Picture-in-Picture Mode", 'P', false, false, false };
    shortcuts["frameStepForward"] = { "frameStepForward", "Next Video Frame", VK_OEM_PERIOD, false, false, false };
    shortcuts["frameStepBackward"] = { "frameStepBackward", "Previous Video Frame", VK_OEM_COMMA, false, false, false };
    shortcuts["speedUp"] = { "speedUp", "Increase Playback Speed", VK_OEM_6, false, false, false }; // ']'
    shortcuts["speedDown"] = { "speedDown", "Decrease Playback Speed", VK_OEM_4, false, false, false }; // '['
    shortcuts["speedReset"] = { "speedReset", "Reset Playback Speed", VK_BACK, false, false, false };
    shortcuts["playNext"] = { "playNext", "Play Next Video in Queue", 'N', false, false, false };
    shortcuts["playPrevious"] = { "playPrevious", "Play Previous Video in Queue", VK_PRIOR, false, false, false };
    shortcuts["nextChapter"] = { "nextChapter", "Next Chapter", VK_NEXT, false, false, false };
    shortcuts["previousChapter"] = { "previousChapter", "Previous Chapter", 'P', false, true, false }; // Shift + P
    shortcuts["restartVideo"] = { "restartVideo", "Restart Video", 'R', false, false, false };
    shortcuts["cycleAspectRatio"] = { "cycleAspectRatio", "Cycle Aspect Ratio", 'X', false, false, false };
    shortcuts["audioDelayPlus"] = { "audioDelayPlus", "Increase Audio Delay (+50ms)", VK_RIGHT, true, false, false };
    shortcuts["audioDelayMinus"] = { "audioDelayMinus", "Decrease Audio Delay (-50ms)", VK_LEFT, true, false, false };
    shortcuts["audioDelayReset"] = { "audioDelayReset", "Reset Audio Delay", VK_BACK, true, false, false };
    shortcuts["subDelayPlus"] = { "subDelayPlus", "Increase Subtitle Delay (+100ms)", VK_RIGHT, true, true, false };
    shortcuts["subDelayMinus"] = { "subDelayMinus", "Decrease Subtitle Delay (-100ms)", VK_LEFT, true, true, false };
    shortcuts["subDelayReset"] = { "subDelayReset", "Reset Subtitle Delay", VK_BACK, true, true, false };
    shortcuts["togglePlaybackDiagnostics"] = { "togglePlaybackDiagnostics", "Toggle Playback Diagnostics", 'I', false, false, false };
    shortcuts["takeScreenshot"] = { "takeScreenshot", "Take Screenshot", 'S', true, false, false };
    shortcuts["togglePrivateMode"] = { "togglePrivateMode", "Toggle Private Session Mode", 0, false, false, false };
    shortcuts["closePlayer"] = { "closePlayer", "Exit Player", VK_ESCAPE, false, false, false };
    shortcuts["jumpToTime"] = { "jumpToTime", "Go To Time", 'T', true, false, false }; // Ctrl + T
}

const std::vector<std::pair<std::string, std::string>>& Settings::GetShortcutActionListRef() const {
    // Display names come from the active locale catalog (with static
    // binding-name fallbacks), so the cached list is keyed on the locale tag.
    const std::string locale = Localization::Instance().GetCurrentLocale();
    if (shortcutActionListCacheLocale != locale || shortcutActionListCache.empty()) {
        const std::vector<std::string> actionIds = {
            "togglePlay",      "seekForward",       "seekBackward",     "seekForwardLarge", "seekBackwardLarge", "volumeUp",
            "volumeDown",      "toggleMute",        "toggleFullscreen", "togglePiP",        "cycleSubtitles",    "cycleAudio",
            "speedUp",         "speedDown",         "speedReset",       "frameStepForward", "frameStepBackward", "playNext",
            "playPrevious",    "nextChapter",       "previousChapter",  "restartVideo",     "cycleAspectRatio",  "audioDelayPlus",
            "audioDelayMinus", "audioDelayReset",   "subDelayPlus",     "subDelayMinus",    "subDelayReset",     "togglePlaybackDiagnostics",
            "takeScreenshot",  "togglePrivateMode", "toggleShuffle",    "toggleAlwaysOnTop", "jumpToTime",       "closePlayer"};
        std::vector<std::pair<std::string, std::string>> list;
        list.reserve(actionIds.size());
        for (const auto& id : actionIds) {
            std::string locKey = "shortcut." + id;
            std::string displayName = Localization::Instance().GetUtf8(locKey);
            if (displayName == locKey) { auto it = shortcuts.find(id); displayName = (it != shortcuts.end()) ? it->second.displayName : id; }
            list.emplace_back(id, displayName);
        }
        shortcutActionListCache = std::move(list);
        shortcutActionListCacheLocale = locale;
    }
    return shortcutActionListCache;
}

std::wstring Settings::FormatShortcutKey(const ShortcutBinding& b) {
    std::wstring result;
    if (b.ctrl) result += L"Ctrl + ";
    if (b.alt) result += L"Alt + ";
    if (b.shift) result += L"Shift + ";
    if (b.vkCode >= 'A' && b.vkCode <= 'Z') { result += (wchar_t)b.vkCode; } else if (b.vkCode >= '0' && b.vkCode <= '9') {
        result += (wchar_t)b.vkCode;
    } else if (b.vkCode >= VK_F1 && b.vkCode <= VK_F12) { result += L"F" + std::to_wstring(b.vkCode - VK_F1 + 1); } else {
        switch (b.vkCode) {
            case VK_SPACE: result += L"Space"; break;
            // Mouse-button/wheel pseudo-keys (see CommandDispatcher.h). Modifier
            // prefixes render normally; only Shift/Ctrl/Alt held at click time apply.
            case 0x100: result += L"Middle Click"; break;
            case 0x101: result += L"X Button 1"; break;
            case 0x102: result += L"X Button 2"; break;
            case 0x103: result += L"Wheel Up"; break;
            case 0x104: result += L"Wheel Down"; break;
            case VK_LEFT: result += L"Left Arrow"; break;
            case VK_RIGHT: result += L"Right Arrow"; break;
            case VK_UP: result += L"Up Arrow"; break;
            case VK_DOWN: result += L"Down Arrow"; break;
            case VK_RETURN: result += L"Enter"; break;
            case VK_ESCAPE: result += L"Escape"; break;
            case VK_BACK: result += L"Backspace"; break;
            case VK_TAB: result += L"Tab"; break;
            case VK_DELETE: result += L"Delete"; break;
            case VK_HOME: result += L"Home"; break;
            case VK_END: result += L"End"; break;
            case VK_PRIOR: result += L"Page Up"; break;
            case VK_NEXT: result += L"Page Down"; break;
            case VK_OEM_PERIOD: result += L"."; break;
            case VK_OEM_COMMA: result += L","; break;
            case VK_OEM_4: result += L"["; break;
            case VK_OEM_6: result += L"]"; break;
            case VK_OEM_1: result += L";"; break;
            case VK_OEM_7: result += L"'"; break;
            case VK_OEM_2: result += L"/"; break;
            case VK_OEM_5: result += L"\\"; break;
            case VK_OEM_MINUS: result += L"-"; break;
            case VK_OEM_PLUS: result += L"+"; break;
            default:
                if (b.vkCode != 0) {
                    wchar_t buf[16];
                    swprintf_s(buf, L"Key 0x%02X", b.vkCode);
                    result += buf;
                } else { result = L"Unassigned"; }
                break;
        }
    }
    return result;
}

std::string Settings::VKToString(UINT vkCode) {
    if (vkCode >= 'A' && vkCode <= 'Z') return std::string(1, (char)vkCode);
    if (vkCode >= '0' && vkCode <= '9') return std::string(1, (char)vkCode);
    switch (vkCode) {
        case VK_SPACE: return "Space";
        case 0x100: return "MiddleClick";
        case 0x101: return "XButton1";
        case 0x102: return "XButton2";
        case 0x103: return "WheelUp";
        case 0x104: return "WheelDown";
        case VK_LEFT: return "ArrowLeft";
        case VK_RIGHT: return "ArrowRight";
        case VK_UP: return "ArrowUp";
        case VK_DOWN: return "ArrowDown";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Escape";
        case VK_BACK: return "Backspace";
        case VK_TAB: return "Tab";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_OEM_PERIOD: return "Period";
        case VK_OEM_COMMA: return "Comma";
        case VK_OEM_4: return "BracketLeft";
        case VK_OEM_6: return "BracketRight";
        default: return std::to_string(vkCode);
    }
}

UINT Settings::StringToVK(const std::string& str) {
    if (str.length() == 1) {
        char c = (char)toupper(str[0]);
        if (c >= 'A' && c <= 'Z') return (UINT)c;
        if (c >= '0' && c <= '9') return (UINT)c;
        if (c == '.') return VK_OEM_PERIOD;
        if (c == ',') return VK_OEM_COMMA;
        if (c == '[') return VK_OEM_4;
        if (c == ']') return VK_OEM_6;
    }
    if (str == "Space" || str == "VK_SPACE") return VK_SPACE;
    // Mouse-button/wheel pseudo-keys: persisted ids and display forms both parse
    // back to the shared 0x100..0x104 range (see CommandDispatcher.h).
    if (str == "MiddleClick" || str == "Middle Click") return 0x100;
    if (str == "XButton1" || str == "X Button 1") return 0x101;
    if (str == "XButton2" || str == "X Button 2") return 0x102;
    if (str == "WheelUp" || str == "Wheel Up") return 0x103;
    if (str == "WheelDown" || str == "Wheel Down") return 0x104;
    if (str == "ArrowLeft" || str == "Left" || str == "VK_LEFT") return VK_LEFT;
    if (str == "ArrowRight" || str == "Right" || str == "VK_RIGHT") return VK_RIGHT;
    if (str == "ArrowUp" || str == "Up" || str == "VK_UP") return VK_UP;
    if (str == "ArrowDown" || str == "Down" || str == "VK_DOWN") return VK_DOWN;
    if (str == "Enter" || str == "Return" || str == "VK_RETURN") return VK_RETURN;
    if (str == "Escape" || str == "Esc" || str == "VK_ESCAPE") return VK_ESCAPE;
    if (str == "Backspace" || str == "VK_BACK") return VK_BACK;
    if (str == "Tab" || str == "VK_TAB") return VK_TAB;
    if (str == "Delete" || str == "Del" || str == "VK_DELETE") return VK_DELETE;
    if (str == "Home" || str == "VK_HOME") return VK_HOME;
    if (str == "End" || str == "VK_END") return VK_END;
    if (str == "PageUp" || str == "VK_PRIOR") return VK_PRIOR;
    if (str == "PageDown" || str == "VK_NEXT") return VK_NEXT;
    if (str == "Period" || str == "VK_OEM_PERIOD") return VK_OEM_PERIOD;
    if (str == "Comma" || str == "VK_OEM_COMMA") return VK_OEM_COMMA;
    if (str == "BracketLeft" || str == "VK_OEM_4") return VK_OEM_4;
    if (str == "BracketRight" || str == "VK_OEM_6") return VK_OEM_6;
    try { return (UINT)std::stoi(str); } catch (...) { return 0; }
}

const std::vector<ThemePalette>& Settings::GetAllThemes() const {
    static const auto themes = [] {
        std::vector<ThemePalette> list;
        // Neutral, tool-grade palettes: near-neutral dark surfaces, one restrained accent, plain names. Radii stay small (8/4/6).
        // 1. Dark (Default)
        list.push_back({
            "default", "Dark", HexToD2DColor("#1b1b1d"), HexToD2DColor("#252527"), HexToD2DColor("#2e2e31"),
            HexToD2DColor("#323236"), HexToD2DColor("#252527"), HexToD2DColor("#6b9bd2"), HexToD2DColor("#5a8cc2"), HexToD2DColor("#6b9bd2", 0.16f),
            HexToD2DColor("#e8e8ea"), HexToD2DColor("#9c9ca3"), HexToD2DColor("#e05d68"), {}, {}, {}, {}, {}, {}, {}, {}, 8.0f, 4.0f, 6.0f
        });
        // 2. Black (OLED)
        list.push_back({
            "oled", "Black (OLED)", HexToD2DColor("#000000"), HexToD2DColor("#0e0e10"), HexToD2DColor("#17171a"),
            HexToD2DColor("#232327"), HexToD2DColor("#0e0e10"), HexToD2DColor("#6b9bd2"), HexToD2DColor("#5a8cc2"), HexToD2DColor("#6b9bd2", 0.16f),
            HexToD2DColor("#e8e8ea"), HexToD2DColor("#8f8f96"), HexToD2DColor("#e05d68"), {}, {}, {}, {}, {}, {}, {}, {}, 8.0f, 4.0f, 6.0f
        });
        // 3. Slate
        list.push_back({
            "slate", "Slate", HexToD2DColor("#1e2228"), HexToD2DColor("#272c33"), HexToD2DColor("#313841"),
            HexToD2DColor("#384049"), HexToD2DColor("#272c33"), HexToD2DColor("#8fb0cf"), HexToD2DColor("#7ba0bf"), HexToD2DColor("#8fb0cf", 0.16f),
            HexToD2DColor("#e6e9ed"), HexToD2DColor("#98a2ad"), HexToD2DColor("#e05d68"), {}, {}, {}, {}, {}, {}, {}, {}, 8.0f, 4.0f, 6.0f
        });
        for (auto& th : list) {
            th.surfaceElevated = th.cardBg;
            th.surfaceFloating = HexToD2DColor(th.id == "oled" ? "#08080a" : "#1e1e21", 0.98f);
            th.surfaceModal = th.cardBg;
            th.scrim = D2D1::ColorF(0.0f, 0.0f, 0.0f, th.id == "oled" ? 0.80f : 0.65f);
            th.focusRing = th.accent;
            th.borderSubtle = D2D1::ColorF(th.border.r, th.border.g, th.border.b, std::min(th.border.a, 0.15f));
            // Hover/selection are neutral highlights, not accent tints; accent is
            // reserved for progress, active controls, and the focus ring.
            th.surfaceHover = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f);
            th.surfaceSelected = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f);
        }
        return list;
    }();
    return themes;
}
std::vector<std::string> Settings::GetAvailableFonts() const {
    return { "Inter", "Space Grotesk", "Segoe UI Variable", "JetBrains Mono", "Aptos", "Roboto", "Segoe UI" };
}
std::vector<LocaleInfo> Settings::GetAvailableLanguages() const { return Localization::Instance().GetAvailableLocales(); }
ThemePalette Settings::GetThemePalette(const std::string& id) const {
    const auto& all = GetAllThemes();
    for (const auto& th : all) { if (th.id == id) return th; }
    return all[0];
}

void Settings::UpdateThemeColors() {
    currentTheme = GetThemePalette(themePreset);
    if (!accentColorHex.empty()) {
        // A custom accent only retints accent roles; hover/selection stay
        // neutral so selection states never read as decoration.
        currentTheme.accent = HexToD2DColor(accentColorHex);
        currentTheme.accentDark = currentTheme.accent;
        currentTheme.accentSubtle = HexToD2DColor(accentColorHex, 0.16f);
        currentTheme.focusRing = currentTheme.accent;
    }
}

float Settings::GetSubFontSizePoints() const {
    std::string value = LowerAscii(TrimAscii(subFontSize));
    if (value.size() > 2 && value.compare(value.size() - 2, 2, "px") == 0) { value.resize(value.size() - 2); }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end && *end == '\0' && parsed >= 14 && parsed <= 32) { return static_cast<float>(parsed); }
    return 20.0f;
}

D2D1_COLOR_F Settings::GetSubTextColor() const { return HexToD2DColor(subColorHex); }

bool Settings::MigrateSchema(JsonValue& root, int fromVersion, int targetVersion) {
    if (!root.is_object()) return false;
    if (fromVersion >= targetVersion) return true;
    for (int v = fromVersion; v < targetVersion; ++v) {
        if (v == 0) {
            // Stage 0 -> 1: Unversioned legacy to Schema 1
            if (!root.has_key("themePreset")) root["themePreset"] = "default";
            if (!root.has_key("accentColor")) root["accentColor"] = "";
            if (!root.has_key("fontFamily")) root["fontFamily"] = "Inter";
            if (!root.has_key("autoplay")) root["autoplay"] = true;
            if (!root.has_key("rememberPos")) root["rememberPos"] = true;
            if (!root.has_key("volume")) root["volume"] = 1.0;
            root["schemaVersion"] = 1;
        } else if (v == 1) {
            // Stage 1 -> 2: Standardize shortcuts and audio/subtitle tracks
            if (!root.has_key("audioLang")) root["audioLang"] = "eng";
            if (!root.has_key("subLang")) root["subLang"] = "eng";
            if (!root.has_key("subtitlesEnabled")) root["subtitlesEnabled"] = true;
            if (!root.has_key("subFontSize")) root["subFontSize"] = "20px";
            if (!root.has_key("subColor")) root["subColor"] = "#cad3f5";
            if (root.has_key("shortcuts") && root.get("shortcuts").is_object()) {
                JsonValue& sc = root["shortcuts"];
                for (auto& pair : sc.as_object()) {
                    if (pair.second.is_string() || pair.second.is_number()) {
                        JsonValue obj = JsonValue::Object();
                        UINT vk = pair.second.is_number() ? (UINT)pair.second.as_int() : StringToVK(pair.second.as_string());
                        obj["vk"] = (int)vk;
                        obj["key"] = VKToString(vk);
                        obj["ctrl"] = false;
                        obj["shift"] = false;
                        obj["alt"] = false;
                        obj["name"] = pair.first;
                        pair.second = obj;
                    }
                }
            }
            root["schemaVersion"] = 2;
        } else if (v == 2) {
            // Stage 2 -> 3: Storage, playlist view mode, and window state
            if (!root.has_key("storageMode")) root["storageMode"] = "source";
            if (!root.has_key("playlistViewMode")) root["playlistViewMode"] = "list";
            if (!root.has_key("libraryGroupMode")) root["libraryGroupMode"] = "flat";
            if (!root.has_key("windowWidth")) root["windowWidth"] = 1280;
            if (!root.has_key("windowHeight")) root["windowHeight"] = 780;
            if (!root.has_key("windowX")) root["windowX"] = -1;
            if (!root.has_key("windowY")) root["windowY"] = -1;
            if (!root.has_key("windowMaximized")) root["windowMaximized"] = false;
            root["schemaVersion"] = 3;
        } else if (v == 3) {
            // Stage 3 -> 4: Track titles and theme validation
            if (!root.has_key("lastAudioTrackTitle")) root["lastAudioTrackTitle"] = "";
            if (!root.has_key("lastSubtitleTrackTitle")) root["lastSubtitleTrackTitle"] = "";
            if (!root.has_key("subBgStyle")) root["subBgStyle"] = "vlc-outline";
            if (!root.has_key("subPosition")) root["subPosition"] = "bottom";
            root["schemaVersion"] = 4;
        } else if (v == 4) {
            // Stage 4 -> 5: Playback session restoration
            if (!root.has_key("restoreSession")) root["restoreSession"] = true;
            root["schemaVersion"] = 5;
        } else if (v == 5) {
            // Stage 5 -> 6: Cache budgets (normalized 160 total, see plan 5.4)
            if (!root.has_key("cacheTotalBudgetMB")) root["cacheTotalBudgetMB"] = 160;
            if (!root.has_key("cacheThumbnailBudgetMB")) root["cacheThumbnailBudgetMB"] = 64;
            if (!root.has_key("cacheShaderBudgetMB")) root["cacheShaderBudgetMB"] = 64;
            if (!root.has_key("cacheSubtitleBudgetMB")) root["cacheSubtitleBudgetMB"] = 16;
            if (!root.has_key("cacheGenericBudgetMB")) root["cacheGenericBudgetMB"] = 16;
            root["schemaVersion"] = 6;
        } else if (v == 6) {
            // Stage 6 -> 7: Color management and HDR controls
            if (!root.has_key("hdrMode")) root["hdrMode"] = "auto";
            if (!root.has_key("toneMappingAlgorithm")) root["toneMappingAlgorithm"] = "auto";
            if (!root.has_key("targetPeak")) root["targetPeak"] = 0;
            if (!root.has_key("gamutMapping")) root["gamutMapping"] = "auto";
            if (!root.has_key("iccProfilePath")) root["iccProfilePath"] = "";
            root["schemaVersion"] = 7;
        } else if (v == 7) {
            // Stage 7 -> 8: Surround audio, device selection, night mode, balance and channel map
            if (!root.has_key("audioDevice")) root["audioDevice"] = "auto";
            if (!root.has_key("audioLayout")) root["audioLayout"] = "auto-safe";
            if (!root.has_key("audioChannelMix")) root["audioChannelMix"] = "auto";
            if (!root.has_key("audioNormalizeDownmix")) root["audioNormalizeDownmix"] = true;
            if (!root.has_key("nightMode")) root["nightMode"] = "off";
            if (!root.has_key("audioBalance")) root["audioBalance"] = 0.0;
            if (!root.has_key("audioChannelMap")) root["audioChannelMap"] = "auto";
            root["schemaVersion"] = 8;
        } else if (v == 8) {
            // Stage 8 -> 9: Subtitle style profiles, active profile, and safe area controls
            if (!root.has_key("subtitleProfiles")) root["subtitleProfiles"] = JsonValue::Array();
            if (!root.has_key("activeSubtitleProfile")) root["activeSubtitleProfile"] = "";
            if (!root.has_key("subSafeAreaX")) root["subSafeAreaX"] = 0;
            if (!root.has_key("subSafeAreaY")) root["subSafeAreaY"] = 0;
            root["schemaVersion"] = 9;
        } else if (v == 9) {
            // Stage 9 -> 10: Online subtitle retrieval opt-in and endpoint
            if (!root.has_key("onlineSubtitlesEnabled")) root["onlineSubtitlesEnabled"] = false;
            if (!root.has_key("onlineSubtitleEndpoint")) root["onlineSubtitleEndpoint"] = "";
            root["schemaVersion"] = 10;
        } else if (v == 10) {
            // Stage 10 -> 11: Custom MPV configuration opt-in and path
            if (!root.has_key("customMpvConfigEnabled")) root["customMpvConfigEnabled"] = false;
            if (!root.has_key("customMpvConfigPath")) root["customMpvConfigPath"] = "mpv.conf";
            root["schemaVersion"] = 11;
        } else if (v == 11) {
            if (!root.has_key("jumpListEnabled")) root["jumpListEnabled"] = false;
            root["schemaVersion"] = 12;
        } else if (v == 12) {
            if (!root.has_key("hoverPreviewEnabled")) root["hoverPreviewEnabled"] = true;
            if (!root.has_key("reduceMotionEnabled")) root["reduceMotionEnabled"] = false;
            root["schemaVersion"] = 13;
        } else if (v == 13) {
            if (!root.has_key("autoUpdateCheckEnabled")) root["autoUpdateCheckEnabled"] = false;
            if (!root.has_key("updateChannel")) root["updateChannel"] = "stable";
            if (!root.has_key("updateCheckUrl")) root["updateCheckUrl"] = "";
            root["schemaVersion"] = 14;
        } else if (v == 14) {
            if (!root.has_key("appLanguage")) {
                if (root.has_key("locale") && root["locale"].is_string()) {
                    root["appLanguage"] = root["locale"].as_string();
                } else { root["appLanguage"] = "en-US"; }
            }
            root["schemaVersion"] = 15;
        } else if (v == 15) {
            if (!root.has_key("experienceMode")) root["experienceMode"] = "simple";
            if (!root.has_key("customEnabledFeatures")) root["customEnabledFeatures"] = JsonValue::Array();
            root["schemaVersion"] = 16;
        }
    }
    return true;
}

StorageStatus Settings::Load() {
    ResetToDefaults();
    recoveredFromBackup = false;
    loadedSchemaVersion = 0;
    savesAllowed = false;
    const auto rootStatus = CheckPortableRootAvailability(GetExeDir());
    if (!rootStatus.is_ok()) { UpdateThemeColors(); lastLoadStatus = rootStatus; return lastLoadStatus; }
    std::string jsonStr;
    const std::wstring path = GetDataDir() + L"\\settings.json";
    savesAllowed = false;
    DWORD pathError = ERROR_SUCCESS;
    const auto availability = QueryPathAvailability(path, &pathError);
    if (availability == PathAvailability::Missing) { savesAllowed = true; UpdateThemeColors(); lastLoadStatus = StorageStatus::Ok(path); return lastLoadStatus; }
    if (availability == PathAvailability::Unavailable) {
        UpdateThemeColors();
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::FileReadFailed, pathError, path, "Settings file is unavailable");
        return lastLoadStatus;
    }
    StorageStatus readStatus = ReadTextFile(path, jsonStr);
    JsonValue root;
    const bool parsed = readStatus.is_ok() && JsonValue::parse(jsonStr, root);
    if (parsed && HasFutureStoreSchema(root, kCurrentSettingsSchemaVersion)) {
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, path, "Settings require a newer application version");
        return lastLoadStatus;
    }
    bool parseOk = parsed && HasSupportedStoreSchema(root, kCurrentSettingsSchemaVersion);
    if (!parseOk) {
        // Attempt recovery from rotating backup
        std::string backupText;
        StorageStatus backupStatus = RestoreFromNewestValidBackup(path, backupText, [](const std::string& t) {
            JsonValue j;
            return JsonValue::parse(t, j) && HasSupportedStoreSchema(j, kCurrentSettingsSchemaVersion);
        });
        if (backupStatus.is_ok() && JsonValue::parse(backupText, root) && root.is_object()) {
            recoveredFromBackup = true;
            parseOk = true;
        } else {
            UpdateThemeColors();
            lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_INVALID_DATA, path, "Malformed JSON in settings.json and no valid backup available");
            return lastLoadStatus;
        }
    }
    int fileVersion = root.get("schemaVersion").as_int(0);
    if (fileVersion > kCurrentSettingsSchemaVersion) {
        lastLoadStatus = StorageStatus::Error(StorageErrorCode::SerializationFailed, ERROR_REVISION_MISMATCH, path, "Settings require a newer application version");
        return lastLoadStatus;
    }
    loadedSchemaVersion = fileVersion;
    if (fileVersion < kCurrentSettingsSchemaVersion) { MigrateSchema(root, fileVersion, kCurrentSettingsSchemaVersion); }
    persistedFields = root;
    themePreset = root.get("themePreset").as_string(themePreset);
    accentColorHex = root.get("accentColor").as_string(accentColorHex);
    uiFontFamily = root.get("fontFamily").as_string(uiFontFamily);
    appLanguage = root.get("appLanguage").as_string(root.get("locale").as_string(appLanguage));
    autoplay = root.get("autoplay").as_bool(autoplay);
    rememberPos = root.get("rememberPos").as_bool(rememberPos);
    restoreSession = root.get("restoreSession").as_bool(restoreSession);
    loop = root.get("loop").as_bool(loop);
    shuffleQueue = root.get("shuffleQueue").as_bool(shuffleQueue);
    alwaysOnTop = root.get("alwaysOnTop").as_bool(alwaysOnTop);
    hwAccel = root.get("hwAccel").as_bool(hwAccel);
    defaultSpeed = Number(root.get("defaultSpeed"), defaultSpeed);
    arrowSeekStepSec = Number(root.get("arrowSeekStep"), arrowSeekStepSec);
    largeSeekStepSec = Number(root.get("largeSeekStep"), largeSeekStepSec);
    ctrlSeekFrames = Integer(root.get("ctrlSeekFrames"), ctrlSeekFrames);
    customMpvConfigEnabled = root.get("customMpvConfigEnabled").as_bool(customMpvConfigEnabled);
    customMpvConfigPath = NormalizeMpvConfigPath(root.get("customMpvConfigPath").as_string(customMpvConfigPath));
    globalHotkeysEnabled = root.get("globalHotkeysEnabled").as_bool(globalHotkeysEnabled);
    trayIconEnabled = root.get("trayIconEnabled").as_bool(trayIconEnabled);
    minimizeToTray = root.get("minimizeToTray").as_bool(minimizeToTray);
    screenshotFormat = root.get("screenshotFormat").as_string(screenshotFormat);
    jumpListEnabled = root.get("jumpListEnabled").as_bool(jumpListEnabled);
    hoverPreviewEnabled = root.get("hoverPreviewEnabled").as_bool(hoverPreviewEnabled);
    reduceMotionEnabled = root.get("reduceMotionEnabled").as_bool(reduceMotionEnabled);
    hdrMode = root.get("hdrMode").as_string(hdrMode);
    toneMappingAlgorithm = root.get("toneMappingAlgorithm").as_string(toneMappingAlgorithm);
    targetPeak = Integer(root.get("targetPeak"), targetPeak);
    gamutMapping = root.get("gamutMapping").as_string(gamutMapping);
    iccProfilePath = root.get("iccProfilePath").as_string(iccProfilePath);
    audioLang = root.get("audioLang").as_string(audioLang);
    subLang = root.get("subLang").as_string(subLang);
    lastAudioTrackTitle = root.get("lastAudioTrackTitle").as_string(lastAudioTrackTitle);
    lastSubtitleTrackTitle = root.get("lastSubtitleTrackTitle").as_string(lastSubtitleTrackTitle);
    subtitlesEnabled = root.get("subtitlesEnabled").as_bool(subtitlesEnabled);
    // Keep subtitle visibility separate from the preferred language. Older or
    // intermediate builds could store a disable sentinel in subLang; migrate
    // that to the existing boolean so mpv never receives "off"/"no" as a language preference.
    if (_stricmp(subLang.c_str(), "off") == 0 || _stricmp(subLang.c_str(), "no") == 0) { subtitlesEnabled = false; subLang = "eng"; }
    subFontSize = root.get("subFontSize").as_string(subFontSize);
    subColorHex = root.get("subColor").as_string(subColorHex);
    // Keep the older saturated subtitle palette readable while moving the
    // primary controls to the softer colors used by the native reference UI.
    if (_stricmp(subColorHex.c_str(), "#ffffff") == 0) subColorHex = "#cad3f5";
    else if (_stricmp(subColorHex.c_str(), "#ffff00") == 0) subColorHex = "#eed49f";
    else if (_stricmp(subColorHex.c_str(), "#00ff00") == 0) subColorHex = "#a6da95";
    else if (_stricmp(subColorHex.c_str(), "#00ffff") == 0) subColorHex = "#91d7e3";
    else if (_stricmp(subColorHex.c_str(), "#f59e0b") == 0) subColorHex = "#f5a97f";
    else if (_stricmp(subColorHex.c_str(), "#f43f5e") == 0) subColorHex = "#ed8796";
    subBgStyle = root.get("subBgStyle").as_string(subBgStyle);
    subPosition = root.get("subPosition").as_string(subPosition);
    subSafeAreaX = NormalizeSafeAreaPercent(Integer(root.get("subSafeAreaX"), Integer(root.get("safeAreaX"), Integer(root.get("subMarginXPercent"), subSafeAreaX))));
    subSafeAreaY = NormalizeSafeAreaPercent(Integer(root.get("subSafeAreaY"), Integer(root.get("safeAreaY"), Integer(root.get("subMarginYPercent"), subSafeAreaY))));
    subForcedOnly = root.get("subForcedOnly").as_bool(subForcedOnly);
    subCodepage = root.get("subCodepage").as_string(subCodepage);
    subFontFamily = root.get("subFontFamily").as_string(subFontFamily);
    audioDelayMs = Integer(root.get("audioDelayMs"), audioDelayMs);
    subDelayMs = Integer(root.get("subDelayMs"), subDelayMs);
    secondarySubLang = root.get("secondarySubLang").as_string(secondarySubLang);
    lastSecondarySubtitleTrackTitle = root.get("lastSecondarySubtitleTrackTitle").as_string(lastSecondarySubtitleTrackTitle);
    secondarySubtitlesEnabled = root.get("secondarySubtitlesEnabled").as_bool(secondarySubtitlesEnabled);
    secondarySubFontSize = root.get("secondarySubFontSize").as_string(secondarySubFontSize);
    secondarySubColorHex = root.get("secondarySubColor").as_string(secondarySubColorHex);
    secondarySubPosition = root.get("secondarySubPosition").as_string(secondarySubPosition);
    secondarySubDelayMs = Integer(root.get("secondarySubDelayMs"), secondarySubDelayMs);
    onlineSubtitlesEnabled = root.get("onlineSubtitlesEnabled").as_bool(onlineSubtitlesEnabled);
    onlineSubtitleEndpoint = root.get("onlineSubtitleEndpoint").as_string(onlineSubtitleEndpoint);
    activeSubtitleProfile = root.get("activeSubtitleProfile").as_string("");
    subtitleProfiles.clear();
    if (root.has_key("subtitleProfiles") && root.get("subtitleProfiles").is_array()) {
        const auto& arr = root.get("subtitleProfiles").as_array();
        for (const auto& item : arr) {
            if (item.is_object()) {
                std::string rawName = item.get("name").as_string("");
                std::string cleanName = SanitizeProfileName(rawName);
                if (IsValidProfileName(cleanName)) {
                    SubtitleStyleProfile prof;
                    prof.name = cleanName;
                    std::string sz = item.has_key("fontSize") ? item.get("fontSize").as_string("") : item.get("subFontSize").as_string("");
                    prof.fontSize = NormalizeProfileFontSize(sz);
                    std::string col = item.has_key("color") ? item.get("color").as_string("") : (item.has_key("colorHex") ? item.get("colorHex").as_string("") : item.get("subColor").as_string(""));
                    prof.colorHex = NormalizeProfileColor(col);
                    std::string bg = item.has_key("bgStyle") ? item.get("bgStyle").as_string("") : item.get("subBgStyle").as_string("");
                    prof.bgStyle = NormalizeProfileBgStyle(bg);
                    std::string pos = item.has_key("position") ? item.get("position").as_string("") : item.get("subPosition").as_string("");
                    prof.position = NormalizeProfilePosition(pos);
                    int saX = item.has_key("safeAreaX") ? item.get("safeAreaX").as_int(0) : (item.has_key("subSafeAreaX") ? item.get("subSafeAreaX").as_int(0) : 0);
                    int saY = item.has_key("safeAreaY") ? item.get("safeAreaY").as_int(0) : (item.has_key("subSafeAreaY") ? item.get("subSafeAreaY").as_int(0) : 0);
                    prof.safeAreaX = NormalizeSafeAreaPercent(saX);
                    prof.safeAreaY = NormalizeSafeAreaPercent(saY);
                    for (const auto& kv : item.as_object()) {
                        const std::string& k = kv.first;
                        if (k != "name" && k != "fontSize" && k != "subFontSize" && k != "color" && k != "colorHex" && k != "subColor" && k != "bgStyle" && k != "subBgStyle" && k != "position" && k != "subPosition" && k != "safeAreaX" && k != "safeAreaY" && k != "subSafeAreaX" && k != "subSafeAreaY")
                            prof.unknownFields[k] = kv.second;
                    }
                    AddOrUpdateSubtitleProfile(prof);
                }
            }
        }
    }
    audioDevice = root.get("audioDevice").as_string(audioDevice);
    audioLayout = root.get("audioLayout").as_string(audioLayout);
    audioChannelMix = root.get("audioChannelMix").as_string(audioChannelMix);
    audioNormalizeDownmix = root.get("audioNormalizeDownmix").as_bool(audioNormalizeDownmix);
    nightMode = root.get("nightMode").as_string(nightMode);
    audioBalance = Number(root.get("audioBalance"), audioBalance);
    audioChannelMap = root.get("audioChannelMap").as_string(audioChannelMap);
    replayGainMode = root.get("replayGainMode").as_string(replayGainMode);
    replayGainPreampDb = Number(root.get("replayGainPreampDb"), replayGainPreampDb);
    deinterlaceMode = root.get("deinterlaceMode").as_string(deinterlaceMode);
    audioPassthrough = root.get("audioPassthrough").as_bool(audioPassthrough);
    pitchCorrection = root.get("pitchCorrection").as_bool(pitchCorrection);
    storageMode = root.get("storageMode").as_string(storageMode);
    playlistViewMode = root.get("playlistViewMode").as_string(playlistViewMode);
    libraryGroupMode = root.get("libraryGroupMode").as_string(libraryGroupMode);
    librarySortField = root.get("librarySortField").as_string(librarySortField);
    librarySortDirection = root.get("librarySortDirection").as_string(librarySortDirection);
    customFolderOrder.clear();
    manualMediaOrder = root.get("manualMediaOrder").as_bool(false);
    if (root.has_key("customFolderOrder") && root.get("customFolderOrder").is_array()) {
        for (const auto& item : root.get("customFolderOrder").as_array()) {
            if (item.is_string()) { customFolderOrder.push_back(Utf8ToUtf16(item.as_string())); }
        }
    }
    experienceMode = root.get("experienceMode").as_string(experienceMode);
    experienceMode = FeatureGate::NormalizeModeString(experienceMode);
    customEnabledFeatures.clear();
    if (root.has_key("customEnabledFeatures") && root.get("customEnabledFeatures").is_array()) {
        for (const auto& item : root.get("customEnabledFeatures").as_array()) {
            if (item.is_string()) customEnabledFeatures.push_back(item.as_string());
        }
    }
    customEnabledFeatures = FeatureGate::SanitizeFeatureKeys(customEnabledFeatures);
    diagnosticsLogging = root.get("diagnosticsLogging").as_bool(diagnosticsLogging);
    smartSeriesDetection = root.get("smartSeriesDetection").as_bool(false);
    autoUpdateCheckEnabled = root.get("autoUpdateCheckEnabled").as_bool(autoUpdateCheckEnabled);
    std::string rawChannel = root.get("updateChannel").as_string(updateChannel);
    updateChannel = ChannelPolicy::Instance().CanonicalizeChannel(rawChannel);
    updateCheckUrl = root.get("updateCheckUrl").as_string(updateCheckUrl);
    muted = root.get("muted").as_bool(muted);
    volume = root.get("volume").as_number(volume);
    lastAspectRatio = root.get("lastAspectRatio").as_string(lastAspectRatio);
    videoRotate = Integer(root.get("videoRotate"), videoRotate);
    brightness = Integer(root.get("brightness"), brightness);
    contrast = Integer(root.get("contrast"), contrast);
    saturation = Integer(root.get("saturation"), saturation);
    gamma = Integer(root.get("gamma"), gamma);
    hue = Integer(root.get("hue"), hue);
    windowX = Integer(root.get("windowX"), windowX);
    windowY = Integer(root.get("windowY"), windowY);
    windowWidth = Integer(root.get("windowWidth"), windowWidth);
    windowHeight = Integer(root.get("windowHeight"), windowHeight);
    windowMaximized = root.get("windowMaximized").as_bool(windowMaximized);
    // Legacy -1/-1 was the unset sentinel; preserve other negative positions.
    hasWindowPlacement = root.get("hasWindowPlacement").as_bool(windowX != -1 || windowY != -1);
    pipX = Integer(root.get("pipX"), pipX);
    pipY = Integer(root.get("pipY"), pipY);
    pipWidth = Integer(root.get("pipWidth"), pipWidth);
    pipHeight = Integer(root.get("pipHeight"), pipHeight);
    cacheTotalBudgetMB = Integer(root.get("cacheTotalBudgetMB"), cacheTotalBudgetMB);
    cacheThumbnailBudgetMB = Integer(root.get("cacheThumbnailBudgetMB"), cacheThumbnailBudgetMB);
    cacheShaderBudgetMB = Integer(root.get("cacheShaderBudgetMB"), cacheShaderBudgetMB);
    cacheSubtitleBudgetMB = Integer(root.get("cacheSubtitleBudgetMB"), cacheSubtitleBudgetMB);
    cacheGenericBudgetMB = Integer(root.get("cacheGenericBudgetMB"), cacheGenericBudgetMB);
    cacheTotalBudgetMB = std::clamp(cacheTotalBudgetMB, 0, 2048);
    cacheThumbnailBudgetMB = std::clamp(cacheThumbnailBudgetMB, 0, 2048);
    cacheShaderBudgetMB = std::clamp(cacheShaderBudgetMB, 0, 2048);
    cacheSubtitleBudgetMB = std::clamp(cacheSubtitleBudgetMB, 0, 2048);
    cacheGenericBudgetMB = std::clamp(cacheGenericBudgetMB, 0, 2048);
    // Enforce the product-level total: class budgets cannot exceed the total
    // ceiling, and everything stays under the executable's own volume.
    {
        int classSum = cacheThumbnailBudgetMB + cacheShaderBudgetMB + cacheSubtitleBudgetMB + cacheGenericBudgetMB;
        if (cacheTotalBudgetMB > 0 && classSum > cacheTotalBudgetMB) {
            // Scale classes proportionally down to the total instead of silently allowing 4x growth beyond the advertised ceiling.
            double scale = (double)cacheTotalBudgetMB / (double)classSum;
            cacheThumbnailBudgetMB = (int)std::floor(cacheThumbnailBudgetMB * scale);
            cacheShaderBudgetMB = (int)std::floor(cacheShaderBudgetMB * scale);
            cacheSubtitleBudgetMB = (int)std::floor(cacheSubtitleBudgetMB * scale);
            cacheGenericBudgetMB = cacheTotalBudgetMB - cacheThumbnailBudgetMB - cacheShaderBudgetMB - cacheSubtitleBudgetMB;
        }
    }
    SetGlobalCacheBudgets(GetCacheBudgets());
    const auto& themes = GetAllThemes();
    bool validTheme = false;
    for (const auto& t : themes) { if (t.id == themePreset) validTheme = true; }
    if (!validTheme) themePreset = "default";
    if (!IsHexColor(accentColorHex)) accentColorHex = "";
    if (!IsHexColor(subColorHex)) subColorHex = "#cad3f5";
    if (!IsHexColor(secondarySubColorHex)) secondarySubColorHex = "#ffff00";
    bool validFont = false;
    for (const auto& f : GetAvailableFonts()) { if (f == uiFontFamily) validFont = true; }
    if (!validFont) uiFontFamily = "Inter";
    bool validSubFont = false;
    for (const auto& f : GetAvailableFonts()) { if (f == subFontFamily) validSubFont = true; }
    if (!validSubFont) subFontFamily = "Inter";
    {
        // Curated character-set list mirrored by the subtitle context menu; unknown values fall back to auto-detection.
        static const char* kSubCodepages[] = { "auto", "utf-8", "utf-16le", "windows-1252", "windows-1250", "windows-1251",
                                               "windows-1253", "windows-1254", "windows-1255", "windows-1256",
                                               "shift-jis", "big5", "gbk", "euc-kr" };
        bool validCodepage = false;
        for (const char* cp : kSubCodepages) { if (subCodepage == cp) validCodepage = true; }
        if (!validCodepage) subCodepage = "auto";
    }
    if (!std::isfinite(defaultSpeed)) defaultSpeed = 1.0;
    defaultSpeed = std::clamp(defaultSpeed, 0.25, 4.0);
    if (!std::isfinite(arrowSeekStepSec)) arrowSeekStepSec = 5.0;
    arrowSeekStepSec = std::clamp(arrowSeekStepSec, 1.0, 300.0);
    if (!std::isfinite(largeSeekStepSec)) largeSeekStepSec = 10.0;
    largeSeekStepSec = std::clamp(largeSeekStepSec, 1.0, 600.0);
    if (screenshotFormat != "png" && screenshotFormat != "jpg") screenshotFormat = "png";
    ctrlSeekFrames = std::clamp(ctrlSeekFrames, 1, 100);
    hdrMode = NormalizeHdrMode(hdrMode);
    toneMappingAlgorithm = NormalizeToneMapping(toneMappingAlgorithm);
    targetPeak = NormalizeTargetPeak(targetPeak);
    gamutMapping = NormalizeGamutMapping(gamutMapping);
    iccProfilePath = NormalizeIccPath(iccProfilePath, GetExeDir());
    audioBalance = NormalizeAudioBalance(audioBalance);
    audioChannelMap = NormalizeAudioChannelMap(audioChannelMap);
    replayGainMode = NormalizeReplayGainMode(replayGainMode);
    replayGainPreampDb = NormalizeReplayGainPreampDb(replayGainPreampDb);
    deinterlaceMode = NormalizeDeinterlaceMode(deinterlaceMode);
    volume = std::clamp(std::isfinite(volume) ? volume : 1.0, 0.0, kMaxVolumeRatio);
    audioDelayMs = std::clamp(audioDelayMs, -5000, 5000);
    subDelayMs = std::clamp(subDelayMs, -10000, 10000);
    secondarySubDelayMs = std::clamp(secondarySubDelayMs, -600000, 600000);
    if (secondarySubFontSize != "14px" && secondarySubFontSize != "18px" && secondarySubFontSize != "20px" && secondarySubFontSize != "24px" && secondarySubFontSize != "32px") secondarySubFontSize = "18px";
    if (secondarySubPosition != "bottom" && secondarySubPosition != "elevated" && secondarySubPosition != "top") secondarySubPosition = "top";
    storageMode = "source";
    if (playlistViewMode != "list" && playlistViewMode != "grid") playlistViewMode = "list";
    if (libraryGroupMode != "flat" && libraryGroupMode != "folder" && libraryGroupMode != "series" && libraryGroupMode != "duplicates") libraryGroupMode = "flat";
    if (librarySortField != "auto" && librarySortField != "manual" && librarySortField != "title" &&
        librarySortField != "dateAdded" && librarySortField != "lastPlayed" && librarySortField != "duration" &&
        librarySortField != "progress") librarySortField = "auto";
    if (librarySortDirection != "ascending" && librarySortDirection != "descending") librarySortDirection = "ascending";
    subFontSize = NormalizeProfileFontSize(subFontSize);
    if (subBgStyle != "vlc-outline" && subBgStyle != "translucent" && subBgStyle != "solid" && subBgStyle != "clean") subBgStyle = "vlc-outline";
    if (subPosition != "bottom" && subPosition != "elevated" && subPosition != "top") subPosition = "bottom";
    if (lastAspectRatio != "auto" && lastAspectRatio != "16:9" && lastAspectRatio != "4:3" && lastAspectRatio != "21:9" && lastAspectRatio != "fill" && lastAspectRatio != "cover" && !IsValidCustomAspectString(lastAspectRatio)) lastAspectRatio = "auto";
    if (videoRotate < 0 || videoRotate > 359) videoRotate = 0;
    brightness = std::clamp(brightness, -100, 100);
    contrast = std::clamp(contrast, -100, 100);
    saturation = std::clamp(saturation, -100, 100);
    gamma = std::clamp(gamma, -100, 100);
    hue = std::clamp(hue, -100, 100);
    windowWidth = std::clamp(windowWidth, 320, 16384);
    windowHeight = std::clamp(windowHeight, 240, 16384);
    windowX = std::clamp(windowX, -100000, 100000);
    windowY = std::clamp(windowY, -100000, 100000);
    if (pipWidth > 0 && pipHeight > 0) {
        pipWidth = std::clamp(pipWidth, 0, 16384);
        pipHeight = std::clamp(pipHeight, 0, 16384);
        pipX = std::clamp(pipX, -100000, 100000);
        pipY = std::clamp(pipY, -100000, 100000);
    } else {
        pipX = -1;
        pipY = -1;
        pipWidth = 0;
        pipHeight = 0;
    }
    JsonValue scObj = root.get("shortcuts");
    ResetShortcuts();
    if (scObj.is_object()) {
        persistedShortcutFields = scObj;
        for (const auto& kv : scObj.as_object()) {
            const JsonValue& v = kv.second;
            UINT vk = (UINT)Integer(v.is_object() ? v.get("vk") : v, 0);
            std::string key = v.is_object() ? v.get("key").as_string("") : v.as_string("");
            if (!vk && !key.empty()) vk = StringToVK(key);
            if (vk) {
                shortcuts[kv.first] = {
                    kv.first, v.is_object() ? v.get("name").as_string(kv.first) : kv.first, vk,
                    v.is_object() && v.get("ctrl").as_bool(false), v.is_object() && v.get("shift").as_bool(false),
                    v.is_object() && v.get("alt").as_bool(false)
                };
            }
        }
    }
    UpdateThemeColors();
    lastLoadStatus = StorageStatus::Ok(path);
    savesAllowed = true;
    return lastLoadStatus;
}

StorageStatus Settings::Save() {
    if (!savesAllowed) { lastSaveStatus = lastLoadStatus; return lastSaveStatus; }
    StorageStatus writability = CheckDataDirectoryWritability();
    if (!writability.is_ok()) { lastSaveStatus = writability; return lastSaveStatus; }
    JsonValue root = persistedFields.is_object() ? persistedFields : JsonValue::Object();
    root["schemaVersion"] = kCurrentSettingsSchemaVersion;
    root["themePreset"] = themePreset;
    root["accentColor"] = accentColorHex;
    root["fontFamily"] = uiFontFamily;
    root["appLanguage"] = appLanguage;
    root["locale"] = appLanguage;
    root["autoplay"] = autoplay;
    root["rememberPos"] = rememberPos;
    root["restoreSession"] = restoreSession;
    root["loop"] = loop;
    root["shuffleQueue"] = shuffleQueue;
    root["alwaysOnTop"] = alwaysOnTop;
    root["hwAccel"] = hwAccel;
    root["defaultSpeed"] = defaultSpeed;
    root["arrowSeekStep"] = arrowSeekStepSec;
    root["largeSeekStep"] = largeSeekStepSec;
    root["ctrlSeekFrames"] = ctrlSeekFrames;
    root["customMpvConfigEnabled"] = customMpvConfigEnabled;
    root["customMpvConfigPath"] = customMpvConfigPath;
    root["globalHotkeysEnabled"] = globalHotkeysEnabled;
    root["trayIconEnabled"] = trayIconEnabled;
    root["minimizeToTray"] = minimizeToTray;
    root["screenshotFormat"] = screenshotFormat;
    root["jumpListEnabled"] = jumpListEnabled;
    root["hoverPreviewEnabled"] = hoverPreviewEnabled;
    root["reduceMotionEnabled"] = reduceMotionEnabled;
    root["hdrMode"] = hdrMode;
    root["toneMappingAlgorithm"] = toneMappingAlgorithm;
    root["targetPeak"] = targetPeak;
    root["gamutMapping"] = gamutMapping;
    root["iccProfilePath"] = iccProfilePath;
    root["audioLang"] = audioLang;
    root["subLang"] = subLang;
    root["lastAudioTrackTitle"] = lastAudioTrackTitle;
    root["lastSubtitleTrackTitle"] = lastSubtitleTrackTitle;
    root["subtitlesEnabled"] = subtitlesEnabled;
    root["subFontSize"] = subFontSize;
    root["subColor"] = subColorHex;
    root["subBgStyle"] = subBgStyle;
    root["subPosition"] = subPosition;
    root["subSafeAreaX"] = subSafeAreaX;
    root["subSafeAreaY"] = subSafeAreaY;
    root["subForcedOnly"] = subForcedOnly;
    root["subCodepage"] = subCodepage;
    root["subFontFamily"] = subFontFamily;
    root["audioDelayMs"] = audioDelayMs;
    root["subDelayMs"] = subDelayMs;
    root["secondarySubLang"] = secondarySubLang;
    root["lastSecondarySubtitleTrackTitle"] = lastSecondarySubtitleTrackTitle;
    root["secondarySubtitlesEnabled"] = secondarySubtitlesEnabled;
    root["secondarySubFontSize"] = secondarySubFontSize;
    root["secondarySubColor"] = secondarySubColorHex;
    root["secondarySubPosition"] = secondarySubPosition;
    root["secondarySubDelayMs"] = secondarySubDelayMs;
    root["onlineSubtitlesEnabled"] = onlineSubtitlesEnabled;
    root["onlineSubtitleEndpoint"] = onlineSubtitleEndpoint;
    root["activeSubtitleProfile"] = activeSubtitleProfile;
    JsonValue profilesArr = JsonValue::Array();
    for (const auto& prof : subtitleProfiles) {
        JsonValue profObj = prof.unknownFields.is_object() ? prof.unknownFields : JsonValue::Object();
        profObj["name"] = prof.name;
        profObj["fontSize"] = prof.fontSize;
        profObj["color"] = prof.colorHex;
        profObj["colorHex"] = prof.colorHex;
        profObj["bgStyle"] = prof.bgStyle;
        profObj["position"] = prof.position;
        profObj["safeAreaX"] = prof.safeAreaX;
        profObj["safeAreaY"] = prof.safeAreaY;
        profilesArr.push_back(profObj);
    }
    root["subtitleProfiles"] = profilesArr;
    root["autoUpdateCheckEnabled"] = autoUpdateCheckEnabled;
    root["updateChannel"] = updateChannel;
    root["updateCheckUrl"] = updateCheckUrl;
    root["audioDevice"] = audioDevice;
    root["audioLayout"] = audioLayout;
    root["audioChannelMix"] = audioChannelMix;
    root["audioNormalizeDownmix"] = audioNormalizeDownmix;
    root["nightMode"] = nightMode;
    root["audioBalance"] = audioBalance;
    root["audioChannelMap"] = audioChannelMap;
    root["replayGainMode"] = replayGainMode;
    root["replayGainPreampDb"] = replayGainPreampDb;
    root["deinterlaceMode"] = deinterlaceMode;
    root["audioPassthrough"] = audioPassthrough;
    root["pitchCorrection"] = pitchCorrection;
    root["storageMode"] = storageMode;
    root["playlistViewMode"] = playlistViewMode;
    root["libraryGroupMode"] = libraryGroupMode;
    root["librarySortField"] = librarySortField;
    root["librarySortDirection"] = librarySortDirection;
    JsonValue folderOrderArr = JsonValue::Array();
    for (const auto& f : customFolderOrder) { folderOrderArr.as_array().push_back(JsonValue(Utf16ToUtf8(f))); }
    root["customFolderOrder"] = folderOrderArr;
    root["experienceMode"] = experienceMode;
    JsonValue customFeaturesArr = JsonValue::Array();
    for (const auto& key : customEnabledFeatures) { customFeaturesArr.as_array().push_back(JsonValue(key)); }
    root["customEnabledFeatures"] = customFeaturesArr;
    root["manualMediaOrder"] = manualMediaOrder;
    root["smartSeriesDetection"] = smartSeriesDetection;
    root["diagnosticsLogging"] = diagnosticsLogging;
    root["muted"] = muted;
    root["volume"] = std::clamp(std::isfinite(volume) ? volume : 1.0, 0.0, kMaxVolumeRatio);
    root["lastAspectRatio"] = lastAspectRatio;
    root["videoRotate"] = videoRotate;
    root["brightness"] = brightness;
    root["contrast"] = contrast;
    root["saturation"] = saturation;
    root["gamma"] = gamma;
    root["hue"] = hue;
    root["windowX"] = windowX;
    root["windowY"] = windowY;
    root["windowWidth"] = windowWidth;
    root["windowHeight"] = windowHeight;
    root["windowMaximized"] = windowMaximized;
    root["hasWindowPlacement"] = hasWindowPlacement;
    root["pipX"] = pipX;
    root["pipY"] = pipY;
    root["pipWidth"] = pipWidth;
    root["pipHeight"] = pipHeight;
    root["cacheTotalBudgetMB"] = cacheTotalBudgetMB;
    root["cacheThumbnailBudgetMB"] = cacheThumbnailBudgetMB;
    root["cacheShaderBudgetMB"] = cacheShaderBudgetMB;
    root["cacheSubtitleBudgetMB"] = cacheSubtitleBudgetMB;
    root["cacheGenericBudgetMB"] = cacheGenericBudgetMB;
    // Serialize shortcuts
    JsonValue scObj = persistedShortcutFields.is_object() ? persistedShortcutFields : JsonValue::Object();
    for (const auto& pair : shortcuts) {
        JsonValue bObj = scObj.get(pair.first).is_object() ? scObj.get(pair.first) : JsonValue::Object();
        bObj["vk"] = (int)pair.second.vkCode;
        bObj["key"] = VKToString(pair.second.vkCode);
        bObj["ctrl"] = pair.second.ctrl;
        bObj["shift"] = pair.second.shift;
        bObj["alt"] = pair.second.alt;
        bObj["name"] = pair.second.displayName;
        scObj[pair.first] = bObj;
    }
    root["shortcuts"] = scObj;
    persistedShortcutFields = scObj;
    const std::string json = root.stringify(true);
    std::wstring path = GetDataDir() + L"\\settings.json";
    if (json != lastSavedJson || !FileExists(path)) {
        StorageStatus backup = CreateRotatingBackup(path, 3);
        if (!backup.is_ok()) { lastSaveStatus = backup; return lastSaveStatus; }
        StorageStatus writeRes = AtomicWriteTextFile(path, json);
        if (!writeRes.is_ok()) { lastSaveStatus = writeRes; return lastSaveStatus; }
        lastSavedJson = json;
    }
    lastSaveStatus = StorageStatus::Ok(path);
    return lastSaveStatus;
}

std::string Settings::NormalizeHdrMode(const std::string& mode) {
    std::string s = LowerAscii(TrimAscii(mode));
    if (s == "passthrough" || s == "tone-map" || s == "off") return s;
    return "auto";
}

std::string Settings::NormalizeToneMapping(const std::string& tm) {
    std::string s = LowerAscii(TrimAscii(tm));
    if (s == "spline" || s == "bt.2390" || s == "bt.2446a" || s == "hable" ||
        s == "mobius" || s == "reinhard" || s == "clip" || s == "linear" || s == "gamma") {
        return s;
    }
    return "auto";
}

int Settings::NormalizeTargetPeak(int peak) { return std::clamp(peak, 0, 10000); }

std::string Settings::NormalizeGamutMapping(const std::string& gm) {
    std::string s = LowerAscii(TrimAscii(gm));
    if (s == "clip" || s == "warn" || s == "linear" || s == "darken" || s == "desaturate") { return s; }
    return "auto";
}

bool Settings::IsRootContainedIccPath(const std::string& rawPath, const std::wstring& exeDir) {
    return ContainedRelativePath(rawPath, exeDir, true).has_value();
}

std::string Settings::NormalizeIccPath(const std::string& rawPath, const std::wstring& exeDir) {
    const auto relative = ContainedRelativePath(rawPath, exeDir, true);
    return relative ? Utf16ToUtf8(*relative) : "";
}

bool Settings::IsRootContainedMpvConfigPath(const std::string& rawPath, const std::wstring& exeDir) {
    return ContainedRelativePath(rawPath, exeDir, false).has_value();
}

std::string Settings::NormalizeMpvConfigPath(const std::string& rawPath, const std::wstring& exeDir) {
    const auto relative = ContainedRelativePath(rawPath, exeDir, false);
    return relative ? Utf16ToUtf8(*relative) : "mpv.conf";
}

std::wstring Settings::ResolveMpvConfigPath(const std::string& path, const std::wstring& exeDir) {
    std::string norm = NormalizeMpvConfigPath(path, exeDir);
    std::wstring baseDir = exeDir;
    while (!baseDir.empty() && (baseDir.back() == L'\\' || baseDir.back() == L'/')) { baseDir.pop_back(); }
    return baseDir + L"\\" + Utf8ToUtf16(norm);
}

std::wstring Settings::GetMpvConfigLastKnownGoodPath(const std::wstring& configPath) { return configPath + L".lkg"; }

StorageStatus Settings::SaveMpvConfigLastKnownGood(const std::wstring& configPath, const std::string& content) {
    if (configPath.empty()) return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, configPath, "Empty config path");
    std::wstring lkgPath = GetMpvConfigLastKnownGoodPath(configPath);
    return AtomicWriteTextFile(lkgPath, content);
}

StorageStatus Settings::LoadMpvConfigLastKnownGood(const std::wstring& configPath, std::string& outContent) {
    outContent.clear();
    if (configPath.empty()) return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, configPath, "Empty config path");
    std::wstring lkgPath = GetMpvConfigLastKnownGoodPath(configPath);
    if (!FileExists(lkgPath))
        return StorageStatus::Error(StorageErrorCode::FileOpenFailed, ERROR_FILE_NOT_FOUND, lkgPath, "No last-known-good backup available");
    return ReadTextFile(lkgPath, outContent);
}

StorageStatus Settings::RevertMpvConfigToLastKnownGood(const std::wstring& configPath) {
    std::string lkgContent;
    StorageStatus s = LoadMpvConfigLastKnownGood(configPath, lkgContent);
    if (!s.is_ok()) return s;
    CreateRotatingBackup(configPath, 3);
    return AtomicWriteTextFile(configPath, lkgContent);
}

StorageStatus Settings::ResetMpvConfigToDefault(const std::wstring& configPath) {
    if (configPath.empty()) return StorageStatus::Error(StorageErrorCode::InvalidPath, ERROR_INVALID_PARAMETER, configPath, "Empty config path");
    if (FileExists(configPath)) { CreateRotatingBackup(configPath, 3); }
    std::string defaultTemplate = "# Aperture Custom MPV Configuration\n"
                                  "# Add custom mpv properties below (format: property=value)\n"
                                  "# Example: deband=yes\n"
                                  "# Example: scale=spline36\n";
    return AtomicWriteTextFile(configPath, defaultTemplate);
}

std::string Settings::NormalizeAudioDevice(const std::string& dev) { return AudioConfig::NormalizeAudioDevice(dev); }

std::string Settings::NormalizeAudioLayout(const std::string& layout) { return AudioConfig::NormalizeAudioLayout(layout); }

std::string Settings::NormalizeAudioChannelMix(const std::string& mix) { return AudioConfig::NormalizeAudioChannelMix(mix); }

std::string Settings::NormalizeNightMode(const std::string& nm) { return AudioConfig::NormalizeNightMode(nm); }

double Settings::NormalizeAudioBalance(double balance) { return AudioConfig::NormalizeAudioBalance(balance); }

std::string Settings::NormalizeAudioChannelMap(const std::string& map) { return AudioConfig::NormalizeAudioChannelMap(map); }

std::string Settings::NormalizeReplayGainMode(const std::string& mode) { return AudioConfig::NormalizeReplayGainMode(mode); }

double Settings::NormalizeReplayGainPreampDb(double db) { return AudioConfig::NormalizeReplayGainPreampDb(db); }

std::string Settings::NormalizeDeinterlaceMode(const std::string& mode) {
    std::string s = LowerAscii(TrimAscii(mode));
    if (s == "bwdif" || s == "bwdif_field" || s == "yadif") return s;
    return "off";
}
bool Settings::IsValidProfileName(const std::string& name) {
    std::string trimmed = TrimAscii(name);
    if (trimmed.empty() || trimmed.length() > kMaxProfileNameLength) return false;
    for (char c : trimmed) { unsigned char uc = static_cast<unsigned char>(c); if (uc < 32 || uc == 127) return false; }
    return true;
}

std::string Settings::SanitizeProfileName(const std::string& name) {
    std::string clean;
    clean.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc != 127) { clean.push_back(c); }
    }
    clean = TrimAscii(clean);
    if (clean.length() > kMaxProfileNameLength) { clean = clean.substr(0, kMaxProfileNameLength); clean = TrimAscii(clean); }
    return clean;
}

std::string Settings::NormalizeProfileFontSize(const std::string& size) {
    std::string s = LowerAscii(TrimAscii(size));
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "px") == 0) { s.resize(s.size() - 2); }
    char* end = nullptr;
    const long parsed = std::strtol(s.c_str(), &end, 10);
    if (end && *end == '\0' && parsed >= 14 && parsed <= 32) { return std::to_string(parsed) + "px"; }
    return "20px";
}

std::string Settings::NormalizeProfileColor(const std::string& hex) {
    std::string s = LowerAscii(TrimAscii(hex));
    if (IsHexColor(s)) return s;
    if (s.length() == 6) {
        bool allHex = true;
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) { allHex = false; break; }
        }
        if (allHex) return "#" + s;
    }
    return "#cad3f5";
}

std::string Settings::NormalizeProfileBgStyle(const std::string& style) {
    std::string s = LowerAscii(TrimAscii(style));
    if (s == "vlc-outline" || s == "translucent" || s == "solid" || s == "clean") return s;
    return "vlc-outline";
}

std::string Settings::NormalizeProfilePosition(const std::string& pos) {
    std::string s = LowerAscii(TrimAscii(pos));
    if (s == "bottom" || s == "elevated" || s == "top") return s;
    return "bottom";
}

int Settings::NormalizeSafeAreaPercent(int percent) { return std::clamp(percent, 0, 30); }

const SubtitleStyleProfile* Settings::FindSubtitleProfile(const std::string& name) const {
    std::string target = LowerAscii(TrimAscii(name));
    if (target.empty()) return nullptr;
    for (const auto& p : subtitleProfiles) { if (LowerAscii(p.name) == target) return &p; }
    return nullptr;
}

SubtitleStyleProfile* Settings::FindSubtitleProfile(const std::string& name) {
    std::string target = LowerAscii(TrimAscii(name));
    if (target.empty()) return nullptr;
    for (auto& p : subtitleProfiles) { if (LowerAscii(p.name) == target) return &p; }
    return nullptr;
}

bool Settings::AddOrUpdateSubtitleProfile(const SubtitleStyleProfile& profile) {
    std::string cleanName = SanitizeProfileName(profile.name);
    if (!IsValidProfileName(cleanName)) return false;
    SubtitleStyleProfile cleanProfile = profile;
    cleanProfile.name = cleanName;
    cleanProfile.fontSize = NormalizeProfileFontSize(profile.fontSize);
    cleanProfile.colorHex = NormalizeProfileColor(profile.colorHex);
    cleanProfile.bgStyle = NormalizeProfileBgStyle(profile.bgStyle);
    cleanProfile.position = NormalizeProfilePosition(profile.position);
    cleanProfile.safeAreaX = NormalizeSafeAreaPercent(profile.safeAreaX);
    cleanProfile.safeAreaY = NormalizeSafeAreaPercent(profile.safeAreaY);
    for (auto& existing : subtitleProfiles) {
        if (LowerAscii(existing.name) == LowerAscii(cleanName)) {
            JsonValue mergedUnknown = existing.unknownFields.is_object() ? existing.unknownFields : JsonValue::Object();
            if (profile.unknownFields.is_object()) {
                for (const auto& kv : profile.unknownFields.as_object()) { mergedUnknown[kv.first] = kv.second; }
            }
            cleanProfile.unknownFields = mergedUnknown;
            existing = cleanProfile;
            return true;
        }
    }
    if (subtitleProfiles.size() >= kMaxSubtitleProfiles) return false;
    subtitleProfiles.push_back(cleanProfile);
    return true;
}

bool Settings::RenameSubtitleProfile(const std::string& oldName, const std::string& newName) {
    std::string cleanOld = SanitizeProfileName(oldName);
    std::string cleanNew = SanitizeProfileName(newName);
    if (!IsValidProfileName(cleanNew)) return false;
    if (LowerAscii(cleanOld) != LowerAscii(cleanNew)) { if (FindSubtitleProfile(cleanNew) != nullptr) return false; }
    for (auto& p : subtitleProfiles) {
        if (LowerAscii(p.name) == LowerAscii(cleanOld)) {
            p.name = cleanNew;
            if (LowerAscii(activeSubtitleProfile) == LowerAscii(cleanOld)) { activeSubtitleProfile = cleanNew; }
            return true;
        }
    }
    return false;
}

bool Settings::DeleteSubtitleProfile(const std::string& name) {
    std::string clean = SanitizeProfileName(name);
    for (auto it = subtitleProfiles.begin(); it != subtitleProfiles.end(); ++it) {
        if (LowerAscii(it->name) == LowerAscii(clean)) {
            if (LowerAscii(activeSubtitleProfile) == LowerAscii(clean)) { activeSubtitleProfile = ""; }
            subtitleProfiles.erase(it);
            return true;
        }
    }
    return false;
}

bool Settings::ApplySubtitleProfile(const std::string& name) {
    const SubtitleStyleProfile* prof = FindSubtitleProfile(name);
    if (!prof) return false;
    subFontSize = prof->fontSize;
    subColorHex = prof->colorHex;
    subBgStyle = prof->bgStyle;
    subPosition = prof->position;
    subSafeAreaX = prof->safeAreaX;
    subSafeAreaY = prof->safeAreaY;
    activeSubtitleProfile = prof->name;
    return true;
}

void Settings::ResetSubtitleStyleToDefault() {
    subFontSize = "20px";
    subColorHex = "#cad3f5";
    subBgStyle = "vlc-outline";
    subPosition = "bottom";
    subSafeAreaX = 0;
    subSafeAreaY = 0;
    activeSubtitleProfile = "";
}

SubtitleStyleProfile Settings::GetCurrentSubtitleStyleAsProfile(const std::string& name) const {
    SubtitleStyleProfile prof;
    prof.name = SanitizeProfileName(name);
    prof.fontSize = NormalizeProfileFontSize(subFontSize);
    prof.colorHex = NormalizeProfileColor(subColorHex);
    prof.bgStyle = NormalizeProfileBgStyle(subBgStyle);
    prof.position = NormalizeProfilePosition(subPosition);
    prof.safeAreaX = NormalizeSafeAreaPercent(subSafeAreaX);
    prof.safeAreaY = NormalizeSafeAreaPercent(subSafeAreaY);
    return prof;
}
} // namespace Aperture
