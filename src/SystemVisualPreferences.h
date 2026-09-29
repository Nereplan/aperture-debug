#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

namespace Aperture {

// Bits returned by SystemVisualPreferences::Refresh(). A zero result means the normalized snapshot is unchanged. QueryStatus is separate so
// a caller can observe a transient SPI failure without treating it as a visual change.
enum SystemVisualChange : uint32_t {
    SystemVisualChangeNone = 0, SystemVisualChangeMotion = 1u << 0, SystemVisualChangeAccessibility = 1u << 1,
    SystemVisualChangeColors = 1u << 2, SystemVisualChangeQueryStatus = 1u << 3, };

struct SystemHighContrastPalette {
    COLORREF text = RGB(0, 0, 0);
    COLORREF background = RGB(255, 255, 255);
    COLORREF highlight = RGB(0, 120, 215);
    COLORREF highlightText = RGB(255, 255, 255);
    COLORREF disabled = RGB(128, 128, 128);
    COLORREF border = RGB(128, 128, 128);
};

struct SystemVisualPreferencesSnapshot {
    bool highContrast = false, clientAreaAnimation = true, menuAnimation = true, menuFade = true;
    bool screenReader = false, reduceMotion = false;
    float textScaleFactor = 1.0f;
    bool querySucceeded = false;
    SystemHighContrastPalette colors;
};

// Pure test/integration override inputs. A field is changed only when its
// corresponding `override...` bit is true; this keeps Normalize deterministic
// without querying or changing any system setting.
struct SystemVisualPreferenceOverrides {
    bool overrideHighContrast = false, highContrast = false, overrideClientAreaAnimation = false, clientAreaAnimation = true;
    bool overrideMenuAnimation = false, menuAnimation = true, overrideMenuFade = false, menuFade = true;
    bool overrideScreenReader = false, screenReader = false, overrideTextScaleFactor = false;
    float textScaleFactor = 1.0f;
    bool overrideColors = false;
    SystemHighContrastPalette colors;
};

class SystemVisualPreferences {
public:
    SystemVisualPreferences();
    // Refreshes the cached snapshot from Win32 system preferences. No registry writes or system setting changes occur. Returns only changed
    // categories, plus QueryStatus when querySucceeded changed.
    uint32_t Refresh();
    const SystemVisualPreferencesSnapshot& Get() const { return snapshot_; }
    bool IsInitialized() const { return initialized_; }
    // Pure normalization hook for tests and future deterministic UI previews.
    // The reduced-motion decision is derived after overrides are applied.
    static SystemVisualPreferencesSnapshot Normalize( const SystemVisualPreferencesSnapshot& raw,
        const SystemVisualPreferenceOverrides& overrides = {});
    static uint32_t Diff( const SystemVisualPreferencesSnapshot& before, const SystemVisualPreferencesSnapshot& after);

private:
    static SystemVisualPreferencesSnapshot Query();
    SystemVisualPreferencesSnapshot snapshot_;
    bool initialized_ = false;
};

} // namespace Aperture
