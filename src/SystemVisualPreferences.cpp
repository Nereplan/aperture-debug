#include "SystemVisualPreferences.h"
#include <algorithm>
#include <cmath>

namespace Aperture {
namespace {

bool QueryBool(UINT action, bool fallback, bool& succeeded) {
    BOOL value = fallback ? TRUE : FALSE;
    if (!SystemParametersInfoW(action, 0, &value, 0)) { succeeded = false; return fallback; }
    return value != FALSE;
}

bool SameColors(const SystemHighContrastPalette& a, const SystemHighContrastPalette& b) {
    return a.text == b.text && a.background == b.background && a.highlight == b.highlight && a.highlightText == b.highlightText &&
           a.disabled == b.disabled && a.border == b.border;
}

} // namespace

SystemVisualPreferences::SystemVisualPreferences() { snapshot_ = SystemVisualPreferencesSnapshot{}; }

SystemVisualPreferencesSnapshot SystemVisualPreferences::Query() {
    SystemVisualPreferencesSnapshot result;
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
