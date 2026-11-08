#if !defined(_WIN32)

#include "LinuxPlatform.h"
#include <dlfcn.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

namespace {

// Dynamic SDL2 loader supporting Wayland and X11 on CachyOS / Arch Linux
struct Sdl2Api {
    void* lib = nullptr;
    bool initialized = false;

    using SDL_Window = void;
    using SDL_Renderer = void;
    using SDL_Texture = void;

    int (*SDL_Init)(uint32_t flags) = nullptr;
    void (*SDL_Quit)() = nullptr;
    int (*SDL_SetHint)(const char* name, const char* value) = nullptr;
    SDL_Window* (*SDL_CreateWindow)(const char* title, int x, int y, int w, int h, uint32_t flags) = nullptr;
    void (*SDL_DestroyWindow)(SDL_Window* window) = nullptr;
    void (*SDL_SetWindowTitle)(SDL_Window* window, const char* title) = nullptr;
    void (*SDL_SetWindowSize)(SDL_Window* window, int w, int h) = nullptr;
    void (*SDL_GetWindowSize)(SDL_Window* window, int* w, int* h) = nullptr;
    void (*SDL_SetWindowPosition)(SDL_Window* window, int x, int y) = nullptr;
    void (*SDL_GetWindowPosition)(SDL_Window* window, int* x, int* y) = nullptr;
    void (*SDL_ShowWindow)(SDL_Window* window) = nullptr;
    void (*SDL_HideWindow)(SDL_Window* window) = nullptr;
    void (*SDL_MaximizeWindow)(SDL_Window* window) = nullptr;
    void (*SDL_MinimizeWindow)(SDL_Window* window) = nullptr;
    void (*SDL_RestoreWindow)(SDL_Window* window) = nullptr;
    void (*SDL_RaiseWindow)(SDL_Window* window) = nullptr;
    int (*SDL_SetWindowFullscreen)(SDL_Window* window, uint32_t flags) = nullptr;
    uint32_t (*SDL_GetWindowFlags)(SDL_Window* window) = nullptr;
    uint32_t (*SDL_GetWindowID)(SDL_Window* window) = nullptr;

    SDL_Renderer* (*SDL_CreateRenderer)(SDL_Window* window, int index, uint32_t flags) = nullptr;
    void (*SDL_DestroyRenderer)(SDL_Renderer* renderer) = nullptr;
    int (*SDL_RenderClear)(SDL_Renderer* renderer) = nullptr;
    int (*SDL_RenderCopy)(SDL_Renderer* renderer, SDL_Texture* texture, const void* srcrect, const void* dstrect) = nullptr;
    void (*SDL_RenderPresent)(SDL_Renderer* renderer) = nullptr;
    int (*SDL_SetRenderDrawBlendMode)(SDL_Renderer* renderer, int blendMode) = nullptr;

    SDL_Texture* (*SDL_CreateTexture)(SDL_Renderer* renderer, uint32_t format, int access, int w, int h) = nullptr;
    void (*SDL_DestroyTexture)(SDL_Texture* texture) = nullptr;
    int (*SDL_UpdateTexture)(SDL_Texture* texture, const void* rect, const void* pixels, int pitch) = nullptr;
    int (*SDL_SetTextureBlendMode)(SDL_Texture* texture, int blendMode) = nullptr;

    int (*SDL_PollEvent)(void* event) = nullptr;
    int (*SDL_SetClipboardText)(const char* text) = nullptr;
    char* (*SDL_GetClipboardText)() = nullptr;
    int (*SDL_HasClipboardText)() = nullptr;
    void (*SDL_free)(void* mem) = nullptr;
    int (*SDL_ShowCursor)(int toggle) = nullptr;
    uint32_t (*SDL_GetGlobalMouseState)(int* x, int* y) = nullptr;
    uint32_t (*SDL_GetMouseState)(int* x, int* y) = nullptr;
    const uint8_t* (*SDL_GetKeyboardState)(int* numkeys) = nullptr;
    int (*SDL_GetDesktopDisplayMode)(int displayIndex, void* mode) = nullptr;

    Sdl2Api() {
        const char* headlessEnv = std::getenv("APERTURE_HEADLESS");
        if (headlessEnv && std::strcmp(headlessEnv, "1") == 0) return;
        const char* disp = std::getenv("DISPLAY");
        const char* wayland = std::getenv("WAYLAND_DISPLAY");
        if ((!disp || !*disp) && (!wayland || !*wayland)) return;

        for (const char* soName : {"libSDL2-2.0.so.0", "libSDL2.so"}) {
            lib = ::dlopen(soName, RTLD_LAZY | RTLD_LOCAL);
            if (lib) break;
        }
        if (!lib) return;

        #define LOAD_SDL(name) name = reinterpret_cast<decltype(name)>(::dlsym(lib, #name))
        LOAD_SDL(SDL_Init);
        LOAD_SDL(SDL_Quit);
        LOAD_SDL(SDL_SetHint);
        LOAD_SDL(SDL_CreateWindow);
        LOAD_SDL(SDL_DestroyWindow);
        LOAD_SDL(SDL_SetWindowTitle);
        LOAD_SDL(SDL_SetWindowSize);
        LOAD_SDL(SDL_GetWindowSize);
        LOAD_SDL(SDL_SetWindowPosition);
        LOAD_SDL(SDL_GetWindowPosition);
        LOAD_SDL(SDL_ShowWindow);
        LOAD_SDL(SDL_HideWindow);
        LOAD_SDL(SDL_MaximizeWindow);
        LOAD_SDL(SDL_MinimizeWindow);
        LOAD_SDL(SDL_RestoreWindow);
        LOAD_SDL(SDL_RaiseWindow);
        LOAD_SDL(SDL_SetWindowFullscreen);
        LOAD_SDL(SDL_GetWindowFlags);
        LOAD_SDL(SDL_GetWindowID);
        LOAD_SDL(SDL_CreateRenderer);
        LOAD_SDL(SDL_DestroyRenderer);
        LOAD_SDL(SDL_RenderClear);
        LOAD_SDL(SDL_RenderCopy);
        LOAD_SDL(SDL_RenderPresent);
        LOAD_SDL(SDL_SetRenderDrawBlendMode);
        LOAD_SDL(SDL_CreateTexture);
        LOAD_SDL(SDL_DestroyTexture);
        LOAD_SDL(SDL_UpdateTexture);
        LOAD_SDL(SDL_SetTextureBlendMode);
        LOAD_SDL(SDL_PollEvent);
        LOAD_SDL(SDL_SetClipboardText);
        LOAD_SDL(SDL_GetClipboardText);
        LOAD_SDL(SDL_HasClipboardText);
        LOAD_SDL(SDL_free);
        LOAD_SDL(SDL_ShowCursor);
        LOAD_SDL(SDL_GetGlobalMouseState);
        LOAD_SDL(SDL_GetMouseState);
        LOAD_SDL(SDL_GetKeyboardState);
        LOAD_SDL(SDL_GetDesktopDisplayMode);
        #undef LOAD_SDL

        if (SDL_Init && SDL_CreateWindow) {
            // Prefer Wayland when running on Wayland compositor (KDE Plasma / Hyprland / GNOME on CachyOS), fallback to X11
            if (SDL_SetHint) {
                SDL_SetHint("SDL_VIDEODRIVER", "wayland,x11");
            }
            if (SDL_Init(0x00000020u /*SDL_INIT_VIDEO*/ | 0x00004000u /*SDL_INIT_EVENTS*/) == 0) {
                initialized = true;
            }
        }
    }
};

Sdl2Api& GetSdl() {
    static Sdl2Api instance;
    return instance;
}

struct LinuxTimerEntry {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    UINT intervalMs = 100;
    TIMERPROC proc = nullptr;
    ULONGLONG nextFireMs = 0;
};

struct LinuxWindow {
    std::wstring className;
    std::wstring title;
    WNDPROC wndProc = DefWindowProcW;
    DWORD style = WS_OVERLAPPEDWINDOW;
    DWORD exStyle = 0;
    int x = 100;
    int y = 100;
    int width = 1280;
    int height = 720;
    HWND parent = nullptr;
    HINSTANCE hInstance = nullptr;
    LONG_PTR userData = 0;
    LONG_PTR id = 0;
    bool visible = false;
    bool minimized = false;
    bool maximized = false;
    bool destroyed = false;

    void* sdlWindow = nullptr;
    void* sdlRenderer = nullptr;
    void* sdlTexture = nullptr;
    int texW = 0;
    int texH = 0;
    std::vector<uint8_t> frameBuffer; // BGRA
};

std::mutex g_winMutex;
std::map<std::wstring, WNDCLASSEXW> g_classes;
std::vector<LinuxWindow*> g_windows;
std::deque<MSG> g_msgQueue;
std::vector<LinuxTimerEntry> g_timers;
bool g_quitPosted = false;
int g_quitCode = 0;
HWND g_foregroundWnd = nullptr;
HWND g_focusWnd = nullptr;
HWND g_captureWnd = nullptr;
POINT g_cursorPos{640, 360};
SHORT g_keyState[256] = {};

bool IsValidLinuxWindow(HWND hWnd) {
    if (!hWnd) return false;
    auto* w = reinterpret_cast<LinuxWindow*>(hWnd);
    return std::find(g_windows.begin(), g_windows.end(), w) != g_windows.end() && !w->destroyed;
}

LinuxWindow* GetRootLinuxWindow(LinuxWindow* w) {
    while (w && w->parent && IsValidLinuxWindow(w->parent)) {
        w = reinterpret_cast<LinuxWindow*>(w->parent);
    }
    return w;
}

void PresentLinuxWindowBuffer(LinuxWindow* win, const uint8_t* bgraPixels, int w, int h) {
    if (!win || !bgraPixels || w <= 0 || h <= 0) return;
    win->frameBuffer.assign(bgraPixels, bgraPixels + static_cast<size_t>(w) * h * 4);
    LinuxWindow* root = GetRootLinuxWindow(win);
    if (!root || !root->sdlRenderer) return;
    auto& sdl = GetSdl();
    if (!sdl.initialized) return;

    if (!root->sdlTexture || root->texW != w || root->texH != h) {
        if (root->sdlTexture) sdl.SDL_DestroyTexture(root->sdlTexture);
        // SDL_PIXELFORMAT_ARGB8888 = 0x16362004 (on little-endian x86_64, memory order is B,G,R,A)
        root->sdlTexture = sdl.SDL_CreateTexture(root->sdlRenderer, 0x16362004u, 1 /*SDL_TEXTUREACCESS_STREAMING*/, w, h);
        root->texW = w;
        root->texH = h;
        if (root->sdlTexture && sdl.SDL_SetTextureBlendMode) {
            sdl.SDL_SetTextureBlendMode(root->sdlTexture, 1 /*SDL_BLENDMODE_BLEND*/);
        }
    }
    if (root->sdlTexture) {
        sdl.SDL_UpdateTexture(root->sdlTexture, nullptr, bgraPixels, w * 4);
        sdl.SDL_RenderClear(root->sdlRenderer);
        sdl.SDL_RenderCopy(root->sdlRenderer, root->sdlTexture, nullptr, nullptr);
        sdl.SDL_RenderPresent(root->sdlRenderer);
    }
}

struct LinuxDC {
    HWND hwnd = nullptr;
    HBITMAP selectedBmp = nullptr;
};

struct LinuxBitmap {
    int width = 1;
    int height = 1;
    std::vector<uint8_t> pixels;
};

} // namespace

HRESULT IDXGISwapChain1::Present1(UINT /*SyncInterval*/, UINT /*PresentFlags*/, const DXGI_PRESENT_PARAMETERS* /*pPresentParameters*/) {
    if (m_hwnd && m_surface && !m_surface->m_pixels.empty()) {
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (IsValidLinuxWindow(m_hwnd)) {
            auto* win = reinterpret_cast<LinuxWindow*>(m_hwnd);
            PresentLinuxWindowBuffer(win, m_surface->m_pixels.data(), static_cast<int>(m_surface->m_width), static_cast<int>(m_surface->m_height));
        }
    }
    return S_OK;
}

void LinuxBlitDCToBitmap(HDC hdc, const RECT& /*rc*/, ID2D1Bitmap1* bmp) {
    if (!hdc || !bmp) return;
    auto* dc = reinterpret_cast<LinuxDC*>(hdc);
    if (!dc->selectedBmp) return;
    auto* gdiBmp = reinterpret_cast<LinuxBitmap*>(dc->selectedBmp);
    if (gdiBmp->width == static_cast<int>(bmp->m_width) && gdiBmp->height == static_cast<int>(bmp->m_height)) {
        std::memcpy(gdiBmp->pixels.data(), bmp->m_pixels.data(), bmp->m_pixels.size());
    }
}

// ============================================================================
// Window Class & Window Management
// ============================================================================

ATOM RegisterClassExW(const WNDCLASSEXW* lpwcx) {
    if (!lpwcx || !lpwcx->lpszClassName) return 0;
    std::lock_guard<std::mutex> lock(g_winMutex);
    g_classes[lpwcx->lpszClassName] = *lpwcx;
    return static_cast<ATOM>(g_classes.size());
}

HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU /*hMenu*/,
                     HINSTANCE hInstance, LPVOID lpParam) {
    auto* win = new LinuxWindow();
    win->className = lpClassName ? lpClassName : L"";
    win->title = lpWindowName ? lpWindowName : L"Aperture";
    win->style = dwStyle;
    win->exStyle = dwExStyle;
    win->x = (X == static_cast<int>(0x80000000)) ? 100 : X;
    win->y = (Y == static_cast<int>(0x80000000)) ? 100 : Y;
    win->width = (nWidth <= 0 || nWidth == static_cast<int>(0x80000000)) ? 1280 : nWidth;
    win->height = (nHeight <= 0 || nHeight == static_cast<int>(0x80000000)) ? 720 : nHeight;
    win->parent = hWndParent;
    win->hInstance = hInstance;
    win->visible = (dwStyle & WS_VISIBLE) != 0;

    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        auto it = g_classes.find(win->className);
        if (it != g_classes.end() && it->second.lpfnWndProc) {
            win->wndProc = it->second.lpfnWndProc;
        }
        g_windows.push_back(win);
        if (!g_foregroundWnd) g_foregroundWnd = reinterpret_cast<HWND>(win);
        if (!g_focusWnd) g_focusWnd = reinterpret_cast<HWND>(win);
    }

    // Create native SDL2 window for top-level main window when display server is available
    auto& sdl = GetSdl();
    if (sdl.initialized && !hWndParent && !(dwStyle & WS_CHILD) && win->width > 64 && win->height > 64) {
        std::string u8Title = LinuxWideToUtf8(win->title);
        uint32_t sdlFlags = 0x00000020u /*SDL_WINDOW_RESIZABLE*/ | 0x00002000u /*SDL_WINDOW_ALLOW_HIGHDPI*/;
        if (!win->visible) sdlFlags |= 0x00000008u /*SDL_WINDOW_HIDDEN*/;
        win->sdlWindow = sdl.SDL_CreateWindow(u8Title.c_str(), win->x, win->y, win->width, win->height, sdlFlags);
        if (win->sdlWindow && sdl.SDL_CreateRenderer) {
            win->sdlRenderer = sdl.SDL_CreateRenderer(win->sdlWindow, -1, 0x00000002u /*SDL_RENDERER_ACCELERATED*/ | 0x00000004u /*SDL_RENDERER_PRESENTVSYNC*/);
            if (!win->sdlRenderer) {
                win->sdlRenderer = sdl.SDL_CreateRenderer(win->sdlWindow, -1, 0);
            }
        }
    }

    HWND hwnd = reinterpret_cast<HWND>(win);
    CREATESTRUCTW cs{};
    cs.lpCreateParams = lpParam;
    cs.hInstance = hInstance;
    cs.hwndParent = hWndParent;
    cs.x = win->x;
    cs.y = win->y;
    cs.cx = win->width;
    cs.cy = win->height;
    cs.style = static_cast<LONG>(dwStyle);
    cs.lpszName = win->title.c_str();
    cs.lpszClass = win->className.c_str();
    cs.dwExStyle = dwExStyle;

    win->wndProc(hwnd, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&cs));
    win->wndProc(hwnd, WM_CREATE, 0, reinterpret_cast<LPARAM>(&cs));
    win->wndProc(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(win->width, win->height));
    return hwnd;
}

BOOL DestroyWindow(HWND hWnd) {
    LinuxWindow* win = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (!IsValidLinuxWindow(hWnd)) return FALSE;
        win = reinterpret_cast<LinuxWindow*>(hWnd);
        win->destroyed = true;
    }
    win->wndProc(hWnd, WM_DESTROY, 0, 0);
    win->wndProc(hWnd, WM_NCDESTROY, 0, 0);

    std::lock_guard<std::mutex> lock(g_winMutex);
    auto& sdl = GetSdl();
    if (sdl.initialized) {
        if (win->sdlTexture) sdl.SDL_DestroyTexture(win->sdlTexture);
        if (win->sdlRenderer) sdl.SDL_DestroyRenderer(win->sdlRenderer);
        if (win->sdlWindow) sdl.SDL_DestroyWindow(win->sdlWindow);
    }
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), win), g_windows.end());
    delete win;
    return TRUE;
}

BOOL IsWindow(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    return IsValidLinuxWindow(hWnd) ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    return reinterpret_cast<LinuxWindow*>(hWnd)->visible ? TRUE : FALSE;
}

BOOL IsIconic(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    return reinterpret_cast<LinuxWindow*>(hWnd)->minimized ? TRUE : FALSE;
}

BOOL IsZoomed(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    return reinterpret_cast<LinuxWindow*>(hWnd)->maximized ? TRUE : FALSE;
}

BOOL ShowWindow(HWND hWnd, int nCmdShow) {
    LinuxWindow* win = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (!IsValidLinuxWindow(hWnd)) return FALSE;
        win = reinterpret_cast<LinuxWindow*>(hWnd);
        auto& sdl = GetSdl();
        if (nCmdShow == SW_HIDE) {
            win->visible = false;
            if (win->sdlWindow && sdl.SDL_HideWindow) sdl.SDL_HideWindow(win->sdlWindow);
        } else if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE) {
            win->visible = true;
            win->minimized = true;
            win->maximized = false;
            if (win->sdlWindow && sdl.SDL_MinimizeWindow) sdl.SDL_MinimizeWindow(win->sdlWindow);
        } else if (nCmdShow == SW_MAXIMIZE || nCmdShow == SW_SHOWMAXIMIZED) {
            win->visible = true;
            win->minimized = false;
            win->maximized = true;
            if (win->sdlWindow && sdl.SDL_MaximizeWindow) sdl.SDL_MaximizeWindow(win->sdlWindow);
        } else {
            win->visible = true;
            win->minimized = false;
            if (nCmdShow == SW_RESTORE) win->maximized = false;
            if (win->sdlWindow && sdl.SDL_ShowWindow) {
                sdl.SDL_ShowWindow(win->sdlWindow);
                if (nCmdShow == SW_RESTORE && sdl.SDL_RestoreWindow) sdl.SDL_RestoreWindow(win->sdlWindow);
            }
        }
    }
    return TRUE;
}

BOOL SetWindowPos(HWND hWnd, HWND /*hWndInsertAfter*/, int X, int Y, int cx, int cy, UINT uFlags) {
    LinuxWindow* win = nullptr;
    bool sizeChanged = false;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (!IsValidLinuxWindow(hWnd)) return FALSE;
        win = reinterpret_cast<LinuxWindow*>(hWnd);
        auto& sdl = GetSdl();
        if (!(uFlags & SWP_NOMOVE)) {
            win->x = X;
            win->y = Y;
            if (win->sdlWindow && sdl.SDL_SetWindowPosition) sdl.SDL_SetWindowPosition(win->sdlWindow, X, Y);
        }
        if (!(uFlags & SWP_NOSIZE) && cx > 0 && cy > 0) {
            if (win->width != cx || win->height != cy) sizeChanged = true;
            win->width = cx;
            win->height = cy;
            if (win->sdlWindow && sdl.SDL_SetWindowSize) sdl.SDL_SetWindowSize(win->sdlWindow, cx, cy);
        }
        if (uFlags & SWP_SHOWWINDOW) {
            win->visible = true;
            if (win->sdlWindow && sdl.SDL_ShowWindow) sdl.SDL_ShowWindow(win->sdlWindow);
        }
        if (uFlags & SWP_HIDEWINDOW) {
            win->visible = false;
            if (win->sdlWindow && sdl.SDL_HideWindow) sdl.SDL_HideWindow(win->sdlWindow);
        }
    }
    if (sizeChanged && win) {
        win->wndProc(hWnd, WM_SIZE, win->maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, MAKELPARAM(win->width, win->height));
    }
    return TRUE;
}

BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL /*bRepaint*/) {
    return SetWindowPos(hWnd, nullptr, X, Y, nWidth, nHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

BOOL GetClientRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) {
        *lpRect = {0, 0, 1280, 720};
        return FALSE;
    }
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    *lpRect = {0, 0, win->width, win->height};
    return TRUE;
}

BOOL GetWindowRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) {
        *lpRect = {0, 0, 1280, 720};
        return FALSE;
    }
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    *lpRect = {win->x, win->y, win->x + win->width, win->y + win->height};
    return TRUE;
}

BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    lpPoint->x += win->x;
    lpPoint->y += win->y;
    return TRUE;
}

BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    lpPoint->x -= win->x;
    lpPoint->y -= win->y;
    return TRUE;
}

int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
    if (!lpPoints) return 0;
    for (UINT i = 0; i < cPoints; ++i) {
        if (hWndFrom) ClientToScreen(hWndFrom, &lpPoints[i]);
        if (hWndTo) ScreenToClient(hWndTo, &lpPoints[i]);
    }
    return 0;
}

HWND GetParent(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return nullptr;
    return reinterpret_cast<LinuxWindow*>(hWnd)->parent;
}

HWND SetParent(HWND hWndChild, HWND hWndNewParent) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWndChild)) return nullptr;
    auto* win = reinterpret_cast<LinuxWindow*>(hWndChild);
    HWND old = win->parent;
    win->parent = hWndNewParent;
    return old;
}

HWND GetAncestor(HWND hWnd, UINT gaFlags) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return nullptr;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    if (gaFlags == GA_PARENT) return win->parent;
    return reinterpret_cast<HWND>(GetRootLinuxWindow(win));
}

BOOL SetForegroundWindow(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    g_foregroundWnd = hWnd;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    auto& sdl = GetSdl();
    if (win->sdlWindow && sdl.SDL_RaiseWindow) sdl.SDL_RaiseWindow(win->sdlWindow);
    return TRUE;
}

HWND GetForegroundWindow() {
    std::lock_guard<std::mutex> lock(g_winMutex);
    return g_foregroundWnd;
}

HWND SetFocus(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    HWND old = g_focusWnd;
    g_focusWnd = hWnd;
    return old;
}

HWND GetFocus() {
    std::lock_guard<std::mutex> lock(g_winMutex);
    return g_focusWnd;
}

BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    win->title = lpString ? lpString : L"";
    auto& sdl = GetSdl();
    if (win->sdlWindow && sdl.SDL_SetWindowTitle) {
        std::string u8 = LinuxWideToUtf8(win->title);
        sdl.SDL_SetWindowTitle(win->sdlWindow, u8.c_str());
    }
    return TRUE;
}

int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount) {
    if (!lpString || nMaxCount <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) { lpString[0] = L'\0'; return 0; }
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    wcscpy_s(lpString, static_cast<size_t>(nMaxCount), win->title.c_str());
    return static_cast<int>(std::wcslen(lpString));
}

LONG_PTR GetWindowLongPtrW(HWND hWnd, int nIndex) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return 0;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    switch (nIndex) {
        case GWLP_USERDATA: return win->userData;
        case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(win->wndProc);
        case GWLP_HINSTANCE: return reinterpret_cast<LONG_PTR>(win->hInstance);
        case GWLP_HWNDPARENT: return reinterpret_cast<LONG_PTR>(win->parent);
        case GWLP_ID: return win->id;
        case GWL_STYLE: return static_cast<LONG_PTR>(win->style);
        case GWL_EXSTYLE: return static_cast<LONG_PTR>(win->exStyle);
        default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return 0;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    LONG_PTR old = 0;
    switch (nIndex) {
        case GWLP_USERDATA: old = win->userData; win->userData = dwNewLong; break;
        case GWLP_WNDPROC: old = reinterpret_cast<LONG_PTR>(win->wndProc); win->wndProc = reinterpret_cast<WNDPROC>(dwNewLong); break;
        case GWLP_ID: old = win->id; win->id = dwNewLong; break;
        case GWL_STYLE: old = static_cast<LONG_PTR>(win->style); win->style = static_cast<DWORD>(dwNewLong); break;
        case GWL_EXSTYLE: old = static_cast<LONG_PTR>(win->exStyle); win->exStyle = static_cast<DWORD>(dwNewLong); break;
        default: break;
    }
    return old;
}

BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl) {
    if (!lpwndpl) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (!IsValidLinuxWindow(hWnd)) return FALSE;
    auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
    std::memset(lpwndpl, 0, sizeof(*lpwndpl));
    lpwndpl->length = sizeof(*lpwndpl);
    lpwndpl->showCmd = win->minimized ? SW_SHOWMINIMIZED : (win->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    lpwndpl->rcNormalPosition = {win->x, win->y, win->x + win->width, win->y + win->height};
    return TRUE;
}

BOOL SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl) {
    if (!lpwndpl) return FALSE;
    const RECT& r = lpwndpl->rcNormalPosition;
    SetWindowPos(hWnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hWnd, static_cast<int>(lpwndpl->showCmd));
    return TRUE;
}

BOOL AdjustWindowRectEx(LPRECT /*lpRect*/, DWORD /*dwStyle*/, BOOL /*bMenu*/, DWORD /*dwExStyle*/) { return TRUE; }
BOOL AdjustWindowRectExForDpi(LPRECT /*lpRect*/, DWORD /*dwStyle*/, BOOL /*bMenu*/, DWORD /*dwExStyle*/, UINT /*dpi*/) { return TRUE; }

// ============================================================================
// Message Loop & SDL2 Event Pumping
// ============================================================================

LRESULT DefWindowProcW(HWND /*hWnd*/, UINT /*Msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    return 0;
}

BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (Msg == WM_QUIT) {
        g_quitPosted = true;
        g_quitCode = static_cast<int>(wParam);
    }
    MSG m{};
    m.hwnd = hWnd;
    m.message = Msg;
    m.wParam = wParam;
    m.lParam = lParam;
    m.time = GetTickCount();
    m.pt = g_cursorPos;
    g_msgQueue.push_back(m);
    return TRUE;
}

LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (!IsValidLinuxWindow(hWnd)) return 0;
        proc = reinterpret_cast<LinuxWindow*>(hWnd)->wndProc;
    }
    return proc ? proc(hWnd, Msg, wParam, lParam) : 0;
}

LRESULT SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, UINT /*fuFlags*/, UINT /*uTimeout*/, DWORD_PTR* lpdwResult) {
    LRESULT r = SendMessageW(hWnd, Msg, wParam, lParam);
    if (lpdwResult) *lpdwResult = static_cast<DWORD_PTR>(r);
    return 1;
}

void PostQuitMessage(int nExitCode) {
    PostMessageW(nullptr, WM_QUIT, static_cast<WPARAM>(nExitCode), 0);
}

UINT RegisterWindowMessageW(LPCWSTR lpString) {
    static std::mutex s_mtx;
    static std::map<std::wstring, UINT> s_msgs;
    static UINT s_next = 0xC000;
    std::lock_guard<std::mutex> lock(s_mtx);
    std::wstring key = lpString ? lpString : L"";
    auto it = s_msgs.find(key);
    if (it != s_msgs.end()) return it->second;
    UINT id = s_next++;
    s_msgs[key] = id;
    return id;
}

static int MapSdlKeycodeToVk(int sym) {
    if (sym >= 'a' && sym <= 'z') return sym - 'a' + 'A';
    if (sym >= '0' && sym <= '9') return sym;
    switch (sym) {
        case 8: return VK_BACK;
        case 9: return VK_TAB;
        case 13: return VK_RETURN;
        case 27: return VK_ESCAPE;
        case 32: return VK_SPACE;
        case 127: return VK_DELETE;
        case 0x4000003A: return VK_F1;
        case 0x4000003B: return VK_F2;
        case 0x4000003C: return VK_F3;
        case 0x4000003D: return VK_F4;
        case 0x4000003E: return VK_F5;
        case 0x4000003F: return VK_F6;
        case 0x40000040: return VK_F7;
        case 0x40000041: return VK_F8;
        case 0x40000042: return VK_F9;
        case 0x40000043: return VK_F10;
        case 0x40000044: return VK_F11;
        case 0x40000045: return VK_F12;
        case 0x40000049: return VK_INSERT;
        case 0x4000004A: return VK_HOME;
        case 0x4000004B: return VK_PRIOR;
        case 0x4000004D: return VK_END;
        case 0x4000004E: return VK_NEXT;
        case 0x4000004F: return VK_RIGHT;
        case 0x40000050: return VK_LEFT;
        case 0x40000051: return VK_DOWN;
        case 0x40000052: return VK_UP;
        case 0x400000E0: return VK_CONTROL;
        case 0x400000E1: return VK_SHIFT;
        case 0x400000E2: return VK_MENU;
        case 0x400000E4: return VK_CONTROL;
        case 0x400000E5: return VK_SHIFT;
        case 0x400000E6: return VK_MENU;
        case 0x40000102: return VK_MEDIA_NEXT_TRACK;
        case 0x40000103: return VK_MEDIA_PREV_TRACK;
        case 0x40000104: return VK_MEDIA_STOP;
        case 0x40000105: return VK_MEDIA_PLAY_PAUSE;
        case 0x40000106: return VK_VOLUME_MUTE;
        default: return 0;
    }
}

struct LinuxDropFiles {
    std::vector<std::wstring> files;
};

static void PumpSdlEventsAndTimers() {
    auto& sdl = GetSdl();
    HWND mainHwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        for (auto* w : g_windows) {
            if (w->sdlWindow && !w->destroyed) {
                mainHwnd = reinterpret_cast<HWND>(w);
                break;
            }
        }
        if (!mainHwnd && !g_windows.empty()) {
            mainHwnd = reinterpret_cast<HWND>(g_windows.front());
        }
    }

    if (sdl.initialized && sdl.SDL_PollEvent && mainHwnd) {
        alignas(8) uint8_t ev[64] = {};
        while (sdl.SDL_PollEvent(ev)) {
            uint32_t type = *reinterpret_cast<const uint32_t*>(ev);
            if (type == 0x100 /*SDL_QUIT*/) {
                PostMessageW(mainHwnd, WM_CLOSE, 0, 0);
            } else if (type == 0x200 /*SDL_WINDOWEVENT*/) {
                uint8_t wev = ev[12];
                int32_t d1 = *reinterpret_cast<const int32_t*>(ev + 16);
                int32_t d2 = *reinterpret_cast<const int32_t*>(ev + 20);
                if (wev == 5 /*SDL_WINDOWEVENT_RESIZED*/ || wev == 6 /*SDL_WINDOWEVENT_SIZE_CHANGED*/) {
                    {
                        std::lock_guard<std::mutex> lock(g_winMutex);
                        if (IsValidLinuxWindow(mainHwnd)) {
                            auto* mw = reinterpret_cast<LinuxWindow*>(mainHwnd);
                            mw->width = d1;
                            mw->height = d2;
                        }
                    }
                    PostMessageW(mainHwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(d1, d2));
                    PostMessageW(mainHwnd, WM_PAINT, 0, 0);
                } else if (wev == 14 /*SDL_WINDOWEVENT_CLOSE*/) {
                    PostMessageW(mainHwnd, WM_CLOSE, 0, 0);
                } else if (wev == 1 /*SDL_WINDOWEVENT_SHOWN*/ || wev == 2 /*SDL_WINDOWEVENT_EXPOSED*/) {
                    PostMessageW(mainHwnd, WM_PAINT, 0, 0);
                }
            } else if (type == 0x400 /*SDL_MOUSEMOTION*/) {
                int32_t mx = *reinterpret_cast<const int32_t*>(ev + 20);
                int32_t my = *reinterpret_cast<const int32_t*>(ev + 24);
                g_cursorPos = {mx, my};
                PostMessageW(mainHwnd, WM_MOUSEMOVE, 0, MAKELPARAM(mx, my));
            } else if (type == 0x401 /*SDL_MOUSEBUTTONDOWN*/ || type == 0x402 /*SDL_MOUSEBUTTONUP*/) {
                uint8_t btn = ev[16];
                uint8_t clicks = ev[18];
                int32_t mx = *reinterpret_cast<const int32_t*>(ev + 20);
                int32_t my = *reinterpret_cast<const int32_t*>(ev + 24);
                g_cursorPos = {mx, my};
                bool down = (type == 0x401);
                if (btn == 1) {
                    g_keyState[VK_LBUTTON] = down ? static_cast<SHORT>(0x8000) : 0;
                    UINT msg = down ? (clicks == 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN) : WM_LBUTTONUP;
                    PostMessageW(mainHwnd, msg, down ? MK_LBUTTON : 0, MAKELPARAM(mx, my));
                } else if (btn == 3) {
                    g_keyState[VK_RBUTTON] = down ? static_cast<SHORT>(0x8000) : 0;
                    PostMessageW(mainHwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, MAKELPARAM(mx, my));
                } else if (btn == 2) {
                    g_keyState[VK_MBUTTON] = down ? static_cast<SHORT>(0x8000) : 0;
                    PostMessageW(mainHwnd, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, down ? MK_MBUTTON : 0, MAKELPARAM(mx, my));
                }
            } else if (type == 0x403 /*SDL_MOUSEWHEEL*/) {
                int32_t wy = *reinterpret_cast<const int32_t*>(ev + 20);
                if (wy != 0) {
                    PostMessageW(mainHwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, wy * WHEEL_DELTA), MAKELPARAM(g_cursorPos.x, g_cursorPos.y));
                }
            } else if (type == 0x300 /*SDL_KEYDOWN*/ || type == 0x301 /*SDL_KEYUP*/) {
                int32_t sym = *reinterpret_cast<const int32_t*>(ev + 20);
                int vk = MapSdlKeycodeToVk(sym);
                if (vk != 0) {
                    bool down = (type == 0x300);
                    g_keyState[vk & 0xFF] = down ? static_cast<SHORT>(0x8000) : 0;
                    PostMessageW(mainHwnd, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), 0);
                }
            } else if (type == 0x303 /*SDL_TEXTINPUT*/) {
                const char* txt = reinterpret_cast<const char*>(ev + 12);
                std::wstring wtxt = LinuxUtf8ToWide(txt);
                for (wchar_t ch : wtxt) {
                    PostMessageW(mainHwnd, WM_CHAR, static_cast<WPARAM>(ch), 0);
                }
            } else if (type == 0x1000 /*SDL_DROPFILE*/) {
                char* dropped = *reinterpret_cast<char* const*>(ev + 8);
                if (dropped) {
                    auto* df = new LinuxDropFiles();
                    df->files.push_back(LinuxPosixPathToWin(dropped));
                    if (sdl.SDL_free) sdl.SDL_free(dropped);
                    PostMessageW(mainHwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(df), 0);
                }
            }
        }
    }

    // Fire due timers
    ULONGLONG now = GetTickCount64();
    std::vector<LinuxTimerEntry> due;
    {
        std::lock_guard<std::mutex> lock(g_winMutex);
        for (auto& t : g_timers) {
            if (now >= t.nextFireMs) {
                t.nextFireMs = now + std::max<UINT>(1, t.intervalMs);
                due.push_back(t);
            }
        }
    }
    for (const auto& t : due) {
        if (t.proc) {
            t.proc(t.hwnd, WM_TIMER, t.id, static_cast<DWORD>(now));
        } else {
            PostMessageW(t.hwnd, WM_TIMER, t.id, 0);
        }
    }
}

BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT /*wMsgFilterMin*/, UINT /*wMsgFilterMax*/, UINT wRemoveMsg) {
    if (!lpMsg) return FALSE;
    PumpSdlEventsAndTimers();
    std::lock_guard<std::mutex> lock(g_winMutex);
    for (auto it = g_msgQueue.begin(); it != g_msgQueue.end(); ++it) {
        if (!hWnd || it->hwnd == hWnd) {
            *lpMsg = *it;
            if (wRemoveMsg & PM_REMOVE) {
                g_msgQueue.erase(it);
            }
            return TRUE;
        }
    }
    return FALSE;
}

BOOL GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    if (!lpMsg) return FALSE;
    while (true) {
        if (PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, PM_REMOVE)) {
            return (lpMsg->message != WM_QUIT) ? TRUE : FALSE;
        }
        {
            std::lock_guard<std::mutex> lock(g_winMutex);
            if (g_quitPosted) {
                lpMsg->hwnd = nullptr;
                lpMsg->message = WM_QUIT;
                lpMsg->wParam = static_cast<WPARAM>(g_quitCode);
                lpMsg->lParam = 0;
                return FALSE;
            }
        }
        Sleep(4);
    }
}

BOOL TranslateMessage(const MSG* /*lpMsg*/) { return TRUE; }

LRESULT DispatchMessageW(const MSG* lpMsg) {
    if (!lpMsg || !lpMsg->hwnd) return 0;
    return SendMessageW(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    ULONGLONG now = GetTickCount64();
    for (auto& t : g_timers) {
        if (t.hwnd == hWnd && t.id == nIDEvent) {
            t.intervalMs = std::max<UINT>(1, uElapse);
            t.proc = lpTimerFunc;
            t.nextFireMs = now + t.intervalMs;
            return nIDEvent;
        }
    }
    LinuxTimerEntry entry{};
    entry.hwnd = hWnd;
    entry.id = nIDEvent ? nIDEvent : (g_timers.size() + 1);
    entry.intervalMs = std::max<UINT>(1, uElapse);
    entry.proc = lpTimerFunc;
    entry.nextFireMs = now + entry.intervalMs;
    g_timers.push_back(entry);
    return entry.id;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    auto it = std::remove_if(g_timers.begin(), g_timers.end(), [&](const LinuxTimerEntry& t) {
        return t.hwnd == hWnd && t.id == uIDEvent;
    });
    if (it == g_timers.end()) return FALSE;
    g_timers.erase(it, g_timers.end());
    return TRUE;
}

BOOL InvalidateRect(HWND hWnd, const RECT* /*lpRect*/, BOOL /*bErase*/) {
    PostMessageW(hWnd, WM_PAINT, 0, 0);
    return TRUE;
}

BOOL UpdateWindow(HWND hWnd) {
    SendMessageW(hWnd, WM_PAINT, 0, 0);
    return TRUE;
}

// ============================================================================
// Input, Monitors, Metrics, Rects, GDI
// ============================================================================

SHORT GetKeyState(int nVirtKey) {
    if (nVirtKey < 0 || nVirtKey >= 256) return 0;
    return g_keyState[nVirtKey];
}

SHORT GetAsyncKeyState(int nVirtKey) {
    return GetKeyState(nVirtKey);
}

BOOL GetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    *lpPoint = g_cursorPos;
    return TRUE;
}

HCURSOR LoadCursorW(HINSTANCE /*hInstance*/, LPCWSTR lpCursorName) {
    return reinterpret_cast<HCURSOR>(const_cast<LPWSTR>(lpCursorName));
}

HCURSOR SetCursor(HCURSOR hCursor) { return hCursor; }

int ShowCursor(BOOL bShow) {
    auto& sdl = GetSdl();
    if (sdl.initialized && sdl.SDL_ShowCursor) {
        sdl.SDL_ShowCursor(bShow ? 1 : 0);
    }
    return bShow ? 0 : -1;
}

HWND SetCapture(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMutex);
    HWND old = g_captureWnd;
    g_captureWnd = hWnd;
    return old;
}

BOOL ReleaseCapture() {
    std::lock_guard<std::mutex> lock(g_winMutex);
    g_captureWnd = nullptr;
    return TRUE;
}

HWND GetCapture() {
    std::lock_guard<std::mutex> lock(g_winMutex);
    return g_captureWnd;
}

BOOL TrackMouseEvent(LPTRACKMOUSEEVENT /*lpEventTrack*/) { return TRUE; }
UINT GetDoubleClickTime() { return 500; }

HMONITOR MonitorFromWindow(HWND /*hwnd*/, DWORD /*dwFlags*/) { return reinterpret_cast<HMONITOR>(0x3001); }
HMONITOR MonitorFromPoint(POINT /*pt*/, DWORD /*dwFlags*/) { return reinterpret_cast<HMONITOR>(0x3001); }
HMONITOR MonitorFromRect(LPCRECT /*lprc*/, DWORD /*dwFlags*/) { return reinterpret_cast<HMONITOR>(0x3001); }

BOOL GetMonitorInfoW(HMONITOR /*hMonitor*/, LPMONITORINFO lpmi) {
    if (!lpmi) return FALSE;
    int w = 1920, h = 1080;
    auto& sdl = GetSdl();
    if (sdl.initialized && sdl.SDL_GetDesktopDisplayMode) {
        struct SdlDisplayMode { uint32_t fmt; int w; int h; int refresh; void* drv; } mode{};
        if (sdl.SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
            w = mode.w;
            h = mode.h;
        }
    }
    lpmi->rcMonitor = {0, 0, w, h};
    lpmi->rcWork = {0, 0, w, h};
    lpmi->dwFlags = 1; // MONITORINFOF_PRIMARY
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC /*hdc*/, LPCRECT /*lprcClip*/, MONITORENUMPROC lpfnEnum, LPARAM dwData) {
    if (!lpfnEnum) return FALSE;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(reinterpret_cast<HMONITOR>(0x3001), &mi);
    lpfnEnum(reinterpret_cast<HMONITOR>(0x3001), nullptr, &mi.rcMonitor, dwData);
    return TRUE;
}

UINT GetDpiForWindow(HWND /*hwnd*/) { return 96; }
UINT GetDpiForSystem() { return 96; }

int GetSystemMetrics(int nIndex) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(reinterpret_cast<HMONITOR>(0x3001), &mi);
    switch (nIndex) {
        case SM_CXSCREEN:
        case SM_CXFULLSCREEN:
        case SM_CXVIRTUALSCREEN: return mi.rcMonitor.right;
        case SM_CYSCREEN:
        case SM_CYFULLSCREEN:
        case SM_CYVIRTUALSCREEN: return mi.rcMonitor.bottom;
        case SM_CMONITORS: return 1;
        case SM_CXDOUBLECLK:
        case SM_CYDOUBLECLK: return 4;
        case SM_CXICON:
        case SM_CYICON: return 32;
        case SM_CXSMICON:
        case SM_CYSMICON: return 16;
        case SM_CXSIZEFRAME:
        case SM_CYSIZEFRAME: return 4;
        case SM_CYCAPTION: return 32;
        default: return 0;
    }
}

int GetSystemMetricsForDpi(int nIndex, UINT /*dpi*/) { return GetSystemMetrics(nIndex); }

BOOL SystemParametersInfoW(UINT uiAction, UINT /*uiParam*/, PVOID pvParam, UINT /*fWinIni*/) {
    if (!pvParam) return FALSE;
    if (uiAction == SPI_GETWORKAREA) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(reinterpret_cast<HMONITOR>(0x3001), &mi);
        *static_cast<RECT*>(pvParam) = mi.rcWork;
        return TRUE;
    }
    if (uiAction == SPI_GETWHEELSCROLLLINES || uiAction == SPI_GETWHEELSCROLLCHARS) {
        *static_cast<UINT*>(pvParam) = 3;
        return TRUE;
    }
    if (uiAction == SPI_GETCLIENTAREAANIMATION) {
        *static_cast<BOOL*>(pvParam) = TRUE;
        return TRUE;
    }
    if (uiAction == SPI_GETHIGHCONTRAST) {
        auto* hc = static_cast<HIGHCONTRASTW*>(pvParam);
        hc->dwFlags = 0;
        return TRUE;
    }
    return TRUE;
}

DWORD GetSysColor(int /*nIndex*/) { return RGB(32, 32, 32); }

BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom) {
    if (!lprc) return FALSE;
    lprc->left = xLeft; lprc->top = yTop; lprc->right = xRight; lprc->bottom = yBottom;
    return TRUE;
}

BOOL SetRectEmpty(LPRECT lprc) {
    if (!lprc) return FALSE;
    *lprc = {0, 0, 0, 0};
    return TRUE;
}

BOOL IsRectEmpty(const RECT* lprc) {
    if (!lprc) return TRUE;
    return (lprc->left >= lprc->right || lprc->top >= lprc->bottom) ? TRUE : FALSE;
}

BOOL EqualRect(const RECT* lprc1, const RECT* lprc2) {
    if (!lprc1 || !lprc2) return FALSE;
    return (lprc1->left == lprc2->left && lprc1->top == lprc2->top &&
            lprc1->right == lprc2->right && lprc1->bottom == lprc2->bottom) ? TRUE : FALSE;
}

BOOL PtInRect(const RECT* lprc, POINT pt) {
    if (!lprc) return FALSE;
    return (pt.x >= lprc->left && pt.x < lprc->right && pt.y >= lprc->top && pt.y < lprc->bottom) ? TRUE : FALSE;
}

BOOL IntersectRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2) {
    if (!lprcDst || !lprcSrc1 || !lprcSrc2) return FALSE;
    lprcDst->left = std::max(lprcSrc1->left, lprcSrc2->left);
    lprcDst->top = std::max(lprcSrc1->top, lprcSrc2->top);
    lprcDst->right = std::min(lprcSrc1->right, lprcSrc2->right);
    lprcDst->bottom = std::min(lprcSrc1->bottom, lprcSrc2->bottom);
    if (lprcDst->left >= lprcDst->right || lprcDst->top >= lprcDst->bottom) {
        *lprcDst = {0, 0, 0, 0};
        return FALSE;
    }
    return TRUE;
}

BOOL UnionRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2) {
    if (!lprcDst || !lprcSrc1 || !lprcSrc2) return FALSE;
    lprcDst->left = std::min(lprcSrc1->left, lprcSrc2->left);
    lprcDst->top = std::min(lprcSrc1->top, lprcSrc2->top);
    lprcDst->right = std::max(lprcSrc1->right, lprcSrc2->right);
    lprcDst->bottom = std::max(lprcSrc1->bottom, lprcSrc2->bottom);
    return TRUE;
}

BOOL OffsetRect(LPRECT lprc, int dx, int dy) {
    if (!lprc) return FALSE;
    lprc->left += dx; lprc->right += dx;
    lprc->top += dy; lprc->bottom += dy;
    return TRUE;
}

BOOL InflateRect(LPRECT lprc, int dx, int dy) {
    if (!lprc) return FALSE;
    lprc->left -= dx; lprc->right += dx;
    lprc->top -= dy; lprc->bottom += dy;
    return TRUE;
}

HDC GetDC(HWND hWnd) {
    auto* dc = new LinuxDC();
    dc->hwnd = hWnd;
    return reinterpret_cast<HDC>(dc);
}

int ReleaseDC(HWND /*hWnd*/, HDC hDC) {
    if (hDC) delete reinterpret_cast<LinuxDC*>(hDC);
    return 1;
}

int GetDeviceCaps(HDC /*hdc*/, int /*index*/) { return 96; }

HDC CreateCompatibleDC(HDC /*hdc*/) {
    return reinterpret_cast<HDC>(new LinuxDC());
}

BOOL DeleteDC(HDC hdc) {
    if (hdc) delete reinterpret_cast<LinuxDC*>(hdc);
    return TRUE;
}

HBITMAP CreateDIBSection(HDC /*hdc*/, const BITMAPINFO* pbmi, UINT /*usage*/, void** ppvBits, HANDLE /*hSection*/, DWORD /*offset*/) {
    int w = pbmi ? std::max<int>(1, pbmi->bmiHeader.biWidth) : 1;
    int h = pbmi ? std::max<int>(1, std::abs(pbmi->bmiHeader.biHeight)) : 1;
    auto* bmp = new LinuxBitmap();
    bmp->width = w;
    bmp->height = h;
    bmp->pixels.resize(static_cast<size_t>(w) * h * 4, 0);
    if (ppvBits) *ppvBits = bmp->pixels.data();
    return reinterpret_cast<HBITMAP>(bmp);
}

HBITMAP CreateBitmap(int nWidth, int nHeight, UINT /*nPlanes*/, UINT /*nBitCount*/, const void* /*lpBits*/) {
    auto* bmp = new LinuxBitmap();
    bmp->width = std::max(1, nWidth);
    bmp->height = std::max(1, nHeight);
    bmp->pixels.resize(static_cast<size_t>(bmp->width) * bmp->height * 4, 0);
    return reinterpret_cast<HBITMAP>(bmp);
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    if (!hdc) return nullptr;
    auto* dc = reinterpret_cast<LinuxDC*>(hdc);
    HGDIOBJ old = dc->selectedBmp;
    dc->selectedBmp = reinterpret_cast<HBITMAP>(h);
    return old;
}

BOOL DeleteObject(HGDIOBJ ho) {
    if (ho && ho != reinterpret_cast<HGDIOBJ>(0x4001)) {
        delete reinterpret_cast<LinuxBitmap*>(ho);
    }
    return TRUE;
}

HBRUSH CreateSolidBrush(COLORREF /*color*/) { return reinterpret_cast<HBRUSH>(0x4001); }
int FillRect(HDC /*hDC*/, const RECT* /*lprc*/, HBRUSH /*hbr*/) { return 1; }

HDC BeginPaint(HWND hWnd, PAINTSTRUCT* lpPaint) {
    if (lpPaint) {
        std::memset(lpPaint, 0, sizeof(*lpPaint));
        GetClientRect(hWnd, &lpPaint->rcPaint);
        lpPaint->hdc = GetDC(hWnd);
        return lpPaint->hdc;
    }
    return GetDC(hWnd);
}

BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint) {
    if (lpPaint && lpPaint->hdc) ReleaseDC(hWnd, lpPaint->hdc);
    return TRUE;
}

BOOL UpdateLayeredWindow(HWND hWnd, HDC /*hdcDst*/, POINT* /*pptDst*/, SIZE* psize,
                         HDC hdcSrc, POINT* /*pptSrc*/, COLORREF /*crKey*/, BLENDFUNCTION* /*pblend*/, DWORD /*dwFlags*/) {
    if (!hWnd || !hdcSrc) return TRUE;
    auto* dc = reinterpret_cast<LinuxDC*>(hdcSrc);
    if (!dc->selectedBmp) return TRUE;
    auto* bmp = reinterpret_cast<LinuxBitmap*>(dc->selectedBmp);
    std::lock_guard<std::mutex> lock(g_winMutex);
    if (IsValidLinuxWindow(hWnd)) {
        auto* win = reinterpret_cast<LinuxWindow*>(hWnd);
        int w = psize ? psize->cx : bmp->width;
        int h = psize ? psize->cy : bmp->height;
        PresentLinuxWindowBuffer(win, bmp->pixels.data(), w, h);
    }
    return TRUE;
}

HRESULT DwmSetWindowAttribute(HWND /*hwnd*/, DWORD /*dwAttribute*/, LPCVOID /*pvAttribute*/, DWORD /*cbAttribute*/) { return S_OK; }
HRESULT DwmExtendFrameIntoClientArea(HWND /*hWnd*/, const MARGINS* /*pMarInset*/) { return S_OK; }
HRESULT DwmFlush() { return S_OK; }

// ============================================================================
// Clipboard & Global Memory
// ============================================================================

struct LinuxGlobalMem {
    std::vector<uint8_t> data;
};

static std::wstring g_clipboardText;

HGLOBAL GlobalAlloc(UINT /*uFlags*/, SIZE_T dwBytes) {
    auto* m = new LinuxGlobalMem();
    m->data.resize(std::max<SIZE_T>(1, dwBytes), 0);
    return reinterpret_cast<HGLOBAL>(m);
}

LPVOID GlobalLock(HGLOBAL hMem) {
    if (!hMem) return nullptr;
    return reinterpret_cast<LinuxGlobalMem*>(hMem)->data.data();
}

BOOL GlobalUnlock(HGLOBAL /*hMem*/) { return TRUE; }

SIZE_T GlobalSize(HGLOBAL hMem) {
    if (!hMem) return 0;
    return reinterpret_cast<LinuxGlobalMem*>(hMem)->data.size();
}

HGLOBAL GlobalFree(HGLOBAL hMem) {
    if (hMem) delete reinterpret_cast<LinuxGlobalMem*>(hMem);
    return nullptr;
}

BOOL OpenClipboard(HWND /*hWndNewOwner*/) { return TRUE; }
BOOL CloseClipboard() { return TRUE; }
BOOL EmptyClipboard() { g_clipboardText.clear(); return TRUE; }

BOOL IsClipboardFormatAvailable(UINT format) {
    if (format == CF_UNICODETEXT || format == CF_TEXT) {
        auto& sdl = GetSdl();
        if (sdl.initialized && sdl.SDL_HasClipboardText && sdl.SDL_HasClipboardText()) return TRUE;
        return !g_clipboardText.empty() ? TRUE : FALSE;
    }
    return FALSE;
}

HANDLE GetClipboardData(UINT uFormat) {
    if (uFormat != CF_UNICODETEXT) return nullptr;
    auto& sdl = GetSdl();
    if (sdl.initialized && sdl.SDL_GetClipboardText) {
        char* txt = sdl.SDL_GetClipboardText();
        if (txt) {
            g_clipboardText = LinuxUtf8ToWide(txt);
            if (sdl.SDL_free) sdl.SDL_free(txt);
        }
    }
    size_t bytes = (g_clipboardText.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void* ptr = GlobalLock(h);
    std::memcpy(ptr, g_clipboardText.c_str(), bytes);
    return h;
}

HANDLE SetClipboardData(UINT uFormat, HANDLE hMem) {
    if (uFormat == CF_UNICODETEXT && hMem) {
        const wchar_t* wstr = static_cast<const wchar_t*>(GlobalLock(hMem));
        if (wstr) {
            g_clipboardText = wstr;
            auto& sdl = GetSdl();
            if (sdl.initialized && sdl.SDL_SetClipboardText) {
                std::string u8 = LinuxWideToUtf8(g_clipboardText);
                sdl.SDL_SetClipboardText(u8.c_str());
            }
        }
    }
    return hMem;
}

// ============================================================================
// Drag & Drop, Shell, Native File Dialogs, Menus, Touch, IME
// ============================================================================

UINT DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch) {
    if (!hDrop) return 0;
    auto* df = reinterpret_cast<LinuxDropFiles*>(hDrop);
    if (iFile == 0xFFFFFFFF) return static_cast<UINT>(df->files.size());
    if (iFile >= df->files.size()) return 0;
    const auto& f = df->files[iFile];
    if (!lpszFile) return static_cast<UINT>(f.size());
    wcscpy_s(lpszFile, cch, f.c_str());
    return static_cast<UINT>(f.size());
}

void DragFinish(HDROP hDrop) {
    if (hDrop) delete reinterpret_cast<LinuxDropFiles*>(hDrop);
}

void DragAcceptFiles(HWND /*hWnd*/, BOOL /*fAccept*/) {}

HINSTANCE ShellExecuteW(HWND /*hwnd*/, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR /*lpDirectory*/, int /*nShowCmd*/) {
    if (!lpFile) return reinterpret_cast<HINSTANCE>(33);
    std::string file = LinuxWinPathToPosix(lpFile);
    std::string params = lpParameters ? LinuxWideToUtf8(lpParameters) : "";
    std::string op = lpOperation ? LinuxWideToUtf8(lpOperation) : "open";

    pid_t pid = ::fork();
    if (pid == 0) {
        if (op == "open" && (file.rfind("http://", 0) == 0 || file.rfind("https://", 0) == 0 || params.empty())) {
            ::execlp("xdg-open", "xdg-open", file.c_str(), static_cast<char*>(nullptr));
        }
        if (!params.empty()) {
            std::string cmd = "\"" + file + "\" " + params;
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        } else {
            ::execl(file.c_str(), file.c_str(), static_cast<char*>(nullptr));
        }
        ::_exit(0);
    }
    return reinterpret_cast<HINSTANCE>(42);
}

BOOL ShellExecuteExW(SHELLEXECUTEINFOW* pExecInfo) {
    if (!pExecInfo) return FALSE;
    ShellExecuteW(pExecInfo->hwnd, pExecInfo->lpVerb, pExecInfo->lpFile, pExecInfo->lpParameters, pExecInfo->lpDirectory, pExecInfo->nShow);
    return TRUE;
}

BOOL Shell_NotifyIconW(DWORD /*dwMessage*/, NOTIFYICONDATAW* /*lpData*/) { return TRUE; }
BOOL RegisterHotKey(HWND /*hWnd*/, int /*id*/, UINT /*fsModifiers*/, UINT /*vk*/) { return TRUE; }
BOOL UnregisterHotKey(HWND /*hWnd*/, int /*id*/) { return TRUE; }

HANDLE LoadImageW(HINSTANCE /*hInst*/, LPCWSTR /*name*/, UINT /*type*/, int /*cx*/, int /*cy*/, UINT /*fuLoad*/) {
    return reinterpret_cast<HANDLE>(0x5001);
}
HICON CreateIconIndirect(ICONINFO* /*piconinfo*/) { return reinterpret_cast<HICON>(0x5002); }
BOOL DestroyIcon(HICON /*hIcon*/) { return TRUE; }
HMENU CreatePopupMenu() { return reinterpret_cast<HMENU>(0x6001); }
BOOL AppendMenuW(HMENU /*hMenu*/, UINT /*uFlags*/, UINT_PTR /*uIDNewItem*/, LPCWSTR /*lpNewItem*/) { return TRUE; }
BOOL TrackPopupMenu(HMENU /*hMenu*/, UINT /*uFlags*/, int /*x*/, int /*y*/, int /*nReserved*/, HWND /*hWnd*/, const RECT* /*prcRect*/) { return 0; }
BOOL DestroyMenu(HMENU /*hMenu*/) { return TRUE; }
int MessageBoxW(HWND /*hWnd*/, LPCWSTR /*lpText*/, LPCWSTR /*lpCaption*/, UINT /*uType*/) { return IDOK; }

static std::string RunDialogCommand(const std::string& cmd) {
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        out += buf;
    }
    int status = ::pclose(pipe);
    if (status != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

HRESULT IShellItem::GetDisplayName(SIGDN /*sigdnName*/, LPWSTR* ppszName) {
    if (!ppszName) return E_POINTER;
    size_t bytes = (m_path.size() + 1) * sizeof(wchar_t);
    *ppszName = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!*ppszName) return E_OUTOFMEMORY;
    std::memcpy(*ppszName, m_path.c_str(), bytes);
    return S_OK;
}

HRESULT IFileDialog::Show(HWND /*hwndOwner*/) {
    bool pickFolder = (m_options & FOS_PICKFOLDERS) != 0;
    std::string out;
    if (pickFolder) {
        out = RunDialogCommand("kdialog --getexistingdirectory . 2>/dev/null || zenity --file-selection --directory 2>/dev/null");
    } else {
        out = RunDialogCommand("kdialog --getopenfilename . 2>/dev/null || zenity --file-selection 2>/dev/null");
    }
    if (out.empty()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    m_resultPath = LinuxPosixPathToWin(out);
    auto* openDlg = dynamic_cast<IFileOpenDialog*>(this);
    if (openDlg) {
        openDlg->m_resultPaths.clear();
        openDlg->m_resultPaths.push_back(m_resultPath);
    }
    return S_OK;
}

BOOL GetOpenFileNameW(LPOPENFILENAMEW lpofn) {
    if (!lpofn || !lpofn->lpstrFile || lpofn->nMaxFile == 0) return FALSE;
    std::string out = RunDialogCommand("kdialog --getopenfilename . 2>/dev/null || zenity --file-selection 2>/dev/null");
    if (out.empty()) return FALSE;
    std::wstring wout = LinuxPosixPathToWin(out);
    wcscpy_s(lpofn->lpstrFile, lpofn->nMaxFile, wout.c_str());
    return TRUE;
}

BOOL GetSaveFileNameW(LPOPENFILENAMEW lpofn) {
    if (!lpofn || !lpofn->lpstrFile || lpofn->nMaxFile == 0) return FALSE;
    std::string out = RunDialogCommand("kdialog --getsavefilename . 2>/dev/null || zenity --file-selection --save --confirm-overwrite 2>/dev/null");
    if (out.empty()) return FALSE;
    std::wstring wout = LinuxPosixPathToWin(out);
    wcscpy_s(lpofn->lpstrFile, lpofn->nMaxFile, wout.c_str());
    return TRUE;
}

HIMC ImmGetContext(HWND /*hWnd*/) { return reinterpret_cast<HIMC>(0x7001); }
BOOL ImmSetCompositionWindow(HIMC /*hIMC*/, COMPOSITIONFORM* /*lpCompForm*/) { return TRUE; }
BOOL ImmReleaseContext(HWND /*hWnd*/, HIMC /*hIMC*/) { return TRUE; }

BOOL RegisterTouchWindow(HWND /*hwnd*/, ULONG /*ulFlags*/) { return TRUE; }
BOOL UnregisterTouchWindow(HWND /*hwnd*/) { return TRUE; }
BOOL GetTouchInputInfo(HTOUCHINPUT /*hTouchInput*/, UINT /*cInputs*/, PTOUCHINPUT /*pInputs*/, int /*cbSize*/) { return FALSE; }
BOOL CloseTouchInputHandle(HTOUCHINPUT /*hTouchInput*/) { return TRUE; }

#endif // !defined(_WIN32)
