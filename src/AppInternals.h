#pragma once

// File-scope helpers shared by the App translation units. Everything here was
// static inside App.cpp before the mechanical split; inline keeps one
// definition across the translation units that include this header.

#include "App.h"
#include <commdlg.h>
#include <imm.h>
#include <tpcshrd.h>
#include <string>

namespace Aperture {

// comdlg32 is delay-loaded: POD-only delay gate so callers with C++ objects
// never need __try themselves (C2712). System-only presence first, SEH backstop.
inline bool DelaySafeGetOpenFileNameW(OPENFILENAMEW* ofn) {
    if (!ofn) return false;
    if (!SystemDllPresent(L"comdlg32.dll")) return false;
    bool ok = false;
    __try {
        ok = !!GetOpenFileNameW(ofn);
    } __except (DelayLoadExceptionFilter(GetExceptionCode())) { ok = false; }
    return ok;
}


inline bool IsSupportedSubtitlePath(const std::wstring& path) {
    std::wstring ext = GetFileExtension(path);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
    return ext == L".srt" || ext == L".vtt" || ext == L".ass" || ext == L".ssa";
}


inline int SubtitleSizeFromSliderX(float x, float left, float right) {
    if (right <= left) return 20;
    const float ratio = std::clamp((x - left) / (right - left), 0.0f, 1.0f);
    return std::clamp(14 + static_cast<int>(std::lround(ratio * 18.0f)), 14, 32);
}


inline std::string SubtitleSizeToString(int pixels) { return std::to_string(std::clamp(pixels, 14, 32)) + "px"; }


inline bool TranslatePointerMessage(HWND hwnd, UINT pointerMessage, WPARAM wParam,
                                    UINT& mouseMessage, WPARAM& mouseWParam, LPARAM& mouseLParam) {
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    POINTER_INFO info{};
    if (!GetPointerType(pointerId, &type) || (type != PT_TOUCH && type != PT_PEN) || !GetPointerInfo(pointerId, &info)) return false;
    POINT pt = info.ptPixelLocation;
    ScreenToClient(hwnd, &pt);
    mouseLParam = MAKELPARAM(static_cast<short>(pt.x), static_cast<short>(pt.y));
    mouseWParam = 0;
    const bool secondary = (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) != 0;
    if (pointerMessage == WM_POINTERDOWN) {
        mouseMessage = secondary ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
        mouseWParam = secondary ? MK_RBUTTON : MK_LBUTTON;
    } else if (pointerMessage == WM_POINTERUP) { mouseMessage = secondary ? WM_RBUTTONUP : WM_LBUTTONUP; } else {
        mouseMessage = WM_MOUSEMOVE;
        if (info.pointerFlags & POINTER_FLAG_FIRSTBUTTON) mouseWParam |= MK_LBUTTON;
        if (secondary) mouseWParam |= MK_RBUTTON;
    }
    return true;
}


// Decoded WM_POINTER payload for the touch gesture layer. Positions are
// client-space physical pixels.
struct TouchPointerEvent {
    UINT32 pointerId = 0;
    bool inContact = false;      // POINTER_FLAG_INCONTACT (pen can hover)
    bool barrelButton = false;   // pen barrel button / touch second button
    float xPx = 0.0f, yPx = 0.0f;
};

inline bool GetTouchPointerEvent(HWND hwnd, UINT pointerMessage, WPARAM wParam, TouchPointerEvent& out) {
    const UINT32 pointerId = GET_POINTERID_WPARAM(wParam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    POINTER_INFO info{};
    if (!GetPointerType(pointerId, &type) || (type != PT_TOUCH && type != PT_PEN) || !GetPointerInfo(pointerId, &info)) return false;
    out.pointerId = pointerId;
    out.inContact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    out.barrelButton = (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) != 0;
    POINT pt = info.ptPixelLocation;
    ScreenToClient(hwnd, &pt);
    out.xPx = static_cast<float>(pt.x);
    out.yPx = static_cast<float>(pt.y);
    (void)pointerMessage;
    return true;
}

// The app owns all touch semantics through WM_POINTER (taps, pans, flings,
// long-press); system press-and-hold, flicks, and legacy gesture synthesis
// would inject competing input, so every window opts out of them.
inline constexpr UINT kTabletGestureDisableMask =
    TABLET_DISABLE_PRESSANDHOLD | TABLET_DISABLE_PENTAPFEEDBACK | TABLET_DISABLE_PENBARRELFEEDBACK |
    TABLET_DISABLE_TOUCHSWITCH | TABLET_DISABLE_FLICKS |
    TABLET_DISABLE_SMOOTHSCROLLING | TABLET_DISABLE_FLICKFALLBACKKEYS | TABLET_ENABLE_MULTITOUCHDATA;

inline LRESULT TabletSystemGestureStatus() { return kTabletGestureDisableMask; }

inline void DisableLegacyTouchGestures(HWND hwnd) {
    if (!hwnd) return;
    GESTURECONFIG config = { 0, 0, GC_ALLGESTURES }; // block every legacy gesture
    SetGestureConfig(hwnd, 0, 1, &config, sizeof(config));
}


inline RECT TooltipClientRect(float left, float top, float right, float bottom) {
    RECT result{};
    result.left = static_cast<LONG>(std::lround(left));
    result.top = static_cast<LONG>(std::lround(top));
    result.right = static_cast<LONG>(std::lround(right));
    result.bottom = static_cast<LONG>(std::lround(bottom));
    return result;
}


inline void MixTooltipSignature(unsigned long long& signature, unsigned long long value) {
    signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
}


inline std::wstring TooltipActionLabel(const Settings& settings, const std::wstring& label, const std::string& actionId) {
    const auto it = settings.shortcuts.find(actionId);
    if (it == settings.shortcuts.end()) return label;
    const std::wstring shortcut = Settings::FormatShortcutKey(it->second);
    return shortcut.empty() ? label : label + L"  " + shortcut;
}


inline const char* LibrarySortSettingValue(LibrarySortField field) {
    switch (field) {
        case LibrarySortField::Manual: return "manual";
        case LibrarySortField::DateAdded: return "dateAdded";
        case LibrarySortField::LastPlayed: return "lastPlayed";
        case LibrarySortField::Duration: return "duration";
        case LibrarySortField::Progress: return "progress";
        case LibrarySortField::Title:
        default: return "title";
    }
}


inline LibrarySortField ResolveLibrarySortSetting(const Settings& settings, LibraryGroupMode groupMode) {
    if (settings.librarySortField == "manual") return LibrarySortField::Manual;
    if (settings.librarySortField == "dateAdded") return LibrarySortField::DateAdded;
    if (settings.librarySortField == "lastPlayed") return LibrarySortField::LastPlayed;
    if (settings.librarySortField == "duration") return LibrarySortField::Duration;
    if (settings.librarySortField == "progress") return LibrarySortField::Title;
    if (settings.librarySortField == "title") return LibrarySortField::Title;
    return settings.manualMediaOrder && groupMode == LibraryGroupMode::Flat ? LibrarySortField::Manual : LibrarySortField::Title;
}

inline bool IsHighUtf16Unit(wchar_t c) { const unsigned int u = static_cast<unsigned int>(c); return u >= 0xD800u && u <= 0xDBFFu; }

inline bool IsLowUtf16Unit(wchar_t c) { const unsigned int u = static_cast<unsigned int>(c); return u >= 0xDC00u && u <= 0xDFFFu; }

inline size_t SafeWideBoundary(const std::wstring& text, size_t index) {
    index = std::min(index, text.size());
    if (index > 0 && index < text.size() && IsHighUtf16Unit(text[index - 1]) && IsLowUtf16Unit(text[index])) return index - 1;
    return index;
}

inline std::wstring ReadImeString(HIMC himc, DWORD index) {
    if (!himc) return {};
    LONG bytes = ImmGetCompositionStringW(himc, index, nullptr, 0);
    if (bytes <= 0) return {};
    std::wstring value(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    if (value.empty() || ImmGetCompositionStringW(himc, index, value.data(), bytes) < 0) return {};
    return value;
}

} // namespace Aperture
