#pragma once

#include <windows.h>

namespace Aperture {

// Return the visible screen-space rectangle occupied by a window's client
// area. This is the coordinate space used by the separate player overlay.
inline bool GetClientScreenRect(HWND hWnd, RECT& screenRect) {
    if (!hWnd) return false;

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) return false;

    POINT topLeft{ clientRect.left, clientRect.top };
    POINT bottomRight{ clientRect.right, clientRect.bottom };
    if (!ClientToScreen(hWnd, &topLeft) || !ClientToScreen(hWnd, &bottomRight)) return false;

    screenRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    return screenRect.right > screenRect.left && screenRect.bottom > screenRect.top;
}

inline bool PlaceWindowOverClient(HWND owner, HWND surface, HWND insertAfter, UINT flags, RECT* placedRect = nullptr) {
    if (!surface) return false;

    RECT screenRect{};
    if (!GetClientScreenRect(owner, screenRect)) return false;

    if (!SetWindowPos(surface, insertAfter, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, flags)) {
        return false;
    }
    if (placedRect) *placedRect = screenRect;
    return true;
}

} // namespace Aperture
