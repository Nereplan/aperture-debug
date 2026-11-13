#include "SystemVisualPreferences.h"
#include <algorithm>
#include <cmath>
#if !defined(_WIN32)
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <string>
#endif

namespace Aperture {
namespace {

#if defined(_WIN32)
bool QueryBool(UINT action, bool fallback, bool& succeeded) {
    BOOL value = fallback ? TRUE : FALSE;
    if (!SystemParametersInfoW(action, 0, &value, 0)) { succeeded = false; return fallback; }
    return value != FALSE;
}
#else
std::string ToLowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimAscii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string GetLinuxConfigDir() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return xdg;
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) return std::string(home) + "/.config";
    }
    return {};
}

struct LinuxVisualSignals {
    bool preferDark = false;
    bool hasColorScheme = false;
    bool highContrast = false;
    bool animationsEnabled = true;
    bool hasAccentColor = false;
    COLORREF accentColor = RGB(0, 120, 215);
    bool screenReader = false;
    float textScaleFactor = 1.0f;
    bool portalSucceeded = false;
    bool configFound = false;
};

void ParseLinuxIniPrefs(const std::string& path, bool isKdeGlobals, LinuxVisualSignals& out) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    out.configFound = true;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ToLowerAscii(TrimAscii(line.substr(0, eq)));
        const std::string val = TrimAscii(line.substr(eq + 1));
        const std::string valLower = ToLowerAscii(val);
        if (!isKdeGlobals) {
            if (key == "gtk-application-prefer-dark-theme") {
                if (valLower == "1" || valLower == "true") {
                    out.preferDark = true;
                    out.hasColorScheme = true;
                } else if (valLower == "0" || valLower == "false") {
                    out.preferDark = false;
                    out.hasColorScheme = true;
                }
            } else if (key == "gtk-theme-name") {
                if (valLower.find("highcontrast") != std::string::npos) out.highContrast = true;
                if (valLower.find("dark") != std::string::npos) {
                    out.preferDark = true;
                    out.hasColorScheme = true;
                }
            } else if (key == "gtk-enable-animations") {
                if (valLower == "0" || valLower == "false") out.animationsEnabled = false;
                else if (valLower == "1" || valLower == "true") out.animationsEnabled = true;
            } else if (key == "gtk-xft-dpi") {
                try {
                    const double dpiRaw = std::stod(val);
                    if (dpiRaw > 0.0) {
                        out.textScaleFactor = std::clamp(static_cast<float>((dpiRaw / 1024.0) / 96.0), 0.75f, 2.25f);
                    }
                } catch (...) {}
            }
        } else {
            if (key == "animationdurationfactor") {
                try {
                    if (std::stod(val) <= 0.0) out.animationsEnabled = false;
                } catch (...) {}
            } else if (key == "colorscheme" && !out.hasColorScheme) {
                if (valLower.find("highcontrast") != std::string::npos) out.highContrast = true;
                if (valLower.find("dark") != std::string::npos) {
                    out.preferDark = true;
                    out.hasColorScheme = true;
                }
            }
        }
    }
}

void QueryXdgDesktopPortal(LinuxVisualSignals& out) {
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) return;
    static void* dbusLib = []() -> void* {
        void* lib = ::dlopen("libdbus-1.so.3", RTLD_LAZY);
        return lib ? lib : ::dlopen("libdbus-1.so", RTLD_LAZY);
    }();
    if (!dbusLib) return;
    using FnThreadsInit = int (*)();
    using FnBusGet = void* (*)(int, void*);
    using FnMsgNew = void* (*)(const char*, const char*, const char*, const char*);
    using FnIterInitAppend = void (*)(void*, void*);
    using FnIterInit = int (*)(void*, void*);
    using FnIterOpenContainer = int (*)(void*, int, const char*, void*);
    using FnIterAppendBasic = int (*)(void*, int, const void*);
    using FnIterCloseContainer = int (*)(void*, void*);
    using FnIterGetArgType = int (*)(void*);
    using FnIterRecurse = void (*)(void*, void*);
    using FnIterNext = int (*)(void*);
    using FnIterGetBasic = void (*)(void*, void*);
    using FnSendReply = void* (*)(void*, void*, int, void*);
    using FnMsgUnref = void (*)(void*);
    using FnConnUnref = void (*)(void*);
    static auto pThreadsInit = reinterpret_cast<FnThreadsInit>(::dlsym(dbusLib, "dbus_threads_init_default"));
    static auto pBusGet = reinterpret_cast<FnBusGet>(::dlsym(dbusLib, "dbus_bus_get"));
    static auto pMsgNew = reinterpret_cast<FnMsgNew>(::dlsym(dbusLib, "dbus_message_new_method_call"));
    static auto pIterInitAppend = reinterpret_cast<FnIterInitAppend>(::dlsym(dbusLib, "dbus_message_iter_init_append"));
    static auto pIterInit = reinterpret_cast<FnIterInit>(::dlsym(dbusLib, "dbus_message_iter_init"));
    static auto pIterOpen = reinterpret_cast<FnIterOpenContainer>(::dlsym(dbusLib, "dbus_message_iter_open_container"));
    static auto pIterAppend = reinterpret_cast<FnIterAppendBasic>(::dlsym(dbusLib, "dbus_message_iter_append_basic"));
    static auto pIterClose = reinterpret_cast<FnIterCloseContainer>(::dlsym(dbusLib, "dbus_message_iter_close_container"));
    static auto pGetType = reinterpret_cast<FnIterGetArgType>(::dlsym(dbusLib, "dbus_message_iter_get_arg_type"));
    static auto pRecurse = reinterpret_cast<FnIterRecurse>(::dlsym(dbusLib, "dbus_message_iter_recurse"));
    static auto pNext = reinterpret_cast<FnIterNext>(::dlsym(dbusLib, "dbus_message_iter_next"));
    static auto pGetBasic = reinterpret_cast<FnIterGetBasic>(::dlsym(dbusLib, "dbus_message_iter_get_basic"));
    static auto pSendReply = reinterpret_cast<FnSendReply>(::dlsym(dbusLib, "dbus_connection_send_with_reply_and_block"));
    static auto pMsgUnref = reinterpret_cast<FnMsgUnref>(::dlsym(dbusLib, "dbus_message_unref"));
    static auto pConnUnref = reinterpret_cast<FnConnUnref>(::dlsym(dbusLib, "dbus_connection_unref"));
    if (!pBusGet || !pMsgNew || !pIterInitAppend || !pIterInit || !pIterOpen || !pIterAppend ||
        !pIterClose || !pGetType || !pRecurse || !pNext || !pGetBasic || !pSendReply || !pMsgUnref) {
        return;
    }
    static std::once_flag dbusThreadsOnce;
    std::call_once(dbusThreadsOnce, []() {
        if (pThreadsInit) pThreadsInit();
    });
    void* conn = pBusGet(0 /* DBUS_BUS_SESSION */, nullptr);
    if (!conn) return;
    void* msg = pMsgNew("org.freedesktop.portal.Desktop",
                        "/org/freedesktop/portal/desktop",
                        "org.freedesktop.portal.Settings",
                        "ReadAll");
    if (!msg) {
        if (pConnUnref) pConnUnref(conn);
        return;
    }
    struct RawIter { uintptr_t words[16]; } iter{}, arrIter{};
    pIterInitAppend(msg, &iter);
    const char* namespaces[] = {
        "org.freedesktop.appearance",
        "org.gnome.desktop.interface",
        "org.gnome.desktop.a11y.applications",
    };
    bool okBuild = pIterOpen(&iter, 'a', "s", &arrIter) != 0;
    for (const char* ns : namespaces) {
        if (okBuild && !pIterAppend(&arrIter, 's', &ns)) okBuild = false;
    }
    if (okBuild && !pIterClose(&iter, &arrIter)) okBuild = false;
    void* reply = okBuild ? pSendReply(conn, msg, 250, nullptr) : nullptr;
    pMsgUnref(msg);
    if (!reply) {
        if (pConnUnref) pConnUnref(conn);
        return;
    }
    RawIter rootIter{}, nsArrayIter{};
    if (pIterInit(reply, &rootIter) && pGetType(&rootIter) == 'a') {
        out.portalSucceeded = true;
        pRecurse(&rootIter, &nsArrayIter);
        while (pGetType(&nsArrayIter) == 'e') {
            RawIter nsEntry{}, nsMap{};
            pRecurse(&nsArrayIter, &nsEntry);
            const char* nsName = nullptr;
            if (pGetType(&nsEntry) == 's') {
                pGetBasic(&nsEntry, &nsName);
                pNext(&nsEntry);
            }
            if (nsName && pGetType(&nsEntry) == 'a') {
                const std::string nsStr(nsName);
                pRecurse(&nsEntry, &nsMap);
                while (pGetType(&nsMap) == 'e') {
                    RawIter kvEntry{}, varIter{};
                    pRecurse(&nsMap, &kvEntry);
                    const char* keyName = nullptr;
                    if (pGetType(&kvEntry) == 's') {
                        pGetBasic(&kvEntry, &keyName);
                        pNext(&kvEntry);
                    }
                    if (keyName && pGetType(&kvEntry) == 'v') {
                        const std::string keyStr(keyName);
                        pRecurse(&kvEntry, &varIter);
                        const int vtype = pGetType(&varIter);
                        if (nsStr == "org.freedesktop.appearance") {
                            if (keyStr == "color-scheme" && vtype == 'u') {
                                uint32_t cs = 0;
                                pGetBasic(&varIter, &cs);
                                if (cs == 1) { out.preferDark = true; out.hasColorScheme = true; }
                                else if (cs == 2) { out.preferDark = false; out.hasColorScheme = true; }
                            } else if (keyStr == "contrast" && vtype == 'u') {
                                uint32_t contrast = 0;
                                pGetBasic(&varIter, &contrast);
                                out.highContrast = (contrast == 1);
                            } else if (keyStr == "accent-color" && vtype == 'r') {
                                RawIter rgbIter{};
                                pRecurse(&varIter, &rgbIter);
                                double rgb[3] = { 0.0, 0.47, 0.84 };
                                int idx = 0;
                                while (idx < 3 && pGetType(&rgbIter) == 'd') {
                                    pGetBasic(&rgbIter, &rgb[idx++]);
                                    pNext(&rgbIter);
                                }
                                if (idx == 3 && std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]) &&
                                    rgb[0] >= 0.0 && rgb[0] <= 1.0 && rgb[1] >= 0.0 && rgb[1] <= 1.0 &&
                                    rgb[2] >= 0.0 && rgb[2] <= 1.0) {
                                    auto toByte = [](double v) -> BYTE {
                                        return static_cast<BYTE>(std::clamp(std::lround(v * 255.0), 0L, 255L));
                                    };
                                    out.accentColor = RGB(toByte(rgb[0]), toByte(rgb[1]), toByte(rgb[2]));
                                    out.hasAccentColor = true;
                                }
                            }
                        } else if (nsStr == "org.gnome.desktop.interface") {
                            if (keyStr == "enable-animations" && vtype == 'b') {
                                uint32_t enabled = 1;
                                pGetBasic(&varIter, &enabled);
                                out.animationsEnabled = (enabled != 0);
                            } else if (keyStr == "text-scaling-factor" && vtype == 'd') {
                                double scale = 1.0;
                                pGetBasic(&varIter, &scale);
                                if (std::isfinite(scale) && scale > 0.0) {
                                    out.textScaleFactor = std::clamp(static_cast<float>(scale), 0.75f, 2.25f);
                                }
                            } else if (keyStr == "color-scheme" && vtype == 's' && !out.hasColorScheme) {
                                const char* cs = nullptr;
                                pGetBasic(&varIter, &cs);
                                if (cs && std::string(cs) == "prefer-dark") {
                                    out.preferDark = true;
                                    out.hasColorScheme = true;
                                } else if (cs && std::string(cs) == "prefer-light") {
                                    out.preferDark = false;
                                    out.hasColorScheme = true;
                                }
                            } else if (keyStr == "gtk-theme" && vtype == 's') {
                                const char* theme = nullptr;
                                pGetBasic(&varIter, &theme);
                                if (theme) {
                                    const std::string tLower = ToLowerAscii(theme);
                                    if (tLower.find("highcontrast") != std::string::npos) out.highContrast = true;
                                    if (!out.hasColorScheme && tLower.find("dark") != std::string::npos) {
                                        out.preferDark = true;
                                        out.hasColorScheme = true;
                                    }
                                }
                            }
                        } else if (nsStr == "org.gnome.desktop.a11y.applications") {
                            if (keyStr == "screen-reader-enabled" && vtype == 'b') {
                                uint32_t sr = 0;
                                pGetBasic(&varIter, &sr);
                                out.screenReader = (sr != 0);
                            }
                        }
                    }
                    pNext(&nsMap);
                }
            }
            pNext(&nsArrayIter);
        }
    }
    pMsgUnref(reply);
    if (pConnUnref) pConnUnref(conn);
}
#endif

bool SameColors(const SystemHighContrastPalette& a, const SystemHighContrastPalette& b) {
    return a.text == b.text && a.background == b.background && a.highlight == b.highlight && a.highlightText == b.highlightText &&
           a.disabled == b.disabled && a.border == b.border;
}

} // namespace

SystemVisualPreferences::SystemVisualPreferences() { snapshot_ = SystemVisualPreferencesSnapshot{}; }

SystemVisualPreferencesSnapshot SystemVisualPreferences::Query() {
    SystemVisualPreferencesSnapshot result;
#if defined(_WIN32)
    bool succeeded = true;
    HIGHCONTRASTW highContrast = {};
    highContrast.cbSize = sizeof(highContrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0)) {
        result.highContrast = (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    } else { succeeded = false; }
    result.clientAreaAnimation = QueryBool(SPI_GETCLIENTAREAANIMATION, true, succeeded);
    result.menuAnimation = QueryBool(SPI_GETMENUANIMATION, true, succeeded);
    result.menuFade = QueryBool(SPI_GETMENUFADE, true, succeeded);
    result.screenReader = QueryBool(SPI_GETSCREENREADER, false, succeeded);
    DWORD textScalePercent = 100;
    DWORD textScaleSize = sizeof(textScalePercent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr,
                     &textScalePercent, &textScaleSize) == ERROR_SUCCESS) {
        result.textScaleFactor = std::clamp(textScalePercent / 100.0f, 0.75f, 2.25f);
    }
    result.colors.text = GetSysColor(COLOR_WINDOWTEXT);
    result.colors.background = GetSysColor(COLOR_WINDOW);
    result.colors.highlight = GetSysColor(COLOR_HIGHLIGHT);
    result.colors.highlightText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    result.colors.disabled = GetSysColor(COLOR_GRAYTEXT);
    result.colors.border = GetSysColor(COLOR_3DSHADOW);
    result.querySucceeded = succeeded;
#else
    LinuxVisualSignals sig;
    const std::string cfgDir = GetLinuxConfigDir();
    if (!cfgDir.empty()) {
        ParseLinuxIniPrefs(cfgDir + "/gtk-3.0/settings.ini", false, sig);
        ParseLinuxIniPrefs(cfgDir + "/gtk-4.0/settings.ini", false, sig);
        ParseLinuxIniPrefs(cfgDir + "/kdeglobals", true, sig);
    }
    if (const char* gtkThemeEnv = std::getenv("GTK_THEME")) {
        const std::string tLower = ToLowerAscii(gtkThemeEnv);
        if (tLower.find("highcontrast") != std::string::npos) sig.highContrast = true;
        if (!sig.hasColorScheme && tLower.find("dark") != std::string::npos) {
            sig.preferDark = true;
            sig.hasColorScheme = true;
        }
    }
    QueryXdgDesktopPortal(sig);
    result.highContrast = sig.highContrast;
    result.clientAreaAnimation = sig.animationsEnabled;
    result.menuAnimation = sig.animationsEnabled;
    result.menuFade = sig.animationsEnabled;
    result.screenReader = sig.screenReader;
    result.textScaleFactor = sig.textScaleFactor;
    if (sig.highContrast) {
        result.colors.background = sig.preferDark ? RGB(0, 0, 0) : RGB(255, 255, 255);
        result.colors.text = sig.preferDark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        result.colors.highlight = sig.hasAccentColor ? sig.accentColor : (sig.preferDark ? RGB(26, 235, 255) : RGB(0, 0, 128));
        result.colors.highlightText = sig.preferDark ? RGB(0, 0, 0) : RGB(255, 255, 255);
        result.colors.disabled = sig.preferDark ? RGB(63, 255, 63) : RGB(96, 96, 96);
        result.colors.border = sig.preferDark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    } else if (sig.preferDark) {
        result.colors.background = RGB(24, 24, 27);
        result.colors.text = RGB(244, 244, 245);
        result.colors.highlight = sig.hasAccentColor ? sig.accentColor : RGB(59, 130, 246);
        result.colors.highlightText = RGB(255, 255, 255);
        result.colors.disabled = RGB(140, 140, 150);
        result.colors.border = RGB(72, 72, 82);
    } else {
        result.colors.background = RGB(255, 255, 255);
        result.colors.text = RGB(24, 24, 27);
        result.colors.highlight = sig.hasAccentColor ? sig.accentColor : RGB(0, 120, 215);
        result.colors.highlightText = RGB(255, 255, 255);
        result.colors.disabled = RGB(128, 128, 128);
        result.colors.border = RGB(160, 160, 168);
    }
    result.querySucceeded = sig.portalSucceeded || sig.configFound ||
                            (std::getenv("DISPLAY") != nullptr) || (std::getenv("WAYLAND_DISPLAY") != nullptr);
#endif
    return Normalize(result);
}

SystemVisualPreferencesSnapshot SystemVisualPreferences::Normalize( const SystemVisualPreferencesSnapshot& raw,
    const SystemVisualPreferenceOverrides& overrides) {
    SystemVisualPreferencesSnapshot result = raw;
    if (overrides.overrideHighContrast) result.highContrast = overrides.highContrast;
    if (overrides.overrideClientAreaAnimation) result.clientAreaAnimation = overrides.clientAreaAnimation;
    if (overrides.overrideMenuAnimation) result.menuAnimation = overrides.menuAnimation;
    if (overrides.overrideMenuFade) result.menuFade = overrides.menuFade;
    if (overrides.overrideScreenReader) result.screenReader = overrides.screenReader;
    if (overrides.overrideTextScaleFactor) result.textScaleFactor = overrides.textScaleFactor;
    result.textScaleFactor = std::clamp(result.textScaleFactor, 0.75f, 2.25f);
    if (overrides.overrideColors) result.colors = overrides.colors;
    // Respect every system animation preference when choosing decorative motion.
    result.reduceMotion = !result.clientAreaAnimation || !result.menuAnimation || !result.menuFade;
    return result;
}

uint32_t SystemVisualPreferences::Diff( const SystemVisualPreferencesSnapshot& before, const SystemVisualPreferencesSnapshot& after) {
    uint32_t changes = SystemVisualChangeNone;
    if (before.clientAreaAnimation != after.clientAreaAnimation || before.menuAnimation != after.menuAnimation ||
        before.menuFade != after.menuFade || before.reduceMotion != after.reduceMotion) {
        changes |= SystemVisualChangeMotion;
    }
    if (before.highContrast != after.highContrast || before.screenReader != after.screenReader ||
        std::abs(before.textScaleFactor - after.textScaleFactor) > 0.001f) {
        changes |= SystemVisualChangeAccessibility;
    }
    if (!SameColors(before.colors, after.colors)) { changes |= SystemVisualChangeColors; }
    if (before.querySucceeded != after.querySucceeded) { changes |= SystemVisualChangeQueryStatus; }
    return changes;
}

uint32_t SystemVisualPreferences::Refresh() {
    const SystemVisualPreferencesSnapshot next = Query();
    const uint32_t changes = initialized_ ? Diff(snapshot_, next)
                                          : (SystemVisualChangeMotion | SystemVisualChangeAccessibility | SystemVisualChangeColors |
                                             (next.querySucceeded ? 0u : SystemVisualChangeQueryStatus));
    snapshot_ = next;
    initialized_ = true;
    return changes;
}

} // namespace Aperture
