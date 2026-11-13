#include "LinuxPlatform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

// ============================================================================
// Clean SDL2 / X11 / Wayland ABI Declarations (loaded via dlopen)
// ============================================================================

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Cursor;

struct SDL_Rect {
    int x;
    int y;
    int w;
    int h;
};

struct SDL_version {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
};

struct SDL_DisplayMode {
    uint32_t format;
    int w;
    int h;
    int refresh_rate;
    void* driverdata;
};

enum SDL_SYSWM_TYPE {
    SDL_SYSWM_UNKNOWN = 0,
    SDL_SYSWM_WINDOWS,
    SDL_SYSWM_X11,
    SDL_SYSWM_DIRECTFB,
    SDL_SYSWM_COCOA,
    SDL_SYSWM_UIKIT,
    SDL_SYSWM_WAYLAND
};

struct SDL_SysWMinfo {
    SDL_version version;
    int subsystem;
    union {
        struct {
            void* display;
            unsigned long window;
        } x11;
        struct {
            void* display;
            void* surface;
            void* shell_surface;
            void* egl_window;
            void* xdg_surface;
            void* xdg_toplevel;
            void* xdg_popup;
            void* xdg_positioner;
        } wl;
        uint8_t dummy[64];
    } info;
};

struct SDL_WindowEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t event;
    uint8_t padding1;
    uint8_t padding2;
    uint8_t padding3;
    int32_t data1;
    int32_t data2;
};

struct SDL_Keysym {
    int32_t scancode;
    int32_t sym;
    uint16_t mod;
    uint32_t unused;
};

struct SDL_KeyboardEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t state;
    uint8_t repeat;
    uint8_t padding2;
    uint8_t padding3;
    SDL_Keysym keysym;
};

struct SDL_TextInputEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    char text[32];
};

struct SDL_MouseMotionEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t state;
    int32_t x;
    int32_t y;
    int32_t xrel;
    int32_t yrel;
};

struct SDL_MouseButtonEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    uint8_t padding1;
    int32_t x;
    int32_t y;
};

struct SDL_MouseWheelEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    int32_t x;
    int32_t y;
    uint32_t direction;
    float preciseX;
    float preciseY;
    int32_t mouseX;
    int32_t mouseY;
};

struct SDL_DropEvent {
    uint32_t type;
    uint32_t timestamp;
    char* file;
    uint32_t windowID;
};

struct SDL_DisplayEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t display;
    uint8_t event;
    uint8_t padding1;
    uint8_t padding2;
    uint8_t padding3;
    int32_t data1;
};

union SDL_Event {
    uint32_t type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_DropEvent drop;
    SDL_DisplayEvent display;
    uint8_t padding[56];
};

static constexpr uint32_t kSDL_INIT_VIDEO = 0x00000020u;
static constexpr uint32_t kSDL_INIT_EVENTS = 0x00004000u;

static constexpr uint32_t kSDL_WINDOW_SHOWN = 0x00000004u;
static constexpr uint32_t kSDL_WINDOW_HIDDEN = 0x00000008u;
static constexpr uint32_t kSDL_WINDOW_BORDERLESS = 0x00000010u;
static constexpr uint32_t kSDL_WINDOW_RESIZABLE = 0x00000020u;
static constexpr uint32_t kSDL_WINDOW_MINIMIZED = 0x00000040u;
static constexpr uint32_t kSDL_WINDOW_MAXIMIZED = 0x00000080u;
static constexpr uint32_t kSDL_WINDOW_ALLOW_HIGHDPI = 0x00002000u;
static constexpr int kSDL_WINDOWPOS_CENTERED = 0x2FFF0000;

static constexpr uint32_t kSDL_RENDERER_ACCELERATED = 0x00000002u;
static constexpr uint32_t kSDL_RENDERER_PRESENTVSYNC = 0x00000004u;
static constexpr uint32_t kSDL_PIXELFORMAT_ARGB8888 = 0x16362004u;
static constexpr int kSDL_TEXTUREACCESS_STREAMING = 1;

static constexpr uint32_t kSDL_QUIT = 0x100;
static constexpr uint32_t kSDL_DISPLAYEVENT = 0x150;
static constexpr uint32_t kSDL_WINDOWEVENT = 0x200;
static constexpr uint32_t kSDL_KEYDOWN = 0x300;
static constexpr uint32_t kSDL_KEYUP = 0x301;
static constexpr uint32_t kSDL_TEXTINPUT = 0x303;
static constexpr uint32_t kSDL_MOUSEMOTION = 0x400;
static constexpr uint32_t kSDL_MOUSEBUTTONDOWN = 0x401;
static constexpr uint32_t kSDL_MOUSEBUTTONUP = 0x402;
static constexpr uint32_t kSDL_MOUSEWHEEL = 0x403;
static constexpr uint32_t kSDL_DROPFILE = 0x1000;
static constexpr uint32_t kSDL_DROPBEGIN = 0x1002;
static constexpr uint32_t kSDL_DROPCOMPLETE = 0x1003;

static constexpr uint8_t kSDL_WINDOWEVENT_SHOWN = 1;
static constexpr uint8_t kSDL_WINDOWEVENT_EXPOSED = 3;
static constexpr uint8_t kSDL_WINDOWEVENT_MOVED = 4;
static constexpr uint8_t kSDL_WINDOWEVENT_SIZE_CHANGED = 6;
static constexpr uint8_t kSDL_WINDOWEVENT_MINIMIZED = 7;
static constexpr uint8_t kSDL_WINDOWEVENT_MAXIMIZED = 8;
static constexpr uint8_t kSDL_WINDOWEVENT_RESTORED = 9;
static constexpr uint8_t kSDL_WINDOWEVENT_ENTER = 10;
static constexpr uint8_t kSDL_WINDOWEVENT_LEAVE = 11;
static constexpr uint8_t kSDL_WINDOWEVENT_FOCUS_GAINED = 12;
static constexpr uint8_t kSDL_WINDOWEVENT_FOCUS_LOST = 13;
static constexpr uint8_t kSDL_WINDOWEVENT_CLOSE = 14;
static constexpr uint8_t kSDL_WINDOWEVENT_DISPLAY_CHANGED = 18;

static constexpr uint32_t kSDL_MOUSEWHEEL_FLIPPED = 1;
static constexpr uint16_t kKMOD_LSHIFT = 0x0001;
static constexpr uint16_t kKMOD_RSHIFT = 0x0002;
static constexpr uint16_t kKMOD_LCTRL = 0x0040;
static constexpr uint16_t kKMOD_RCTRL = 0x0080;
static constexpr uint16_t kKMOD_LALT = 0x0100;
static constexpr uint16_t kKMOD_RALT = 0x0200;

struct Sdl2Api {
    void* lib = nullptr;
    bool initialized = false;
    bool available = false;

    int (*SDL_Init)(uint32_t) = nullptr;
    void (*SDL_Quit)() = nullptr;
    void (*SDL_GetVersion)(SDL_version*) = nullptr;
    const char* (*SDL_GetCurrentVideoDriver)() = nullptr;

    SDL_Window* (*SDL_CreateWindow)(const char*, int, int, int, int, uint32_t) = nullptr;
    void (*SDL_DestroyWindow)(SDL_Window*) = nullptr;
    uint32_t (*SDL_GetWindowID)(SDL_Window*) = nullptr;
    void (*SDL_SetWindowTitle)(SDL_Window*, const char*) = nullptr;
    void (*SDL_GetWindowSize)(SDL_Window*, int*, int*) = nullptr;
    void (*SDL_SetWindowSize)(SDL_Window*, int, int) = nullptr;
    void (*SDL_GetWindowPosition)(SDL_Window*, int*, int*) = nullptr;
    void (*SDL_SetWindowPosition)(SDL_Window*, int, int) = nullptr;
    void (*SDL_ShowWindow)(SDL_Window*) = nullptr;
    void (*SDL_HideWindow)(SDL_Window*) = nullptr;
    void (*SDL_MinimizeWindow)(SDL_Window*) = nullptr;
    void (*SDL_MaximizeWindow)(SDL_Window*) = nullptr;
    void (*SDL_RestoreWindow)(SDL_Window*) = nullptr;
    void (*SDL_RaiseWindow)(SDL_Window*) = nullptr;
    uint32_t (*SDL_GetWindowFlags)(SDL_Window*) = nullptr;
    int (*SDL_GetWindowDisplayIndex)(SDL_Window*) = nullptr;
    int (*SDL_GetWindowWMInfo)(SDL_Window*, SDL_SysWMinfo*) = nullptr;

    SDL_Renderer* (*SDL_CreateRenderer)(SDL_Window*, int, uint32_t) = nullptr;
    void (*SDL_DestroyRenderer)(SDL_Renderer*) = nullptr;
    int (*SDL_GetRendererOutputSize)(SDL_Renderer*, int*, int*) = nullptr;
    SDL_Texture* (*SDL_CreateTexture)(SDL_Renderer*, uint32_t, int, int, int) = nullptr;
    void (*SDL_DestroyTexture)(SDL_Texture*) = nullptr;
    int (*SDL_UpdateTexture)(SDL_Texture*, const SDL_Rect*, const void*, int) = nullptr;
    int (*SDL_RenderClear)(SDL_Renderer*) = nullptr;
    int (*SDL_RenderCopy)(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*) = nullptr;
    void (*SDL_RenderPresent)(SDL_Renderer*) = nullptr;

    int (*SDL_PollEvent)(SDL_Event*) = nullptr;
    uint32_t (*SDL_GetMouseState)(int*, int*) = nullptr;
    uint32_t (*SDL_GetGlobalMouseState)(int*, int*) = nullptr;
    uint16_t (*SDL_GetModState)() = nullptr;
    int (*SDL_ShowCursor)(int) = nullptr;
    SDL_Cursor* (*SDL_CreateSystemCursor)(int) = nullptr;
    void (*SDL_SetCursor)(SDL_Cursor*) = nullptr;
    void (*SDL_FreeCursor)(SDL_Cursor*) = nullptr;
    void (*SDL_CaptureMouse)(int) = nullptr;

    int (*SDL_GetNumVideoDisplays)() = nullptr;
    const char* (*SDL_GetDisplayName)(int) = nullptr;
    int (*SDL_GetDisplayBounds)(int, SDL_Rect*) = nullptr;
    int (*SDL_GetDisplayUsableBounds)(int, SDL_Rect*) = nullptr;
    int (*SDL_GetCurrentDisplayMode)(int, SDL_DisplayMode*) = nullptr;
    int (*SDL_GetDesktopDisplayMode)(int, SDL_DisplayMode*) = nullptr;
    int (*SDL_GetDisplayDPI)(int, float*, float*, float*) = nullptr;

    char* (*SDL_GetClipboardText)() = nullptr;
    int (*SDL_SetClipboardText)(const char*) = nullptr;
    int (*SDL_HasClipboardText)() = nullptr;
    void (*SDL_free)(void*) = nullptr;
    int (*SDL_SetHint)(const char*, const char*) = nullptr;

    static Sdl2Api& Get() {
        static Sdl2Api api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
            if (!api.lib) api.lib = dlopen("libSDL2.so", RTLD_LAZY | RTLD_LOCAL);
            if (!api.lib) return;

            auto load = [&](auto& fn, const char* sym) {
                fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(api.lib, sym));
            };

            load(api.SDL_Init, "SDL_Init");
            load(api.SDL_Quit, "SDL_Quit");
            load(api.SDL_GetVersion, "SDL_GetVersion");
            load(api.SDL_GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");

            load(api.SDL_CreateWindow, "SDL_CreateWindow");
            load(api.SDL_DestroyWindow, "SDL_DestroyWindow");
            load(api.SDL_GetWindowID, "SDL_GetWindowID");
            load(api.SDL_SetWindowTitle, "SDL_SetWindowTitle");
            load(api.SDL_GetWindowSize, "SDL_GetWindowSize");
            load(api.SDL_SetWindowSize, "SDL_SetWindowSize");
            load(api.SDL_GetWindowPosition, "SDL_GetWindowPosition");
            load(api.SDL_SetWindowPosition, "SDL_SetWindowPosition");
            load(api.SDL_ShowWindow, "SDL_ShowWindow");
            load(api.SDL_HideWindow, "SDL_HideWindow");
            load(api.SDL_MinimizeWindow, "SDL_MinimizeWindow");
            load(api.SDL_MaximizeWindow, "SDL_MaximizeWindow");
            load(api.SDL_RestoreWindow, "SDL_RestoreWindow");
            load(api.SDL_RaiseWindow, "SDL_RaiseWindow");
            load(api.SDL_GetWindowFlags, "SDL_GetWindowFlags");
            load(api.SDL_GetWindowDisplayIndex, "SDL_GetWindowDisplayIndex");
            load(api.SDL_GetWindowWMInfo, "SDL_GetWindowWMInfo");

            load(api.SDL_CreateRenderer, "SDL_CreateRenderer");
            load(api.SDL_DestroyRenderer, "SDL_DestroyRenderer");
            load(api.SDL_GetRendererOutputSize, "SDL_GetRendererOutputSize");
            load(api.SDL_CreateTexture, "SDL_CreateTexture");
            load(api.SDL_DestroyTexture, "SDL_DestroyTexture");
            load(api.SDL_UpdateTexture, "SDL_UpdateTexture");
            load(api.SDL_RenderClear, "SDL_RenderClear");
            load(api.SDL_RenderCopy, "SDL_RenderCopy");
            load(api.SDL_RenderPresent, "SDL_RenderPresent");

            load(api.SDL_PollEvent, "SDL_PollEvent");
            load(api.SDL_GetMouseState, "SDL_GetMouseState");
            load(api.SDL_GetGlobalMouseState, "SDL_GetGlobalMouseState");
            load(api.SDL_GetModState, "SDL_GetModState");
            load(api.SDL_ShowCursor, "SDL_ShowCursor");
            load(api.SDL_CreateSystemCursor, "SDL_CreateSystemCursor");
            load(api.SDL_SetCursor, "SDL_SetCursor");
            load(api.SDL_FreeCursor, "SDL_FreeCursor");
            load(api.SDL_CaptureMouse, "SDL_CaptureMouse");

            load(api.SDL_GetNumVideoDisplays, "SDL_GetNumVideoDisplays");
            load(api.SDL_GetDisplayName, "SDL_GetDisplayName");
            load(api.SDL_GetDisplayBounds, "SDL_GetDisplayBounds");
            load(api.SDL_GetDisplayUsableBounds, "SDL_GetDisplayUsableBounds");
            load(api.SDL_GetCurrentDisplayMode, "SDL_GetCurrentDisplayMode");
            load(api.SDL_GetDesktopDisplayMode, "SDL_GetDesktopDisplayMode");
            load(api.SDL_GetDisplayDPI, "SDL_GetDisplayDPI");

            load(api.SDL_GetClipboardText, "SDL_GetClipboardText");
            load(api.SDL_SetClipboardText, "SDL_SetClipboardText");
            load(api.SDL_HasClipboardText, "SDL_HasClipboardText");
            load(api.SDL_free, "SDL_free");
            load(api.SDL_SetHint, "SDL_SetHint");

            if (api.SDL_Init && api.SDL_CreateWindow && api.SDL_PollEvent) {
                if (api.SDL_SetHint) {
                    api.SDL_SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
                    api.SDL_SetHint("SDL_VIDEODRIVER", "wayland,x11");
                }
                if (api.SDL_Init(kSDL_INIT_VIDEO | kSDL_INIT_EVENTS) == 0) {
                    api.initialized = true;
                    api.available = true;
                }
            }
        });
        return api;
    }
};

struct X11Api {
    void* lib = nullptr;
    bool loaded = false;

    unsigned long (*XCreateSimpleWindow)(void*, unsigned long, int, int, unsigned int, unsigned int,
                                          unsigned int, unsigned long, unsigned long) = nullptr;
    int (*XDestroyWindow)(void*, unsigned long) = nullptr;
    int (*XMapWindow)(void*, unsigned long) = nullptr;
    int (*XUnmapWindow)(void*, unsigned long) = nullptr;
    int (*XMoveResizeWindow)(void*, unsigned long, int, int, unsigned int, unsigned int) = nullptr;
    int (*XFlush)(void*) = nullptr;
    int (*XConnectionNumber)(void*) = nullptr;
    char* (*XResourceManagerString)(void*) = nullptr;

    static X11Api& Get() {
        static X11Api api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
            if (!api.lib) return;
            auto load = [&](auto& fn, const char* sym) {
                fn = reinterpret_cast<std::decay_t<decltype(fn)>>(dlsym(api.lib, sym));
            };
            load(api.XCreateSimpleWindow, "XCreateSimpleWindow");
            load(api.XDestroyWindow, "XDestroyWindow");
            load(api.XMapWindow, "XMapWindow");
            load(api.XUnmapWindow, "XUnmapWindow");
            load(api.XMoveResizeWindow, "XMoveResizeWindow");
            load(api.XFlush, "XFlush");
            load(api.XConnectionNumber, "XConnectionNumber");
            load(api.XResourceManagerString, "XResourceManagerString");
            api.loaded = (api.XCreateSimpleWindow && api.XDestroyWindow &&
                          api.XMapWindow && api.XUnmapWindow &&
                          api.XMoveResizeWindow && api.XFlush);
        });
        return api;
    }
};

struct WaylandClientApi {
    void* lib = nullptr;
    int (*wl_display_get_fd)(void*) = nullptr;

    static WaylandClientApi& Get() {
        static WaylandClientApi api;
        static std::once_flag once;
        std::call_once(once, [&]() {
            api.lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
            if (!api.lib) return;
            api.wl_display_get_fd = reinterpret_cast<int (*)(void*)>(dlsym(api.lib, "wl_display_get_fd"));
        });
        return api;
    }
};

// ============================================================================
// Window Registry & State
// ============================================================================

struct LinuxWindow {
    HWND hwnd = nullptr;
    std::wstring className;
    std::wstring title;
    DWORD style = 0;
    DWORD exStyle = 0;
    WNDPROC wndProc = nullptr;
    LONG_PTR userData = 0;
    HWND parent = nullptr;
    std::vector<HWND> children;

    RECT rect{100, 100, 1380, 820}; // Physical pixel screen coords
    bool visible = false;
    bool minimized = false;
    bool maximized = false;
    bool acceptsDrop = false;
    bool isLayered = false;
    bool isVideoChild = false;
    bool trackingMouseLeave = false;
    bool mouseInside = false;
    UINT dpi = 96;

    SDL_Window* sdlWindow = nullptr;
    uint32_t sdlWindowId = 0;
    SDL_Renderer* sdlRenderer = nullptr;
    SDL_Texture* sdlTexture = nullptr;
    int texW = 0;
    int texH = 0;

    // X11 native child Window XID for WS_CHILD video embedding
    void* x11Display = nullptr;
    unsigned long x11ParentWindow = 0;
    unsigned long x11ChildWindow = 0;

    // Latest swapchain surface presented on top-level window
    Microsoft::WRL::ComPtr<IDXGISurface> boundSurface;

    // Offscreen overlay (WS_EX_LAYERED) pixel storage
    std::vector<uint8_t> overlayPixels;
    int overlayW = 0;
    int overlayH = 0;
    RECT overlayScreenRect{0, 0, 0, 0};

    // Wayland/software video pixel storage (WS_CHILD video window)
    std::vector<uint8_t> videoPixels;
    int videoW = 0;
    int videoH = 0;
    bool videoFrameDirty = false;
    std::vector<uint8_t> compositeScratch;
};

struct LinuxClassInfo {
    std::wstring name;
    WNDPROC wndProc = nullptr;
};

struct LinuxTimer {
    HWND hwnd = nullptr;
    UINT_PTR id = 0;
    UINT intervalMs = 16;
    TIMERPROC proc = nullptr;
    std::chrono::steady_clock::time_point nextFire;
};

struct LinuxGdiObject {
    enum Kind { Bitmap, DC, Brush } kind = Bitmap;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> dibBits;
    HBITMAP selectedBitmap = nullptr;
    HWND dcHwnd = nullptr;
    COLORREF brushColor = 0;
};

struct LinuxDropData {
    std::vector<std::wstring> files;
};

std::mutex g_winMtx;
std::unordered_map<std::wstring, LinuxClassInfo> g_classes;
std::unordered_map<HWND, LinuxWindow*> g_windows;
std::unordered_map<uint32_t, HWND> g_sdlIdToHwnd;
std::unordered_map<UINT, std::wstring> g_registeredMsgs;
UINT g_nextRegMsg = WM_USER + 0x1000;
uintptr_t g_nextHwndVal = 0x10000;

HWND g_mainHwnd = nullptr;
HWND g_foregroundHwnd = nullptr;
HWND g_focusHwnd = nullptr;
HWND g_captureHwnd = nullptr;
bool g_quitPosted = false;
int g_quitCode = 0;
int g_cursorVisibleCount = 0;
bool g_displayWakeRegistered = false;

std::vector<MSG> g_msgQueue;
std::vector<LinuxTimer> g_timers;
std::array<bool, 256> g_keyDown{};
std::vector<std::string> g_pendingDrops;
uint32_t g_pendingDropWindowId = 0;

std::string g_clipboardText;
HGLOBAL g_lastClipboardHandle = nullptr;
std::unordered_map<int, SDL_Cursor*> g_systemCursors;

// ============================================================================
// Coordinate, Monitor & DPI Helpers
// ============================================================================

bool IsValidLinuxWindow(HWND hWnd) {
    if (!hWnd) return false;
    std::lock_guard<std::mutex> lock(g_winMtx);
    return g_windows.find(hWnd) != g_windows.end();
}

LinuxWindow* GetWindowLocked(HWND hWnd) {
    auto it = g_windows.find(hWnd);
    return it != g_windows.end() ? it->second : nullptr;
}

LinuxWindow* GetRootWindowLocked(LinuxWindow* w) {
    while (w && w->parent) {
        auto it = g_windows.find(w->parent);
        if (it == g_windows.end()) break;
        w = it->second;
    }
    return w;
}

bool QueryWmInfo(SDL_Window* sdlWin, SDL_SysWMinfo& info) {
    auto& sdl = Sdl2Api::Get();
    if (!sdl.available || !sdl.SDL_GetVersion || !sdl.SDL_GetWindowWMInfo || !sdlWin) return false;
    std::memset(&info, 0, sizeof(info));
    sdl.SDL_GetVersion(&info.version);
    return sdl.SDL_GetWindowWMInfo(sdlWin, &info) != 0;
}

void RegisterDisplayWakeFdOnce(SDL_Window* sdlWin) {
    if (g_displayWakeRegistered || !sdlWin) return;
    SDL_SysWMinfo info{};
    if (!QueryWmInfo(sdlWin, info)) return;
    if (info.subsystem == SDL_SYSWM_X11 && info.info.x11.display) {
        auto& x11 = X11Api::Get();
        if (x11.XConnectionNumber) {
            int fd = x11.XConnectionNumber(info.info.x11.display);
            if (fd >= 0) {
                LinuxSetDisplayWakeFd(fd);
                g_displayWakeRegistered = true;
            }
        }
    } else if (info.subsystem == SDL_SYSWM_WAYLAND && info.info.wl.display) {
        auto& wl = WaylandClientApi::Get();
        if (wl.wl_display_get_fd) {
            int fd = wl.wl_display_get_fd(info.info.wl.display);
            if (fd >= 0) {
                LinuxSetDisplayWakeFd(fd);
                g_displayWakeRegistered = true;
            }
        }
    }
}

UINT ComputeWindowDpi(LinuxWindow* w) {
    auto& sdl = Sdl2Api::Get();
    if (w && w->sdlWindow && sdl.available) {
        int winW = 0, winH = 0, outW = 0, outH = 0;
        if (sdl.SDL_GetWindowSize) sdl.SDL_GetWindowSize(w->sdlWindow, &winW, &winH);
        if (w->sdlRenderer && sdl.SDL_GetRendererOutputSize) {
            sdl.SDL_GetRendererOutputSize(w->sdlRenderer, &outW, &outH);
        }
        if (winW > 0 && outW > winW) {
            double ratio = static_cast<double>(outW) / static_cast<double>(winW);
            UINT d = static_cast<UINT>(std::round(ratio * 96.0));
            return std::max<UINT>(96, d);
        }
    }

    // Fallback for X11 fractional scaling (Xft.dpi / GDK_SCALE / QT_SCALE_FACTOR)
    if (const char* gdk = std::getenv("GDK_SCALE")) {
        double s = std::atof(gdk);
        if (s >= 1.0 && s <= 4.0) return static_cast<UINT>(std::round(s * 96.0));
    }
    if (const char* qt = std::getenv("QT_SCALE_FACTOR")) {
        double s = std::atof(qt);
        if (s >= 1.0 && s <= 4.0) return static_cast<UINT>(std::round(s * 96.0));
    }

    SDL_SysWMinfo info{};
    if (w && w->sdlWindow && QueryWmInfo(w->sdlWindow, info) &&
        info.subsystem == SDL_SYSWM_X11 && info.info.x11.display) {
        auto& x11 = X11Api::Get();
        if (x11.XResourceManagerString) {
            if (const char* rms = x11.XResourceManagerString(info.info.x11.display)) {
                if (const char* p = std::strstr(rms, "Xft.dpi:")) {
                    p += 8;
                    while (*p == ' ' || *p == '\t') ++p;
                    double dpiVal = std::atof(p);
                    if (dpiVal >= 72.0 && dpiVal <= 480.0) {
                        return static_cast<UINT>(std::round(dpiVal));
                    }
                }
            }
        }
    }
    return 96;
}

double GetSdlPointToPixelScale(const LinuxWindow* w) {
    auto& sdl = Sdl2Api::Get();
    if (!w || !w->sdlWindow || !w->sdlRenderer || !sdl.SDL_GetWindowSize || !sdl.SDL_GetRendererOutputSize) {
        return 1.0;
    }
    int winW = 0, winH = 0, outW = 0, outH = 0;
    sdl.SDL_GetWindowSize(w->sdlWindow, &winW, &winH);
    sdl.SDL_GetRendererOutputSize(w->sdlRenderer, &outW, &outH);
    if (winW > 0 && outW > 0) {
        return static_cast<double>(outW) / static_cast<double>(winW);
    }
    return 1.0;
}

void SyncWindowPhysicalRectFromSdl(LinuxWindow* w) {
    auto& sdl = Sdl2Api::Get();
    if (!w || !w->sdlWindow || !sdl.available) return;
    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (sdl.SDL_GetWindowPosition) sdl.SDL_GetWindowPosition(w->sdlWindow, &wx, &wy);
    if (sdl.SDL_GetWindowSize) sdl.SDL_GetWindowSize(w->sdlWindow, &ww, &wh);
    int outW = ww, outH = wh;
    if (w->sdlRenderer && sdl.SDL_GetRendererOutputSize) {
        sdl.SDL_GetRendererOutputSize(w->sdlRenderer, &outW, &outH);
    }
    double scale = (ww > 0 && outW > 0) ? (static_cast<double>(outW) / ww) : 1.0;
    w->rect.left = static_cast<LONG>(std::round(wx * scale));
    w->rect.top = static_cast<LONG>(std::round(wy * scale));
    w->rect.right = w->rect.left + std::max(1, outW);
    w->rect.bottom = w->rect.top + std::max(1, outH);
}

int GetNumDisplays() {
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_GetNumVideoDisplays) {
        int n = sdl.SDL_GetNumVideoDisplays();
        if (n > 0) return n;
    }
    return 1;
}

RECT GetDisplayBoundsPx(int idx) {
    auto& sdl = Sdl2Api::Get();
    RECT rc{0, 0, 1920, 1080};
    if (sdl.available && sdl.SDL_GetDisplayBounds) {
        SDL_Rect sr{};
        if (sdl.SDL_GetDisplayBounds(idx, &sr) == 0 && sr.w > 0 && sr.h > 0) {
            // Note: on HiDPI displays where SDL reports logical coords, check desktop mode pixel size.
            SDL_DisplayMode mode{};
            double sx = 1.0, sy = 1.0;
            if (sdl.SDL_GetDesktopDisplayMode && sdl.SDL_GetDesktopDisplayMode(idx, &mode) == 0 &&
                mode.w > sr.w && mode.h > sr.h) {
                sx = static_cast<double>(mode.w) / sr.w;
                sy = static_cast<double>(mode.h) / sr.h;
            }
            rc.left = static_cast<LONG>(std::round(sr.x * sx));
            rc.top = static_cast<LONG>(std::round(sr.y * sy));
            rc.right = rc.left + static_cast<LONG>(std::round(sr.w * sx));
            rc.bottom = rc.top + static_cast<LONG>(std::round(sr.h * sy));
        }
    }
    return rc;
}

RECT GetDisplayWorkPx(int idx) {
    auto& sdl = Sdl2Api::Get();
    RECT mon = GetDisplayBoundsPx(idx);
    if (sdl.available && sdl.SDL_GetDisplayUsableBounds) {
        SDL_Rect sr{}, full{};
        if (sdl.SDL_GetDisplayUsableBounds(idx, &sr) == 0 && sr.w > 0 && sr.h > 0) {
            double sx = 1.0, sy = 1.0;
            if (sdl.SDL_GetDisplayBounds && sdl.SDL_GetDisplayBounds(idx, &full) == 0 && full.w > 0 && full.h > 0) {
                sx = static_cast<double>(mon.right - mon.left) / full.w;
                sy = static_cast<double>(mon.bottom - mon.top) / full.h;
            }
            RECT work;
            work.left = static_cast<LONG>(std::round(sr.x * sx));
            work.top = static_cast<LONG>(std::round(sr.y * sy));
            work.right = work.left + static_cast<LONG>(std::round(sr.w * sx));
            work.bottom = work.top + static_cast<LONG>(std::round(sr.h * sy));
            return work;
        }
    }
    return mon;
}

HMONITOR MakeHMonitor(int idx) {
    return reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x3000 + std::max(0, idx)));
}

int HMonitorToIndex(HMONITOR hMon) {
    uintptr_t v = reinterpret_cast<uintptr_t>(hMon);
    int n = GetNumDisplays();
    if (v >= 0x3000 && v < static_cast<uintptr_t>(0x3000 + n)) {
        return static_cast<int>(v - 0x3000);
    }
    return 0;
}

// ============================================================================
// Compositing (Main UI + Wayland Video + Alpha-Blended Layered Overlay)
// ============================================================================

void BlendPremulOver(uint8_t* dstBGRA, const uint8_t* srcBGRA, uint8_t constAlpha) {
    uint8_t sb = srcBGRA[0];
    uint8_t sg = srcBGRA[1];
    uint8_t sr = srcBGRA[2];
    uint8_t sa = srcBGRA[3];
    if (sa == 0 || constAlpha == 0) return;

    if (constAlpha < 255) {
        sb = static_cast<uint8_t>((static_cast<unsigned>(sb) * constAlpha + 127) / 255);
        sg = static_cast<uint8_t>((static_cast<unsigned>(sg) * constAlpha + 127) / 255);
        sr = static_cast<uint8_t>((static_cast<unsigned>(sr) * constAlpha + 127) / 255);
        sa = static_cast<uint8_t>((static_cast<unsigned>(sa) * constAlpha + 127) / 255);
    }
    if (sa == 255) {
        dstBGRA[0] = sb;
        dstBGRA[1] = sg;
        dstBGRA[2] = sr;
        dstBGRA[3] = 255;
        return;
    }
    unsigned invA = 255u - sa;
    dstBGRA[0] = static_cast<uint8_t>(std::min(255u, sb + (dstBGRA[0] * invA + 127u) / 255u));
    dstBGRA[1] = static_cast<uint8_t>(std::min(255u, sg + (dstBGRA[1] * invA + 127u) / 255u));
    dstBGRA[2] = static_cast<uint8_t>(std::min(255u, sr + (dstBGRA[2] * invA + 127u) / 255u));
    dstBGRA[3] = static_cast<uint8_t>(std::min(255u, sa + (dstBGRA[3] * invA + 127u) / 255u));
}

void BlitScaledNearestOrBilinear(uint8_t* dst, int dstW, int dstH, const RECT& dstSubRect,
                                 const uint8_t* src, int srcW, int srcH) {
    int rx0 = std::max(0, static_cast<int>(dstSubRect.left));
    int ry0 = std::max(0, static_cast<int>(dstSubRect.top));
    int rx1 = std::min(dstW, static_cast<int>(dstSubRect.right));
    int ry1 = std::min(dstH, static_cast<int>(dstSubRect.bottom));
    int rw = dstSubRect.right - dstSubRect.left;
    int rh = dstSubRect.bottom - dstSubRect.top;
    if (rx0 >= rx1 || ry0 >= ry1 || rw <= 0 || rh <= 0 || srcW <= 0 || srcH <= 0) return;

    if (rw == srcW && rh == srcH) {
        size_t copyBytes = static_cast<size_t>(rx1 - rx0) * 4;
        for (int y = ry0; y < ry1; ++y) {
            int sy = y - static_cast<int>(dstSubRect.top);
            int sx = rx0 - static_cast<int>(dstSubRect.left);
            std::memcpy(dst + (static_cast<size_t>(y) * dstW + rx0) * 4,
                        src + (static_cast<size_t>(sy) * srcW + sx) * 4,
                        copyBytes);
        }
        return;
    }

    for (int y = ry0; y < ry1; ++y) {
        int sy = std::min(srcH - 1, std::max(0, (y - static_cast<int>(dstSubRect.top)) * srcH / rh));
        const uint8_t* srow = src + static_cast<size_t>(sy) * srcW * 4;
        uint8_t* drow = dst + static_cast<size_t>(y) * dstW * 4;
        for (int x = rx0; x < rx1; ++x) {
            int sx = std::min(srcW - 1, std::max(0, (x - static_cast<int>(dstSubRect.left)) * srcW / rw));
            std::memcpy(drow + x * 4, srow + sx * 4, 4);
        }
    }
}

void CompositeAndPresentWindow(LinuxWindow* root) {
    auto& sdl = Sdl2Api::Get();
    if (!root || !root->sdlRenderer || !sdl.available) return;

    int targetW = std::max(1, static_cast<int>(root->rect.right - root->rect.left));
    int targetH = std::max(1, static_cast<int>(root->rect.bottom - root->rect.top));

    LinuxWindow* activeVideo = nullptr;
    std::vector<LinuxWindow*> activeOverlays;
    {
        // Note: called under g_winMtx or without? Snapshot pointers safely under g_winMtx is done by caller,
        // or keep simple: callers hold g_winMtx or call inside window operations where g_winMtx is locked.
        for (auto& kv : g_windows) {
            LinuxWindow* cw = kv.second;
            if (!cw || !cw->visible) continue;
            if (cw->isVideoChild && cw->parent == root->hwnd && !cw->videoPixels.empty() && cw->videoW > 0 && cw->videoH > 0) {
                activeVideo = cw;
            } else if (cw->isLayered && !cw->overlayPixels.empty() && cw->overlayW > 0 && cw->overlayH > 0) {
                activeOverlays.push_back(cw);
            }
        }
    }

    const uint8_t* presentBytes = nullptr;
    int presentW = targetW;
    int presentH = targetH;

    bool hasMainSurface = (root->boundSurface && root->boundSurface->m_width > 0 &&
                           root->boundSurface->m_height > 0 &&
                           root->boundSurface->m_pixels.size() >=
                               static_cast<size_t>(root->boundSurface->m_width) * root->boundSurface->m_height * 4);

    if (!activeVideo && activeOverlays.empty()) {
        if (!hasMainSurface) return;
        presentW = static_cast<int>(root->boundSurface->m_width);
        presentH = static_cast<int>(root->boundSurface->m_height);
        presentBytes = root->boundSurface->m_pixels.data();
    } else {
        if (hasMainSurface) {
            presentW = static_cast<int>(root->boundSurface->m_width);
            presentH = static_cast<int>(root->boundSurface->m_height);
        }
        size_t need = static_cast<size_t>(presentW) * presentH * 4;
        root->compositeScratch.resize(need);
        if (hasMainSurface) {
            std::memcpy(root->compositeScratch.data(), root->boundSurface->m_pixels.data(), need);
        } else {
            std::memset(root->compositeScratch.data(), 0, need);
        }

        if (activeVideo) {
            RECT vrc{
                activeVideo->rect.left - root->rect.left,
                activeVideo->rect.top - root->rect.top,
                activeVideo->rect.right - root->rect.left,
                activeVideo->rect.bottom - root->rect.top
            };
            BlitScaledNearestOrBilinear(root->compositeScratch.data(), presentW, presentH, vrc,
                                        activeVideo->videoPixels.data(), activeVideo->videoW, activeVideo->videoH);
        }

        for (LinuxWindow* ov : activeOverlays) {
            int ox = static_cast<int>(ov->overlayScreenRect.left - root->rect.left);
            int oy = static_cast<int>(ov->overlayScreenRect.top - root->rect.top);
            int x0 = std::max(0, ox);
            int y0 = std::max(0, oy);
            int x1 = std::min(presentW, ox + ov->overlayW);
            int y1 = std::min(presentH, oy + ov->overlayH);
            if (x0 >= x1 || y0 >= y1) continue;
            for (int y = y0; y < y1; ++y) {
                int sy = y - oy;
                const uint8_t* srow = ov->overlayPixels.data() + static_cast<size_t>(sy) * ov->overlayW * 4;
                uint8_t* drow = root->compositeScratch.data() + static_cast<size_t>(y) * presentW * 4;
                for (int x = x0; x < x1; ++x) {
                    int sx = x - ox;
                    BlendPremulOver(drow + x * 4, srow + sx * 4, 255);
                }
            }
        }
        presentBytes = root->compositeScratch.data();
    }

    if (!root->sdlTexture || root->texW != presentW || root->texH != presentH) {
        if (root->sdlTexture && sdl.SDL_DestroyTexture) sdl.SDL_DestroyTexture(root->sdlTexture);
        root->sdlTexture = sdl.SDL_CreateTexture(
            root->sdlRenderer, kSDL_PIXELFORMAT_ARGB8888, kSDL_TEXTUREACCESS_STREAMING, presentW, presentH);
        root->texW = presentW;
        root->texH = presentH;
    }
    if (root->sdlTexture && presentBytes) {
        sdl.SDL_UpdateTexture(root->sdlTexture, nullptr, presentBytes, presentW * 4);
        sdl.SDL_RenderClear(root->sdlRenderer);
        sdl.SDL_RenderCopy(root->sdlRenderer, root->sdlTexture, nullptr, nullptr);
        sdl.SDL_RenderPresent(root->sdlRenderer);
    }
}

void SyncX11ChildVideoGeometry(LinuxWindow* w) {
    if (!w || !w->x11Display || !w->x11ChildWindow) return;
    auto& x11 = X11Api::Get();
    if (!x11.loaded) return;

    int relX = 0, relY = 0;
    int cw = std::max(1, static_cast<int>(w->rect.right - w->rect.left));
    int ch = std::max(1, static_cast<int>(w->rect.bottom - w->rect.top));
    if (LinuxWindow* p = GetWindowLocked(w->parent)) {
        relX = static_cast<int>(w->rect.left - p->rect.left);
        relY = static_cast<int>(w->rect.top - p->rect.top);
    }
    x11.XMoveResizeWindow(w->x11Display, w->x11ChildWindow, relX, relY,
                          static_cast<unsigned int>(cw), static_cast<unsigned int>(ch));
    if (w->visible) x11.XMapWindow(w->x11Display, w->x11ChildWindow);
    else x11.XUnmapWindow(w->x11Display, w->x11ChildWindow);
    x11.XFlush(w->x11Display);
}

// ============================================================================
// Keyboard / Modifier / Cursor Mapping Helpers
// ============================================================================

int MapSdlKeycodeToVk(int32_t sym, int32_t scancode) {
    if (sym >= 'a' && sym <= 'z') return 'A' + (sym - 'a');
    if (sym >= 'A' && sym <= 'Z') return sym;
    if (sym >= '0' && sym <= '9') return sym;

    switch (sym) {
        case 8: return VK_BACK;
        case 9: return VK_TAB;
        case 13: return VK_RETURN;
        case 27: return VK_ESCAPE;
        case 32: return VK_SPACE;
        case 127: return VK_DELETE;
        case ';': case ':': return VK_OEM_1;
        case '=': case '+': return VK_OEM_PLUS;
        case ',': case '<': return VK_OEM_COMMA;
        case '-': case '_': return VK_OEM_MINUS;
        case '.': case '>': return VK_OEM_PERIOD;
        case '/': case '?': return VK_OEM_2;
        case '`': case '~': return VK_OEM_3;
        case '[': case '{': return VK_OEM_4;
        case '\\': case '|': return VK_OEM_5;
        case ']': case '}': return VK_OEM_6;
        case '\'': case '"': return VK_OEM_7;
        default: break;
    }

    // SDLK_SCANCODE_MASK = 1 << 30
    constexpr int32_t kMask = (1 << 30);
    if (sym & kMask) {
        int32_t sc = sym & ~kMask;
        if (sc >= 58 && sc <= 69) return VK_F1 + (sc - 58);      // F1..F12
        if (sc >= 104 && sc <= 115) return 0x7C + (sc - 104);    // F13..F24
        switch (sc) {
            case 70: return VK_SNAPSHOT;
            case 71: return VK_SCROLL;
            case 72: return VK_PAUSE;
            case 73: return VK_INSERT;
            case 74: return VK_HOME;
            case 75: return VK_PRIOR;
            case 76: return VK_DELETE;
            case 77: return VK_END;
            case 78: return VK_NEXT;
            case 79: return VK_RIGHT;
            case 80: return VK_LEFT;
            case 81: return VK_DOWN;
            case 82: return VK_UP;
            case 83: return VK_NUMLOCK;
            case 84: return VK_DIVIDE;
            case 85: return VK_MULTIPLY;
            case 86: return VK_SUBTRACT;
            case 87: return VK_ADD;
            case 88: return VK_RETURN;
            case 89: return VK_NUMPAD1;
            case 90: return VK_NUMPAD2;
            case 91: return VK_NUMPAD3;
            case 92: return VK_NUMPAD4;
            case 93: return VK_NUMPAD5;
            case 94: return VK_NUMPAD6;
            case 95: return VK_NUMPAD7;
            case 96: return VK_NUMPAD8;
            case 97: return VK_NUMPAD9;
            case 98: return VK_NUMPAD0;
            case 99: return VK_DECIMAL;
            case 101: return VK_APPS;
            case 224: case 228: return VK_CONTROL;
            case 225: case 229: return VK_SHIFT;
            case 226: case 230: return VK_MENU;
            case 227: return VK_LWIN;
            case 231: return VK_RWIN;
            default: break;
        }
    }

    if (scancode >= 58 && scancode <= 69) return VK_F1 + (scancode - 58);
    return 0;
}

WPARAM BuildMouseModifierWParam(uint32_t sdlButtons) {
    auto& sdl = Sdl2Api::Get();
    WPARAM w = 0;
    if (sdlButtons & 1u) w |= MK_LBUTTON;
    if (sdlButtons & 2u) w |= MK_MBUTTON;
    if (sdlButtons & 4u) w |= MK_RBUTTON;
    if (sdlButtons & 8u) w |= MK_XBUTTON1;
    if (sdlButtons & 16u) w |= MK_XBUTTON2;
    if (sdl.SDL_GetModState) {
        uint16_t mod = sdl.SDL_GetModState();
        if (mod & (kKMOD_LSHIFT | kKMOD_RSHIFT)) w |= MK_SHIFT;
        if (mod & (kKMOD_LCTRL | kKMOD_RCTRL)) w |= MK_CONTROL;
    }
    return w;
}

void UpdateModifierKeyState(uint16_t mod) {
    g_keyDown[VK_LSHIFT] = (mod & kKMOD_LSHIFT) != 0;
    g_keyDown[VK_RSHIFT] = (mod & kKMOD_RSHIFT) != 0;
    g_keyDown[VK_SHIFT] = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT];
    g_keyDown[VK_LCONTROL] = (mod & kKMOD_LCTRL) != 0;
    g_keyDown[VK_RCONTROL] = (mod & kKMOD_RCTRL) != 0;
    g_keyDown[VK_CONTROL] = g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL];
    g_keyDown[VK_LMENU] = (mod & kKMOD_LALT) != 0;
    g_keyDown[VK_RMENU] = (mod & kKMOD_RALT) != 0;
    g_keyDown[VK_MENU] = g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU];
}

void QueueWindowMsgLocked(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, POINT pt = {0, 0}) {
    MSG m{};
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wParam;
    m.lParam = lParam;
    m.pt = pt;
    g_msgQueue.push_back(m);
}

// ============================================================================
// Safe fork + execvp Process Helpers (Zero shell / Zero zombies)
// ============================================================================

std::string FindExeInPath(const char* exeName) {
    if (!exeName || !*exeName) return "";
    if (std::strchr(exeName, '/')) {
        return access(exeName, X_OK) == 0 ? std::string(exeName) : "";
    }
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) pathEnv = "/usr/local/bin:/usr/bin:/bin";
    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + exeName;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

bool RunProcessCaptureStdout(const std::vector<std::string>& args, int& exitCode, std::string& outStdout) {
    exitCode = -1;
    outStdout.clear();
    if (args.empty()) return false;

    std::string exe = FindExeInPath(args[0].c_str());
    if (exe.empty()) return false;

    int pipefd[2]{-1, -1};
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(exe.c_str(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            outStdout.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
        return true;
    }
    return false;
}

bool LaunchDetachedProcess(const std::vector<std::string>& args, const std::string& workDir) {
    if (args.empty()) return false;
    std::string exe = FindExeInPath(args[0].c_str());
    if (exe.empty()) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);

        if (!workDir.empty()) {
            (void)chdir(workDir.c_str());
        }
        int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > 2) close(devNull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(exe.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return true;
}

struct FileDialogSpec {
    bool isSave = false;
    bool pickFolders = false;
    bool multiSelect = false;
    std::string title;
    std::string initialDir;
    std::string defaultExt;
    std::vector<std::pair<std::string, std::string>> filters; // (label, space-separated globs)
};

bool PreferKdialog() {
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop && (std::strstr(desktop, "KDE") || std::strstr(desktop, "kde"))) {
        return !FindExeInPath("kdialog").empty();
    }
    if (!FindExeInPath("zenity").empty()) return false;
    return !FindExeInPath("kdialog").empty();
}

std::string NormalizeFilterGlobs(const std::string& rawSpec) {
    // Converts "*.png;*.jpg" -> "*.png *.jpg"
    std::string out;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out += cur;
        cur.clear();
    };
    for (char c : rawSpec) {
        if (c == ';' || c == ',') flush();
        else if (!std::isspace(static_cast<unsigned char>(c))) cur.push_back(c);
    }
    flush();
    return out;
}

std::vector<std::wstring> RunNativeFileDialog(const FileDialogSpec& spec) {
    std::vector<std::wstring> results;
    std::string startPath = spec.initialDir.empty() ? std::string(".") : spec.initialDir;

    bool useKdialog = PreferKdialog();
    if (!useKdialog && FindExeInPath("zenity").empty() && !FindExeInPath("kdialog").empty()) {
        useKdialog = true;
    }

    std::vector<std::string> args;
    if (useKdialog && !FindExeInPath("kdialog").empty()) {
        args.push_back("kdialog");
        if (!spec.title.empty()) {
            args.push_back("--title");
            args.push_back(spec.title);
        }
        if (spec.pickFolders) {
            args.push_back("--getexistingdirectory");
            args.push_back(startPath);
        } else if (spec.isSave) {
            args.push_back("--getsavefilename");
            args.push_back(startPath);
        } else {
            args.push_back("--getopenfilename");
            args.push_back(startPath);
        }
        if (!spec.pickFolders && !spec.filters.empty()) {
            std::string kfilter;
            for (const auto& f : spec.filters) {
                std::string globs = NormalizeFilterGlobs(f.second);
                if (globs.empty()) continue;
                if (!kfilter.empty()) kfilter += "\n";
                kfilter += f.first + " (" + globs + ")";
            }
            if (!kfilter.empty()) args.push_back(kfilter);
        }
        if (spec.multiSelect && !spec.isSave && !spec.pickFolders) {
            args.push_back("--multiple");
            args.push_back("--separate-output");
        }
    } else if (!FindExeInPath("zenity").empty()) {
        args.push_back("zenity");
        args.push_back("--file-selection");
        if (!spec.title.empty()) args.push_back("--title=" + spec.title);
        if (spec.pickFolders) args.push_back("--directory");
        if (spec.isSave) {
            args.push_back("--save");
            args.push_back("--confirm-overwrite");
        }
        if (spec.multiSelect && !spec.isSave && !spec.pickFolders) {
            args.push_back("--multiple");
            args.push_back("--separator=\n");
        }
        if (!spec.initialDir.empty()) {
            std::string fn = spec.initialDir;
            if (fn.back() != '/') fn.push_back('/');
            args.push_back("--filename=" + fn);
        }
        for (const auto& f : spec.filters) {
            std::string globs = NormalizeFilterGlobs(f.second);
            if (!globs.empty()) {
                args.push_back("--file-filter=" + f.first + " | " + globs);
            }
        }
    } else {
        return results;
    }

    int code = -1;
    std::string output;
    if (!RunProcessCaptureStdout(args, code, output) || code != 0) {
        return results;
    }
    std::stringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) {
            results.push_back(LinuxUtf8ToWide(line));
        }
    }
    return results;
}

} // namespace

// ============================================================================
// Video Window Helpers (Coordinated with MpvPlayer.cpp via LinuxPlatform.h)
// ============================================================================

extern "C" bool ApertureLinuxIsWayland(HWND /*hwnd*/) {
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_GetCurrentVideoDriver) {
        if (const char* drv = sdl.SDL_GetCurrentVideoDriver()) {
            return std::strcmp(drv, "wayland") == 0;
        }
    }
    const char* wlDisplay = std::getenv("WAYLAND_DISPLAY");
    return wlDisplay && *wlDisplay;
}

extern "C" uint64_t ApertureLinuxGetX11WindowId(HWND hwnd) {
    if (ApertureLinuxIsWayland(hwnd)) return 0;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hwnd);
    if (!w) return 0;

    if (w->x11ChildWindow) return static_cast<uint64_t>(w->x11ChildWindow);

    LinuxWindow* root = GetRootWindowLocked(w);
    if (!root || !root->sdlWindow) return 0;

    SDL_SysWMinfo info{};
    if (!QueryWmInfo(root->sdlWindow, info) || info.subsystem != SDL_SYSWM_X11 ||
        !info.info.x11.display || !info.info.x11.window) {
        return 0;
    }

    if (w == root) {
        return static_cast<uint64_t>(info.info.x11.window);
    }

    auto& x11 = X11Api::Get();
    if (!x11.loaded) return 0;

    int relX = static_cast<int>(w->rect.left - root->rect.left);
    int relY = static_cast<int>(w->rect.top - root->rect.top);
    unsigned int cw = static_cast<unsigned int>(std::max<LONG>(1, w->rect.right - w->rect.left));
    unsigned int ch = static_cast<unsigned int>(std::max<LONG>(1, w->rect.bottom - w->rect.top));

    unsigned long child = x11.XCreateSimpleWindow(
        info.info.x11.display, info.info.x11.window,
        relX, relY, cw, ch, 0, 0, 0);
    if (!child) return 0;

    w->x11Display = info.info.x11.display;
    w->x11ParentWindow = info.info.x11.window;
    w->x11ChildWindow = child;
    w->isVideoChild = true;
    if (w->visible) x11.XMapWindow(w->x11Display, child);
    x11.XFlush(w->x11Display);
    return static_cast<uint64_t>(child);
}

extern "C" void ApertureLinuxPresentVideoFrame(HWND hVideoWnd, const uint8_t* bgraPixels, int width, int height) {
    if (!bgraPixels || width <= 0 || height <= 0) return;
    bool woke = false;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        LinuxWindow* vw = GetWindowLocked(hVideoWnd);
        if (!vw) return;
        vw->isVideoChild = true;
        vw->videoW = width;
        vw->videoH = height;
        size_t bytes = static_cast<size_t>(width) * height * 4;
        vw->videoPixels.resize(bytes);
        std::memcpy(vw->videoPixels.data(), bgraPixels, bytes);

        if (LinuxWindow* root = GetRootWindowLocked(vw)) {
            root->videoFrameDirty = true;
            woke = true;
        }
    }
    if (woke) LinuxWakeMessageLoop();
}

// ============================================================================
// DXGI SwapChain Presentation & GDI DIB Blit
// ============================================================================

HRESULT IDXGISwapChain1::Present1(UINT /*SyncInterval*/, UINT /*PresentFlags*/, const DXGI_PRESENT_PARAMETERS* /*pPresentParameters*/) {
    if (!m_surface || m_surface->m_pixels.empty()) return S_OK;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(m_hwnd);
    if (!w) w = GetWindowLocked(g_mainHwnd);
    if (w) {
        w->boundSurface = m_surface;
        CompositeAndPresentWindow(w);
    }
    return S_OK;
}

void LinuxBlitDCToBitmap(HDC hdc, uint8_t* dstBGRA, UINT dstW, UINT dstH, const RECT& /*rect*/) {
    if (!hdc || !dstBGRA || dstW == 0 || dstH == 0) return;
    auto* dcObj = reinterpret_cast<LinuxGdiObject*>(hdc);
    auto* bmpObj = reinterpret_cast<LinuxGdiObject*>(dcObj->selectedBitmap);
    if (!bmpObj || bmpObj->dibBits.empty() || bmpObj->width <= 0 || bmpObj->height <= 0) return;

    UINT cw = std::min<UINT>(dstW, static_cast<UINT>(bmpObj->width));
    UINT ch = std::min<UINT>(dstH, static_cast<UINT>(bmpObj->height));
    size_t rowBytes = static_cast<size_t>(cw) * 4;
    for (UINT y = 0; y < ch; ++y) {
        std::memcpy(dstBGRA + static_cast<size_t>(y) * dstW * 4,
                    bmpObj->dibBits.data() + static_cast<size_t>(y) * bmpObj->width * 4,
                    rowBytes);
    }
}

// ============================================================================
// Window Creation, Destruction & Win32 Window APIs
// ============================================================================

ATOM RegisterClassExW(const WNDCLASSEXW* lpwcx) {
    if (!lpwcx || !lpwcx->lpszClassName) return 0;
    std::lock_guard<std::mutex> lock(g_winMtx);
    g_classes[lpwcx->lpszClassName] = LinuxClassInfo{lpwcx->lpszClassName, lpwcx->lpfnWndProc};
    return 1;
}

HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
                     int X, int Y, int nWidth, int nHeight, HWND hWndParent,
                     HMENU /*hMenu*/, HINSTANCE /*hInstance*/, LPVOID lpParam) {
    if (lpClassName && std::wcscmp(lpClassName, TOOLTIPS_CLASSW) == 0) {
        // Lightweight tooltip handle tracked without creating a separate native X11/Wayland popup
    }

    auto* w = new LinuxWindow();
    w->className = lpClassName ? lpClassName : L"";
    w->title = lpWindowName ? lpWindowName : L"Aperture";
    w->style = dwStyle;
    w->exStyle = dwExStyle;
    w->parent = hWndParent;
    w->isLayered = (dwExStyle & WS_EX_LAYERED) != 0;
    w->isVideoChild = (dwStyle & WS_CHILD) != 0 && hWndParent != nullptr;

    int width = (nWidth > 0 && nWidth != CW_USEDEFAULT) ? nWidth : 1280;
    int height = (nHeight > 0 && nHeight != CW_USEDEFAULT) ? nHeight : 720;
    int px = (X != CW_USEDEFAULT) ? X : 100;
    int py = (Y != CW_USEDEFAULT) ? Y : 100;
    w->rect = {px, py, px + width, py + height};
    w->visible = (dwStyle & WS_VISIBLE) != 0;

    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        hwnd = reinterpret_cast<HWND>(g_nextHwndVal++);
        w->hwnd = hwnd;

        auto clsIt = g_classes.find(w->className);
        if (clsIt != g_classes.end()) {
            w->wndProc = clsIt->second.wndProc;
        }
        if (hWndParent) {
            if (LinuxWindow* pw = GetWindowLocked(hWndParent)) {
                pw->children.push_back(hwnd);
            }
        }
        g_windows[hwnd] = w;
        if (!g_mainHwnd && !(dwStyle & WS_CHILD) && !w->isLayered && w->className != TOOLTIPS_CLASSW) {
            g_mainHwnd = hwnd;
            g_foregroundHwnd = hwnd;
            g_focusHwnd = hwnd;
        }
    }

    const bool isTopLevelSdl = !(dwStyle & WS_CHILD) && !w->isLayered &&
                               w->className != TOOLTIPS_CLASSW &&
                               (dwStyle & WS_POPUP) == 0;
    const bool createForPopup = !(dwStyle & WS_CHILD) && !w->isLayered &&
                                w->className != TOOLTIPS_CLASSW && !isTopLevelSdl &&
                                (w->className == L"ApertureClass" || w->className == L"ApertureMainWnd");
    if (isTopLevelSdl || createForPopup) {
        auto& sdl = Sdl2Api::Get();
        if (sdl.available) {
            uint32_t flags = kSDL_WINDOW_ALLOW_HIGHDPI | kSDL_WINDOW_RESIZABLE;
            flags |= w->visible ? kSDL_WINDOW_SHOWN : kSDL_WINDOW_HIDDEN;
            if ((dwStyle & WS_CAPTION) == 0) flags |= kSDL_WINDOW_BORDERLESS;
            int sx = (X == CW_USEDEFAULT) ? kSDL_WINDOWPOS_CENTERED : px;
            int sy = (Y == CW_USEDEFAULT) ? kSDL_WINDOWPOS_CENTERED : py;

            std::string titleUtf8 = LinuxWideToUtf8(w->title);
            w->sdlWindow = sdl.SDL_CreateWindow(titleUtf8.c_str(), sx, sy, width, height, flags);
            if (w->sdlWindow) {
                w->sdlRenderer = sdl.SDL_CreateRenderer(
                    w->sdlWindow, -1, kSDL_RENDERER_ACCELERATED | kSDL_RENDERER_PRESENTVSYNC);
                if (!w->sdlRenderer) {
                    w->sdlRenderer = sdl.SDL_CreateRenderer(w->sdlWindow, -1, 0);
                }
                if (sdl.SDL_GetWindowID) {
                    w->sdlWindowId = sdl.SDL_GetWindowID(w->sdlWindow);
                    std::lock_guard<std::mutex> lock(g_winMtx);
                    g_sdlIdToHwnd[w->sdlWindowId] = hwnd;
                }
                RegisterDisplayWakeFdOnce(w->sdlWindow);
                w->dpi = ComputeWindowDpi(w);
                SyncWindowPhysicalRectFromSdl(w);
            }
        }
        if (lpClassName && !(dwStyle & WS_CHILD)) {
            LinuxIpcRegisterWindow(hwnd, lpClassName);
        }
    }

    if (w->wndProc) {
        CREATESTRUCTW cs{};
        cs.lpCreateParams = lpParam;
        cs.hwndParent = hWndParent;
        cs.cy = static_cast<int>(w->rect.bottom - w->rect.top);
        cs.cx = static_cast<int>(w->rect.right - w->rect.left);
        cs.y = static_cast<int>(w->rect.top);
        cs.x = static_cast<int>(w->rect.left);
        cs.style = static_cast<LONG>(dwStyle);
        cs.lpszName = lpWindowName;
        cs.lpszClass = lpClassName;
        cs.dwExStyle = dwExStyle;
        w->wndProc(hwnd, WM_CREATE, 0, reinterpret_cast<LPARAM>(&cs));
    }
    return hwnd;
}

BOOL DestroyWindow(HWND hWnd) {
    LinuxWindow* w = nullptr;
    WNDPROC proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        w = GetWindowLocked(hWnd);
        if (!w) return FALSE;
        proc = w->wndProc;
    }

    if (proc) proc(hWnd, WM_DESTROY, 0, 0);
    LinuxIpcUnregisterWindow(hWnd);

    std::lock_guard<std::mutex> lock(g_winMtx);
    w = GetWindowLocked(hWnd);
    if (!w) return TRUE;

    if (w->x11Display && w->x11ChildWindow) {
        auto& x11 = X11Api::Get();
        if (x11.loaded) {
            x11.XDestroyWindow(w->x11Display, w->x11ChildWindow);
            x11.XFlush(w->x11Display);
        }
        w->x11ChildWindow = 0;
    }

    auto& sdl = Sdl2Api::Get();
    if (w->sdlTexture && sdl.SDL_DestroyTexture) sdl.SDL_DestroyTexture(w->sdlTexture);
    if (w->sdlRenderer && sdl.SDL_DestroyRenderer) sdl.SDL_DestroyRenderer(w->sdlRenderer);
    if (w->sdlWindow && sdl.SDL_DestroyWindow) sdl.SDL_DestroyWindow(w->sdlWindow);
    if (w->sdlWindowId != 0) g_sdlIdToHwnd.erase(w->sdlWindowId);

    if (w->parent) {
        if (LinuxWindow* pw = GetWindowLocked(w->parent)) {
            pw->children.erase(std::remove(pw->children.begin(), pw->children.end(), hWnd), pw->children.end());
        }
    }
    if (g_mainHwnd == hWnd) g_mainHwnd = nullptr;
    if (g_foregroundHwnd == hWnd) g_foregroundHwnd = nullptr;
    if (g_focusHwnd == hWnd) g_focusHwnd = nullptr;
    if (g_captureHwnd == hWnd) g_captureHwnd = nullptr;

    g_windows.erase(hWnd);
    delete w;
    return TRUE;
}

BOOL IsWindow(HWND hWnd) {
    return IsValidLinuxWindow(hWnd) ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    return (w && w->visible) ? TRUE : FALSE;
}

BOOL IsIconic(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    auto& sdl = Sdl2Api::Get();
    if (w->sdlWindow && sdl.SDL_GetWindowFlags) {
        return (sdl.SDL_GetWindowFlags(w->sdlWindow) & kSDL_WINDOW_MINIMIZED) ? TRUE : FALSE;
    }
    return w->minimized ? TRUE : FALSE;
}

BOOL IsZoomed(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    auto& sdl = Sdl2Api::Get();
    if (w->sdlWindow && sdl.SDL_GetWindowFlags) {
        return (sdl.SDL_GetWindowFlags(w->sdlWindow) & kSDL_WINDOW_MAXIMIZED) ? TRUE : FALSE;
    }
    return w->maximized ? TRUE : FALSE;
}

BOOL ShowWindow(HWND hWnd, int nCmdShow) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    BOOL wasVisible = w->visible ? TRUE : FALSE;

    auto& sdl = Sdl2Api::Get();
    switch (nCmdShow) {
        case SW_HIDE:
            w->visible = false;
            if (w->sdlWindow && sdl.SDL_HideWindow) sdl.SDL_HideWindow(w->sdlWindow);
            break;
        case SW_MINIMIZE:
        case SW_SHOWMINIMIZED:
            w->minimized = true;
            w->maximized = false;
            if (w->sdlWindow && sdl.SDL_MinimizeWindow) sdl.SDL_MinimizeWindow(w->sdlWindow);
            break;
        case SW_MAXIMIZE:
            w->visible = true;
            w->minimized = false;
            w->maximized = true;
            if (w->sdlWindow) {
                if (sdl.SDL_ShowWindow) sdl.SDL_ShowWindow(w->sdlWindow);
                if (sdl.SDL_MaximizeWindow) sdl.SDL_MaximizeWindow(w->sdlWindow);
            }
            break;
        case SW_RESTORE:
            w->visible = true;
            w->minimized = false;
            w->maximized = false;
            if (w->sdlWindow) {
                if (sdl.SDL_ShowWindow) sdl.SDL_ShowWindow(w->sdlWindow);
                if (sdl.SDL_RestoreWindow) sdl.SDL_RestoreWindow(w->sdlWindow);
            }
            break;
        default:
            w->visible = true;
            w->minimized = false;
            if (w->sdlWindow && sdl.SDL_ShowWindow) sdl.SDL_ShowWindow(w->sdlWindow);
            break;
    }

    if (w->x11ChildWindow) SyncX11ChildVideoGeometry(w);
    if (w->isLayered || w->isVideoChild) {
        if (LinuxWindow* root = GetRootWindowLocked(w)) {
            CompositeAndPresentWindow(root);
        }
    }
    return wasVisible;
}

BOOL SetWindowPos(HWND hWnd, HWND /*hWndInsertAfter*/, int X, int Y, int cx, int cy, UINT uFlags) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;

    int newX = (uFlags & SWP_NOMOVE) ? static_cast<int>(w->rect.left) : X;
    int newY = (uFlags & SWP_NOMOVE) ? static_cast<int>(w->rect.top) : Y;
    if ((uFlags & SWP_NOMOVE) == 0 && (w->style & WS_CHILD) && w->parent) {
        if (LinuxWindow* pw = GetWindowLocked(w->parent)) {
            newX += static_cast<int>(pw->rect.left);
            newY += static_cast<int>(pw->rect.top);
        }
    }
    int newW = (uFlags & SWP_NOSIZE) ? static_cast<int>(w->rect.right - w->rect.left) : std::max(1, cx);
    int newH = (uFlags & SWP_NOSIZE) ? static_cast<int>(w->rect.bottom - w->rect.top) : std::max(1, cy);

    w->rect = {newX, newY, newX + newW, newY + newH};
    if (uFlags & SWP_SHOWWINDOW) w->visible = true;
    if (uFlags & SWP_HIDEWINDOW) w->visible = false;

    if (w->sdlWindow) {
        auto& sdl = Sdl2Api::Get();
        double scale = std::max(1.0, GetSdlPointToPixelScale(w));
        if ((uFlags & SWP_NOMOVE) == 0 && sdl.SDL_SetWindowPosition) {
            sdl.SDL_SetWindowPosition(w->sdlWindow,
                                      static_cast<int>(std::round(newX / scale)),
                                      static_cast<int>(std::round(newY / scale)));
        }
        if ((uFlags & SWP_NOSIZE) == 0 && sdl.SDL_SetWindowSize) {
            sdl.SDL_SetWindowSize(w->sdlWindow,
                                  std::max(1, static_cast<int>(std::round(newW / scale))),
                                  std::max(1, static_cast<int>(std::round(newH / scale))));
        }
        if ((uFlags & SWP_SHOWWINDOW) && sdl.SDL_ShowWindow) sdl.SDL_ShowWindow(w->sdlWindow);
        if ((uFlags & SWP_HIDEWINDOW) && sdl.SDL_HideWindow) sdl.SDL_HideWindow(w->sdlWindow);
    }

    if (w->x11ChildWindow) SyncX11ChildVideoGeometry(w);
    if (w->isLayered) {
        w->overlayScreenRect = {newX, newY, newX + newW, newY + newH};
        if (LinuxWindow* root = GetRootWindowLocked(w)) {
            CompositeAndPresentWindow(root);
        }
    }
    if ((uFlags & SWP_NOSIZE) == 0 && w->wndProc && !w->isLayered) {
        QueueWindowMsgLocked(hWnd, WM_SIZE, 0, MAKELPARAM(newW, newH));
    }
    return TRUE;
}

BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL /*bRepaint*/) {
    return SetWindowPos(hWnd, nullptr, X, Y, nWidth, nHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

BOOL GetClientRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    if (w->sdlWindow) SyncWindowPhysicalRectFromSdl(w);
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = std::max<LONG>(1, w->rect.right - w->rect.left);
    lpRect->bottom = std::max<LONG>(1, w->rect.bottom - w->rect.top);
    return TRUE;
}

BOOL GetWindowRect(HWND hWnd, LPRECT lpRect) {
    if (!lpRect) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    if (w->sdlWindow) SyncWindowPhysicalRectFromSdl(w);
    *lpRect = w->rect;
    return TRUE;
}

BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    if (w->sdlWindow) SyncWindowPhysicalRectFromSdl(w);
    lpPoint->x += w->rect.left;
    lpPoint->y += w->rect.top;
    return TRUE;
}

BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    if (w->sdlWindow) SyncWindowPhysicalRectFromSdl(w);
    lpPoint->x -= w->rect.left;
    lpPoint->y -= w->rect.top;
    return TRUE;
}

int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints) {
    if (!lpPoints || cPoints == 0) return 0;
    POINT srcOrg{0, 0}, dstOrg{0, 0};
    if (hWndFrom) ClientToScreen(hWndFrom, &srcOrg);
    if (hWndTo) ClientToScreen(hWndTo, &dstOrg);
    LONG dx = srcOrg.x - dstOrg.x;
    LONG dy = srcOrg.y - dstOrg.y;
    for (UINT i = 0; i < cPoints; ++i) {
        lpPoints[i].x += dx;
        lpPoints[i].y += dy;
    }
    return static_cast<int>(MAKELPARAM(dx & 0xFFFF, dy & 0xFFFF));
}

HWND GetParent(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    return w ? w->parent : nullptr;
}

HWND SetParent(HWND hWndChild, HWND hWndNewParent) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWndChild);
    if (!w) return nullptr;
    HWND old = w->parent;
    w->parent = hWndNewParent;
    return old;
}

HWND GetAncestor(HWND hWnd, UINT gaFlags) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return nullptr;
    if (gaFlags == 1) return w->parent;
    LinuxWindow* root = GetRootWindowLocked(w);
    return root ? root->hwnd : hWnd;
}

BOOL SetForegroundWindow(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    g_foregroundHwnd = hWnd;
    g_focusHwnd = hWnd;
    auto& sdl = Sdl2Api::Get();
    if (w->sdlWindow && sdl.SDL_RaiseWindow) sdl.SDL_RaiseWindow(w->sdlWindow);
    return TRUE;
}

HWND GetForegroundWindow() {
    std::lock_guard<std::mutex> lock(g_winMtx);
    return g_foregroundHwnd ? g_foregroundHwnd : g_mainHwnd;
}

HWND SetFocus(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    HWND old = g_focusHwnd;
    if (GetWindowLocked(hWnd)) g_focusHwnd = hWnd;
    return old;
}

HWND GetFocus() {
    std::lock_guard<std::mutex> lock(g_winMtx);
    return g_focusHwnd ? g_focusHwnd : g_mainHwnd;
}

BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    w->title = lpString ? lpString : L"";
    auto& sdl = Sdl2Api::Get();
    if (w->sdlWindow && sdl.SDL_SetWindowTitle) {
        sdl.SDL_SetWindowTitle(w->sdlWindow, LinuxWideToUtf8(w->title).c_str());
    }
    return TRUE;
}

int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount) {
    if (!lpString || nMaxCount <= 0) return 0;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) { lpString[0] = L'\0'; return 0; }
    int n = std::min(nMaxCount - 1, static_cast<int>(w->title.size()));
    std::wmemcpy(lpString, w->title.c_str(), n);
    lpString[n] = L'\0';
    return n;
}

LONG_PTR GetWindowLongPtrW(HWND hWnd, int nIndex) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return 0;
    switch (nIndex) {
        case GWL_STYLE: return static_cast<LONG_PTR>(w->style);
        case GWL_EXSTYLE: return static_cast<LONG_PTR>(w->exStyle);
        case GWLP_USERDATA: return w->userData;
        case GWLP_WNDPROC: return reinterpret_cast<LONG_PTR>(w->wndProc);
        default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return 0;
    LONG_PTR old = 0;
    switch (nIndex) {
        case GWL_STYLE: old = w->style; w->style = static_cast<DWORD>(dwNewLong); break;
        case GWL_EXSTYLE: old = w->exStyle; w->exStyle = static_cast<DWORD>(dwNewLong); break;
        case GWLP_USERDATA: old = w->userData; w->userData = dwNewLong; break;
        case GWLP_WNDPROC: old = reinterpret_cast<LONG_PTR>(w->wndProc); w->wndProc = reinterpret_cast<WNDPROC>(dwNewLong); break;
        default: break;
    }
    return old;
}

BOOL GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl) {
    if (!lpwndpl) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hWnd);
    if (!w) return FALSE;
    if (w->sdlWindow) SyncWindowPhysicalRectFromSdl(w);
    std::memset(lpwndpl, 0, sizeof(*lpwndpl));
    lpwndpl->length = sizeof(*lpwndpl);
    lpwndpl->showCmd = w->maximized ? SW_MAXIMIZE : (w->minimized ? SW_MINIMIZE : (w->visible ? SW_SHOWNORMAL : SW_HIDE));
    lpwndpl->rcNormalPosition = w->rect;
    return TRUE;
}

BOOL SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl) {
    if (!lpwndpl) return FALSE;
    MoveWindow(hWnd, lpwndpl->rcNormalPosition.left, lpwndpl->rcNormalPosition.top,
               lpwndpl->rcNormalPosition.right - lpwndpl->rcNormalPosition.left,
               lpwndpl->rcNormalPosition.bottom - lpwndpl->rcNormalPosition.top, TRUE);
    ShowWindow(hWnd, static_cast<int>(lpwndpl->showCmd));
    return TRUE;
}

BOOL AdjustWindowRectEx(LPRECT /*lpRect*/, DWORD /*dwStyle*/, BOOL /*bMenu*/, DWORD /*dwExStyle*/) { return TRUE; }
BOOL AdjustWindowRectExForDpi(LPRECT /*lpRect*/, DWORD /*dwStyle*/, BOOL /*bMenu*/, DWORD /*dwExStyle*/, UINT /*dpi*/) { return TRUE; }

// ============================================================================
// Message Loop, SDL Event Routing & Timers
// ============================================================================

LRESULT DefWindowProcW(HWND /*hWnd*/, UINT /*Msg*/, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    return 0;
}

BOOL PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        HWND target = hWnd ? hWnd : g_mainHwnd;
        if (!target) return FALSE;
        QueueWindowMsgLocked(target, Msg, wParam, lParam);
    }
    LinuxWakeMessageLoop();
    return TRUE;
}

LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (Msg == WM_COPYDATA && LinuxIpcIsRemoteWindow(hWnd)) {
        DWORD_PTR res = 0;
        return LinuxIpcSendCopyData(hWnd, reinterpret_cast<const COPYDATASTRUCT*>(lParam), 3000, &res);
    }
    WNDPROC proc = nullptr;
    HWND target = hWnd;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        if (!target) target = g_mainHwnd;
        if (LinuxWindow* w = GetWindowLocked(target)) proc = w->wndProc;
    }
    return proc ? proc(target, Msg, wParam, lParam) : 0;
}

LRESULT SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam,
                            UINT /*fuFlags*/, UINT uTimeout, DWORD_PTR* lpdwResult) {
    if (Msg == WM_COPYDATA && LinuxIpcIsRemoteWindow(hWnd)) {
        return LinuxIpcSendCopyData(hWnd, reinterpret_cast<const COPYDATASTRUCT*>(lParam), uTimeout, lpdwResult);
    }
    LRESULT r = SendMessageW(hWnd, Msg, wParam, lParam);
    if (lpdwResult) *lpdwResult = static_cast<DWORD_PTR>(r);
    return 1;
}

void PostQuitMessage(int nExitCode) {
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        g_quitPosted = true;
        g_quitCode = nExitCode;
        QueueWindowMsgLocked(nullptr, WM_QUIT, static_cast<WPARAM>(nExitCode), 0);
    }
    LinuxWakeMessageLoop();
}

UINT RegisterWindowMessageW(LPCWSTR lpString) {
    if (!lpString) return 0;
    std::lock_guard<std::mutex> lock(g_winMtx);
    for (const auto& kv : g_registeredMsgs) {
        if (kv.second == lpString) return kv.first;
    }
    UINT id = g_nextRegMsg++;
    g_registeredMsgs[id] = lpString;
    return id;
}

static HWND ResolveTargetWindowLocked(uint32_t sdlWindowID) {
    if (sdlWindowID != 0) {
        auto it = g_sdlIdToHwnd.find(sdlWindowID);
        if (it != g_sdlIdToHwnd.end() && GetWindowLocked(it->second)) {
            return it->second;
        }
    }
    return g_mainHwnd;
}

static void PumpSdlEventsAndTimers() {
    // 1. Pump cross-process WM_COPYDATA IPC messages
    LinuxIpcPumpPending([](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        return SendMessageW(hwnd, msg, wParam, lParam);
    });

    // 2. Pump SDL2 events routed by windowID
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_PollEvent) {
        SDL_Event ev{};
        while (sdl.SDL_PollEvent(&ev)) {
            std::unique_lock<std::mutex> lock(g_winMtx);
            if (ev.type == kSDL_QUIT) {
                if (g_mainHwnd) QueueWindowMsgLocked(g_mainHwnd, WM_CLOSE, 0, 0);
                continue;
            }
            if (ev.type == kSDL_WINDOWEVENT) {
                HWND target = ResolveTargetWindowLocked(ev.window.windowID);
                LinuxWindow* w = GetWindowLocked(target);
                if (!target || !w) continue;
                switch (ev.window.event) {
                    case kSDL_WINDOWEVENT_CLOSE:
                        QueueWindowMsgLocked(target, WM_CLOSE, 0, 0);
                        break;
                    case kSDL_WINDOWEVENT_SIZE_CHANGED: {
                        UINT oldDpi = w->dpi;
                        UINT newDpi = ComputeWindowDpi(w);
                        SyncWindowPhysicalRectFromSdl(w);
                        int pw = std::max(1, static_cast<int>(w->rect.right - w->rect.left));
                        int ph = std::max(1, static_cast<int>(w->rect.bottom - w->rect.top));
                        if (newDpi != oldDpi) {
                            w->dpi = newDpi;
                            RECT suggested = w->rect;
                            WNDPROC proc = w->wndProc;
                            if (proc) {
                                lock.unlock();
                                proc(target, WM_DPICHANGED, MAKEWPARAM(newDpi, newDpi), reinterpret_cast<LPARAM>(&suggested));
                                lock.lock();
                                w = GetWindowLocked(target);
                                if (!w) break;
                                pw = std::max(1, static_cast<int>(w->rect.right - w->rect.left));
                                ph = std::max(1, static_cast<int>(w->rect.bottom - w->rect.top));
                            }
                        }
                        QueueWindowMsgLocked(target, WM_SIZE,
                                             w->maximized ? SIZE_MAXIMIZED : (w->minimized ? SIZE_MINIMIZED : SIZE_RESTORED),
                                             MAKELPARAM(pw, ph));
                        break;
                    }
                    case kSDL_WINDOWEVENT_MOVED:
                        SyncWindowPhysicalRectFromSdl(w);
                        QueueWindowMsgLocked(target, WM_MOVE, 0,
                                             MAKELPARAM(static_cast<int>(w->rect.left), static_cast<int>(w->rect.top)));
                        break;
                    case kSDL_WINDOWEVENT_EXPOSED:
                    case kSDL_WINDOWEVENT_SHOWN:
                        QueueWindowMsgLocked(target, WM_PAINT, 0, 0);
                        break;
                    case kSDL_WINDOWEVENT_MINIMIZED:
                        w->minimized = true;
                        w->maximized = false;
                        QueueWindowMsgLocked(target, WM_SIZE, SIZE_MINIMIZED, 0);
                        break;
                    case kSDL_WINDOWEVENT_MAXIMIZED:
                        w->maximized = true;
                        w->minimized = false;
                        SyncWindowPhysicalRectFromSdl(w);
                        QueueWindowMsgLocked(target, WM_SIZE, SIZE_MAXIMIZED,
                                             MAKELPARAM(w->rect.right - w->rect.left, w->rect.bottom - w->rect.top));
                        break;
                    case kSDL_WINDOWEVENT_RESTORED:
                        w->minimized = false;
                        w->maximized = false;
                        SyncWindowPhysicalRectFromSdl(w);
                        QueueWindowMsgLocked(target, WM_SIZE, SIZE_RESTORED,
                                             MAKELPARAM(w->rect.right - w->rect.left, w->rect.bottom - w->rect.top));
                        break;
                    case kSDL_WINDOWEVENT_FOCUS_GAINED:
                        g_foregroundHwnd = target;
                        g_focusHwnd = target;
                        if (sdl.SDL_GetModState) UpdateModifierKeyState(sdl.SDL_GetModState());
                        QueueWindowMsgLocked(target, WM_ACTIVATE, WA_ACTIVE, 0);
                        QueueWindowMsgLocked(target, WM_SETFOCUS, 0, 0);
                        break;
                    case kSDL_WINDOWEVENT_FOCUS_LOST:
                        g_keyDown.fill(false);
                        QueueWindowMsgLocked(target, WM_ACTIVATE, WA_INACTIVE, 0);
                        QueueWindowMsgLocked(target, WM_KILLFOCUS, 0, 0);
                        break;
                    case kSDL_WINDOWEVENT_ENTER:
                        w->mouseInside = true;
                        break;
                    case kSDL_WINDOWEVENT_LEAVE:
                        w->mouseInside = false;
                        if (w->trackingMouseLeave) {
                            w->trackingMouseLeave = false;
                            QueueWindowMsgLocked(target, WM_MOUSELEAVE, 0, 0);
                        }
                        break;
                    case kSDL_WINDOWEVENT_DISPLAY_CHANGED: {
                        UINT oldDpi = w->dpi;
                        UINT newDpi = ComputeWindowDpi(w);
                        if (newDpi != oldDpi) {
                            w->dpi = newDpi;
                            SyncWindowPhysicalRectFromSdl(w);
                            RECT suggested = w->rect;
                            WNDPROC proc = w->wndProc;
                            if (proc) {
                                lock.unlock();
                                proc(target, WM_DPICHANGED, MAKEWPARAM(newDpi, newDpi), reinterpret_cast<LPARAM>(&suggested));
                                lock.lock();
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            } else if (ev.type == kSDL_KEYDOWN || ev.type == kSDL_KEYUP) {
                HWND target = ResolveTargetWindowLocked(ev.key.windowID);
                if (!target) continue;
                int vk = MapSdlKeycodeToVk(ev.key.keysym.sym, ev.key.keysym.scancode);
                bool down = (ev.type == kSDL_KEYDOWN);
                if (vk > 0 && vk < 256) g_keyDown[vk] = down;
                // Preserve combined L/R modifier state cleanly from SDL's bitmask when one side releases while the other stays held.
                UpdateModifierKeyState(ev.key.keysym.mod);
                if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
                    if (down) g_keyDown[vk] = true;
                }
                if (vk > 0) {
                    LPARAM lp = (ev.key.repeat ? (1 << 30) : 0) | 1;
                    QueueWindowMsgLocked(target, down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(vk), lp);
                }
            } else if (ev.type == kSDL_TEXTINPUT) {
                HWND target = ResolveTargetWindowLocked(ev.text.windowID);
                if (!target) continue;
                std::wstring wch = LinuxUtf8ToWide(ev.text.text);
                for (wchar_t ch : wch) {
                    QueueWindowMsgLocked(target, WM_CHAR, static_cast<WPARAM>(ch), 1);
                }
            } else if (ev.type == kSDL_MOUSEMOTION) {
                HWND target = ResolveTargetWindowLocked(ev.motion.windowID);
                LinuxWindow* w = GetWindowLocked(target);
                if (!target || !w) continue;
                double scale = GetSdlPointToPixelScale(w);
                int mx = static_cast<int>(std::round(ev.motion.x * scale));
                int my = static_cast<int>(std::round(ev.motion.y * scale));
                WPARAM mk = BuildMouseModifierWParam(ev.motion.state);
                QueueWindowMsgLocked(target, WM_SETCURSOR, reinterpret_cast<WPARAM>(target), MAKELPARAM(1, WM_MOUSEMOVE));
                QueueWindowMsgLocked(target, WM_MOUSEMOVE, mk, MAKELPARAM(mx, my));
            } else if (ev.type == kSDL_MOUSEBUTTONDOWN || ev.type == kSDL_MOUSEBUTTONUP) {
                HWND target = ResolveTargetWindowLocked(ev.button.windowID);
                LinuxWindow* w = GetWindowLocked(target);
                if (!target || !w) continue;
                double scale = GetSdlPointToPixelScale(w);
                int mx = static_cast<int>(std::round(ev.button.x * scale));
                int my = static_cast<int>(std::round(ev.button.y * scale));
                bool down = (ev.type == kSDL_MOUSEBUTTONDOWN);
                bool dbl = down && (ev.button.clicks >= 2) && ((ev.button.clicks % 2) == 0);

                uint32_t btnState = sdl.SDL_GetMouseState ? sdl.SDL_GetMouseState(nullptr, nullptr) : 0;
                WPARAM mk = BuildMouseModifierWParam(btnState);

                UINT winMsg = 0;
                if (ev.button.button == 1) {
                    winMsg = dbl ? WM_LBUTTONDBLCLK : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
                    if (down) mk |= MK_LBUTTON;
                } else if (ev.button.button == 2) {
                    winMsg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                    if (down) mk |= MK_MBUTTON;
                } else if (ev.button.button == 3) {
                    winMsg = dbl ? WM_RBUTTONDBLCLK : (down ? WM_RBUTTONDOWN : WM_RBUTTONUP);
                    if (down) mk |= MK_RBUTTON;
                } else if (ev.button.button == 4) {
                    winMsg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    mk = MAKEWPARAM(mk, XBUTTON1);
                } else if (ev.button.button == 5) {
                    winMsg = down ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    mk = MAKEWPARAM(mk, XBUTTON2);
                }
                if (winMsg != 0) {
                    QueueWindowMsgLocked(target, winMsg, mk, MAKELPARAM(mx, my));
                }
            } else if (ev.type == kSDL_MOUSEWHEEL) {
                HWND target = ResolveTargetWindowLocked(ev.wheel.windowID);
                LinuxWindow* w = GetWindowLocked(target);
                if (!target || !w) continue;
                int mx = 0, my = 0;
                uint32_t btnState = sdl.SDL_GetMouseState ? sdl.SDL_GetMouseState(&mx, &my) : 0;
                double scale = GetSdlPointToPixelScale(w);
                POINT screenPt{
                    static_cast<LONG>(w->rect.left + std::round(mx * scale)),
                    static_cast<LONG>(w->rect.top + std::round(my * scale))
                };
                int sign = (ev.wheel.direction == kSDL_MOUSEWHEEL_FLIPPED) ? -1 : 1;
                WPARAM mk = BuildMouseModifierWParam(btnState);
                if (ev.wheel.y != 0 || std::abs(ev.wheel.preciseY) > 0.001f) {
                    double dy = (std::abs(ev.wheel.preciseY) > 0.001f) ? ev.wheel.preciseY : static_cast<double>(ev.wheel.y);
                    int16_t delta = static_cast<int16_t>(std::round(dy * sign * 120.0));
                    QueueWindowMsgLocked(target, WM_MOUSEWHEEL, MAKEWPARAM(mk, static_cast<uint16_t>(delta)),
                                         MAKELPARAM(screenPt.x, screenPt.y));
                }
                if (ev.wheel.x != 0 || std::abs(ev.wheel.preciseX) > 0.001f) {
                    double dx = (std::abs(ev.wheel.preciseX) > 0.001f) ? ev.wheel.preciseX : static_cast<double>(ev.wheel.x);
                    int16_t delta = static_cast<int16_t>(std::round(dx * sign * 120.0));
                    QueueWindowMsgLocked(target, WM_MOUSEHWHEEL, MAKEWPARAM(mk, static_cast<uint16_t>(delta)),
                                         MAKELPARAM(screenPt.x, screenPt.y));
                }
            } else if (ev.type == kSDL_DROPBEGIN) {
                g_pendingDrops.clear();
                g_pendingDropWindowId = ev.drop.windowID;
            } else if (ev.type == kSDL_DROPFILE && ev.drop.file) {
                g_pendingDrops.emplace_back(ev.drop.file);
                g_pendingDropWindowId = ev.drop.windowID;
                if (sdl.SDL_free) sdl.SDL_free(ev.drop.file);
            } else if (ev.type == kSDL_DROPCOMPLETE) {
                if (!g_pendingDrops.empty()) {
                    uint32_t winId = ev.drop.windowID ? ev.drop.windowID : g_pendingDropWindowId;
                    HWND target = ResolveTargetWindowLocked(winId);
                    if (target) {
                        auto* dd = new LinuxDropData();
                        for (const auto& f : g_pendingDrops) dd->files.push_back(LinuxUtf8ToWide(f));
                        QueueWindowMsgLocked(target, WM_DROPFILES, reinterpret_cast<WPARAM>(dd), 0);
                    }
                    g_pendingDrops.clear();
                }
                g_pendingDropWindowId = 0;
            }
        }
    }

    // 3. Present any Wayland/SW video frames queued from worker threads on the main SDL/UI thread,
    //    and fire due timers.
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_winMtx);
    for (auto& kv : g_windows) {
        LinuxWindow* root = kv.second;
        if (root && root->videoFrameDirty) {
            root->videoFrameDirty = false;
            CompositeAndPresentWindow(root);
        }
    }
    for (auto& t : g_timers) {
        if (now >= t.nextFire) {
            t.nextFire = now + std::chrono::milliseconds(std::max<UINT>(1, t.intervalMs));
            QueueWindowMsgLocked(t.hwnd, WM_TIMER, static_cast<WPARAM>(t.id), reinterpret_cast<LPARAM>(t.proc));
        }
    }
}

BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT /*wMsgFilterMin*/, UINT /*wMsgFilterMax*/, UINT wRemoveMsg) {
    PumpSdlEventsAndTimers();
    std::lock_guard<std::mutex> lock(g_winMtx);
    for (size_t i = 0; i < g_msgQueue.size(); ++i) {
        if (!hWnd || g_msgQueue[i].hwnd == hWnd || g_msgQueue[i].message == WM_QUIT) {
            if (lpMsg) *lpMsg = g_msgQueue[i];
            if (wRemoveMsg & PM_REMOVE) {
                g_msgQueue.erase(g_msgQueue.begin() + static_cast<ptrdiff_t>(i));
            }
            return TRUE;
        }
    }
    return FALSE;
}

BOOL GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    for (;;) {
        if (PeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, PM_REMOVE)) {
            return (lpMsg && lpMsg->message == WM_QUIT) ? FALSE : TRUE;
        }

        DWORD waitMs = g_displayWakeRegistered ? 500 : 16;
        {
            std::lock_guard<std::mutex> lock(g_winMtx);
            if (g_quitPosted) {
                if (lpMsg) {
                    std::memset(lpMsg, 0, sizeof(*lpMsg));
                    lpMsg->message = WM_QUIT;
                    lpMsg->wParam = static_cast<WPARAM>(g_quitCode);
                }
                return FALSE;
            }
            if (!g_timers.empty()) {
                auto now = std::chrono::steady_clock::now();
                for (const auto& t : g_timers) {
                    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(t.nextFire - now).count();
                    DWORD ms = diff <= 0 ? 0 : static_cast<DWORD>(diff);
                    if (ms < waitMs) waitMs = ms;
                }
            }
        }

        MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
    }
}

BOOL TranslateMessage(const MSG* /*lpMsg*/) { return TRUE; }

LRESULT DispatchMessageW(const MSG* lpMsg) {
    if (!lpMsg) return 0;
    if (lpMsg->message == WM_TIMER && lpMsg->lParam != 0) {
        auto proc = reinterpret_cast<TIMERPROC>(lpMsg->lParam);
        proc(lpMsg->hwnd, WM_TIMER, static_cast<UINT_PTR>(lpMsg->wParam), 0);
        return 0;
    }
    return SendMessageW(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc) {
    UINT_PTR id = nIDEvent ? nIDEvent : 1;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        for (auto& t : g_timers) {
            if (t.hwnd == hWnd && t.id == id) {
                t.intervalMs = std::max<UINT>(1, uElapse);
                t.proc = lpTimerFunc;
                t.nextFire = std::chrono::steady_clock::now() + std::chrono::milliseconds(t.intervalMs);
                return id;
            }
        }
        g_timers.push_back(LinuxTimer{
            hWnd, id, std::max<UINT>(1, uElapse), lpTimerFunc,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<UINT>(1, uElapse))
        });
    }
    LinuxWakeMessageLoop();
    return id;
}

BOOL KillTimer(HWND hWnd, UINT_PTR uIDEvent) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    for (size_t i = 0; i < g_timers.size(); ++i) {
        if (g_timers[i].hwnd == hWnd && g_timers[i].id == uIDEvent) {
            g_timers.erase(g_timers.begin() + static_cast<ptrdiff_t>(i));
            return TRUE;
        }
    }
    return FALSE;
}

BOOL InvalidateRect(HWND hWnd, const RECT* /*lpRect*/, BOOL /*bErase*/) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    HWND target = hWnd ? hWnd : g_mainHwnd;
    if (!target) return FALSE;
    for (const auto& m : g_msgQueue) {
        if (m.hwnd == target && m.message == WM_PAINT) return TRUE;
    }
    QueueWindowMsgLocked(target, WM_PAINT, 0, 0);
    LinuxWakeMessageLoop();
    return TRUE;
}

BOOL UpdateWindow(HWND hWnd) {
    HWND target = hWnd ? hWnd : g_mainHwnd;
    if (target) SendMessageW(target, WM_PAINT, 0, 0);
    return TRUE;
}

// ============================================================================
// Input, Cursor, Monitors, DPI & System Metrics
// ============================================================================

SHORT GetKeyState(int nVirtKey) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    if (nVirtKey < 0 || nVirtKey >= 256) return 0;
    return g_keyDown[nVirtKey] ? static_cast<SHORT>(0x8000) : 0;
}

SHORT GetAsyncKeyState(int nVirtKey) {
    return GetKeyState(nVirtKey);
}

BOOL GetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return FALSE;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available) {
        int gx = 0, gy = 0;
        if (sdl.SDL_GetGlobalMouseState && !ApertureLinuxIsWayland(nullptr)) {
            sdl.SDL_GetGlobalMouseState(&gx, &gy);
            double scale = 1.0;
            {
                std::lock_guard<std::mutex> lock(g_winMtx);
                if (LinuxWindow* w = GetWindowLocked(g_mainHwnd)) {
                    scale = GetSdlPointToPixelScale(w);
                }
            }
            lpPoint->x = static_cast<LONG>(std::round(gx * scale));
            lpPoint->y = static_cast<LONG>(std::round(gy * scale));
            return TRUE;
        }
        if (sdl.SDL_GetMouseState) {
            sdl.SDL_GetMouseState(&gx, &gy);
            std::lock_guard<std::mutex> lock(g_winMtx);
            if (LinuxWindow* w = GetWindowLocked(g_mainHwnd)) {
                double scale = GetSdlPointToPixelScale(w);
                lpPoint->x = w->rect.left + static_cast<LONG>(std::round(gx * scale));
                lpPoint->y = w->rect.top + static_cast<LONG>(std::round(gy * scale));
                return TRUE;
            }
        }
    }
    lpPoint->x = 0;
    lpPoint->y = 0;
    return TRUE;
}

HCURSOR LoadCursorW(HINSTANCE /*hInstance*/, LPCWSTR lpCursorName) {
    return reinterpret_cast<HCURSOR>(const_cast<LPWSTR>(lpCursorName));
}

HCURSOR SetCursor(HCURSOR hCursor) {
    auto& sdl = Sdl2Api::Get();
    if (!sdl.available || !sdl.SDL_CreateSystemCursor || !sdl.SDL_SetCursor) {
        return hCursor;
    }

    int sysId = 0; // SDL_SYSTEM_CURSOR_ARROW
    if (hCursor == IDC_IBEAM) sysId = 1;
    else if (hCursor == IDC_WAIT || hCursor == IDC_APPSTARTING) sysId = 2;
    else if (hCursor == IDC_CROSS) sysId = 3;
    else if (hCursor == IDC_SIZENWSE) sysId = 5;
    else if (hCursor == IDC_SIZENESW) sysId = 6;
    else if (hCursor == IDC_SIZEWE) sysId = 7;
    else if (hCursor == IDC_SIZENS) sysId = 8;
    else if (hCursor == IDC_SIZEALL) sysId = 9;
    else if (hCursor == IDC_NO) sysId = 10;
    else if (hCursor == IDC_HAND) sysId = 11;

    std::lock_guard<std::mutex> lock(g_winMtx);
    SDL_Cursor*& cur = g_systemCursors[sysId];
    if (!cur) cur = sdl.SDL_CreateSystemCursor(sysId);
    if (cur) sdl.SDL_SetCursor(cur);
    return hCursor;
}

int ShowCursor(BOOL bShow) {
    g_cursorVisibleCount += bShow ? 1 : -1;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_ShowCursor) sdl.SDL_ShowCursor(bShow ? 1 : 0);
    return g_cursorVisibleCount;
}

HWND SetCapture(HWND hWnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    HWND old = g_captureHwnd;
    g_captureHwnd = hWnd;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_CaptureMouse) sdl.SDL_CaptureMouse(1);
    return old;
}

BOOL ReleaseCapture() {
    std::lock_guard<std::mutex> lock(g_winMtx);
    g_captureHwnd = nullptr;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_CaptureMouse) sdl.SDL_CaptureMouse(0);
    return TRUE;
}

HWND GetCapture() {
    std::lock_guard<std::mutex> lock(g_winMtx);
    return g_captureHwnd;
}

BOOL TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack) {
    if (!lpEventTrack || !lpEventTrack->hwndTrack) return FALSE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(lpEventTrack->hwndTrack);
    if (!w) return FALSE;
    w->trackingMouseLeave = ((lpEventTrack->dwFlags & TME_CANCEL) == 0) &&
                            ((lpEventTrack->dwFlags & TME_LEAVE) != 0);
    return TRUE;
}

UINT GetDoubleClickTime() { return 500; }

HMONITOR MonitorFromWindow(HWND hwnd, DWORD /*dwFlags*/) {
    auto& sdl = Sdl2Api::Get();
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hwnd ? hwnd : g_mainHwnd);
    if (w && w->sdlWindow && sdl.available && sdl.SDL_GetWindowDisplayIndex) {
        int idx = sdl.SDL_GetWindowDisplayIndex(w->sdlWindow);
        if (idx >= 0) return MakeHMonitor(idx);
    }
    return MakeHMonitor(0);
}

HMONITOR MonitorFromPoint(POINT pt, DWORD /*dwFlags*/) {
    int n = GetNumDisplays();
    for (int i = 0; i < n; ++i) {
        RECT r = GetDisplayBoundsPx(i);
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
            return MakeHMonitor(i);
        }
    }
    return MakeHMonitor(0);
}

HMONITOR MonitorFromRect(LPCRECT lprc, DWORD dwFlags) {
    if (!lprc) return MakeHMonitor(0);
    POINT c{(lprc->left + lprc->right) / 2, (lprc->top + lprc->bottom) / 2};
    return MonitorFromPoint(c, dwFlags);
}

BOOL GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    if (!lpmi) return FALSE;
    int idx = HMonitorToIndex(hMonitor);
    lpmi->rcMonitor = GetDisplayBoundsPx(idx);
    lpmi->rcWork = GetDisplayWorkPx(idx);
    lpmi->dwFlags = (idx == 0) ? 1 : 0; // MONITORINFOF_PRIMARY
    if (lpmi->cbSize >= sizeof(MONITORINFOEXW)) {
        auto* ex = static_cast<MONITORINFOEXW*>(lpmi);
        std::wstring dev = L"\\\\.\\DISPLAY" + std::to_wstring(idx + 1);
        std::memset(ex->szDevice, 0, sizeof(ex->szDevice));
        std::wcsncpy(ex->szDevice, dev.c_str(), 31);
    }
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData) {
    if (!lpfnEnum) return FALSE;
    int n = GetNumDisplays();
    for (int i = 0; i < n; ++i) {
        RECT rc = GetDisplayBoundsPx(i);
        if (lprcClip) {
            RECT inter{};
            if (!IntersectRect(&inter, &rc, lprcClip)) continue;
            rc = inter;
        }
        if (!lpfnEnum(MakeHMonitor(i), hdc, &rc, dwData)) break;
    }
    return TRUE;
}

BOOL EnumDisplaySettingsW(LPCWSTR lpszDeviceName, DWORD /*iModeNum*/, DEVMODEW* lpDevMode) {
    if (!lpDevMode) return FALSE;
    int idx = 0;
    if (lpszDeviceName && *lpszDeviceName) {
        const wchar_t* p = std::wcsstr(lpszDeviceName, L"DISPLAY");
        if (p) {
            int num = std::wcstol(p + 7, nullptr, 10);
            if (num >= 1 && num <= GetNumDisplays()) idx = num - 1;
        }
    }

    RECT bounds = GetDisplayBoundsPx(idx);
    DWORD w = static_cast<DWORD>(std::max<LONG>(1, bounds.right - bounds.left));
    DWORD h = static_cast<DWORD>(std::max<LONG>(1, bounds.bottom - bounds.top));
    DWORD hz = 60;

    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_GetCurrentDisplayMode) {
        SDL_DisplayMode mode{};
        if (sdl.SDL_GetCurrentDisplayMode(idx, &mode) == 0) {
            if (mode.w > 0) w = static_cast<DWORD>(mode.w);
            if (mode.h > 0) h = static_cast<DWORD>(mode.h);
            if (mode.refresh_rate > 0) hz = static_cast<DWORD>(mode.refresh_rate);
        }
    }
    lpDevMode->dmBitsPerPel = 32;
    lpDevMode->dmPelsWidth = w;
    lpDevMode->dmPelsHeight = h;
    lpDevMode->dmDisplayFrequency = hz;
    return TRUE;
}

UINT GetDpiForWindow(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* w = GetWindowLocked(hwnd ? hwnd : g_mainHwnd);
    if (!w) return 96;
    return w->dpi >= 96 ? w->dpi : 96;
}

UINT GetDpiForSystem() {
    return GetDpiForWindow(nullptr);
}

int GetSystemMetrics(int nIndex) {
    int n = GetNumDisplays();
    switch (nIndex) {
        case SM_CXSCREEN: {
            RECT r = GetDisplayBoundsPx(0);
            return static_cast<int>(r.right - r.left);
        }
        case SM_CYSCREEN: {
            RECT r = GetDisplayBoundsPx(0);
            return static_cast<int>(r.bottom - r.top);
        }
        case SM_CMONITORS:
            return n;
        case SM_XVIRTUALSCREEN: {
            RECT u = GetDisplayBoundsPx(0);
            for (int i = 1; i < n; ++i) {
                RECT r = GetDisplayBoundsPx(i);
                UnionRect(&u, &u, &r);
            }
            return static_cast<int>(u.left);
        }
        case SM_YVIRTUALSCREEN: {
            RECT u = GetDisplayBoundsPx(0);
            for (int i = 1; i < n; ++i) {
                RECT r = GetDisplayBoundsPx(i);
                UnionRect(&u, &u, &r);
            }
            return static_cast<int>(u.top);
        }
        case SM_CXVIRTUALSCREEN: {
            RECT u = GetDisplayBoundsPx(0);
            for (int i = 1; i < n; ++i) {
                RECT r = GetDisplayBoundsPx(i);
                UnionRect(&u, &u, &r);
            }
            return static_cast<int>(u.right - u.left);
        }
        case SM_CYVIRTUALSCREEN: {
            RECT u = GetDisplayBoundsPx(0);
            for (int i = 1; i < n; ++i) {
                RECT r = GetDisplayBoundsPx(i);
                UnionRect(&u, &u, &r);
            }
            return static_cast<int>(u.bottom - u.top);
        }
        default:
            return 0;
    }
}

int GetSystemMetricsForDpi(int nIndex, UINT /*dpi*/) {
    return GetSystemMetrics(nIndex);
}

BOOL SystemParametersInfoW(UINT uiAction, UINT /*uiParam*/, PVOID pvParam, UINT /*fWinIni*/) {
    if (uiAction == SPI_GETWORKAREA && pvParam) {
        *static_cast<RECT*>(pvParam) = GetDisplayWorkPx(0);
        return TRUE;
    }
    if (uiAction == SPI_GETWHEELSCROLLLINES && pvParam) {
        *static_cast<UINT*>(pvParam) = 3;
        return TRUE;
    }
    return FALSE;
}

DWORD GetSysColor(int /*nIndex*/) { return RGB(32, 32, 32); }

// ============================================================================
// Rectangles, GDI & Layered Overlay Window
// ============================================================================

BOOL SetRect(LPRECT lprc, int xLeft, int yTop, int xRight, int yBottom) {
    if (!lprc) return FALSE;
    lprc->left = xLeft; lprc->top = yTop; lprc->right = xRight; lprc->bottom = yBottom;
    return TRUE;
}

BOOL SetRectEmpty(LPRECT lprc) { return SetRect(lprc, 0, 0, 0, 0); }

BOOL IsRectEmpty(const RECT* lprc) {
    return !lprc || lprc->left >= lprc->right || lprc->top >= lprc->bottom;
}

BOOL EqualRect(const RECT* lprc1, const RECT* lprc2) {
    if (!lprc1 || !lprc2) return FALSE;
    return lprc1->left == lprc2->left && lprc1->top == lprc2->top &&
           lprc1->right == lprc2->right && lprc1->bottom == lprc2->bottom;
}

BOOL PtInRect(const RECT* lprc, POINT pt) {
    if (!lprc) return FALSE;
    return pt.x >= lprc->left && pt.x < lprc->right && pt.y >= lprc->top && pt.y < lprc->bottom;
}

BOOL IntersectRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2) {
    if (!lprcDst || !lprcSrc1 || !lprcSrc2) return FALSE;
    lprcDst->left = std::max(lprcSrc1->left, lprcSrc2->left);
    lprcDst->top = std::max(lprcSrc1->top, lprcSrc2->top);
    lprcDst->right = std::min(lprcSrc1->right, lprcSrc2->right);
    lprcDst->bottom = std::min(lprcSrc1->bottom, lprcSrc2->bottom);
    if (IsRectEmpty(lprcDst)) { SetRectEmpty(lprcDst); return FALSE; }
    return TRUE;
}

BOOL UnionRect(LPRECT lprcDst, const RECT* lprcSrc1, const RECT* lprcSrc2) {
    if (!lprcDst || !lprcSrc1 || !lprcSrc2) return FALSE;
    if (IsRectEmpty(lprcSrc1)) { *lprcDst = *lprcSrc2; return !IsRectEmpty(lprcDst); }
    if (IsRectEmpty(lprcSrc2)) { *lprcDst = *lprcSrc1; return !IsRectEmpty(lprcDst); }
    lprcDst->left = std::min(lprcSrc1->left, lprcSrc2->left);
    lprcDst->top = std::min(lprcSrc1->top, lprcSrc2->top);
    lprcDst->right = std::max(lprcSrc1->right, lprcSrc2->right);
    lprcDst->bottom = std::max(lprcSrc1->bottom, lprcSrc2->bottom);
    return TRUE;
}

BOOL OffsetRect(LPRECT lprc, int dx, int dy) {
    if (!lprc) return FALSE;
    lprc->left += dx; lprc->right += dx; lprc->top += dy; lprc->bottom += dy;
    return TRUE;
}

BOOL InflateRect(LPRECT lprc, int dx, int dy) {
    if (!lprc) return FALSE;
    lprc->left -= dx; lprc->top -= dy; lprc->right += dx; lprc->bottom += dy;
    return TRUE;
}

HDC GetDC(HWND hWnd) {
    auto* dc = new LinuxGdiObject();
    dc->kind = LinuxGdiObject::DC;
    dc->dcHwnd = hWnd;
    return reinterpret_cast<HDC>(dc);
}

int ReleaseDC(HWND /*hWnd*/, HDC hDC) {
    delete reinterpret_cast<LinuxGdiObject*>(hDC);
    return 1;
}

int GetDeviceCaps(HDC /*hdc*/, int /*index*/) { return 96; }

HDC CreateCompatibleDC(HDC /*hdc*/) {
    auto* dc = new LinuxGdiObject();
    dc->kind = LinuxGdiObject::DC;
    return reinterpret_cast<HDC>(dc);
}

BOOL DeleteDC(HDC hdc) {
    delete reinterpret_cast<LinuxGdiObject*>(hdc);
    return TRUE;
}

HBITMAP CreateDIBSection(HDC /*hdc*/, const BITMAPINFO* pbmi, UINT /*usage*/,
                         void** ppvBits, HANDLE /*hSection*/, DWORD /*offset*/) {
    if (!pbmi) return nullptr;
    int w = std::max(1, static_cast<int>(pbmi->bmiHeader.biWidth));
    int h = std::max(1, static_cast<int>(std::abs(pbmi->bmiHeader.biHeight)));
    auto* bmp = new LinuxGdiObject();
    bmp->kind = LinuxGdiObject::Bitmap;
    bmp->width = w;
    bmp->height = h;
    bmp->dibBits.resize(static_cast<size_t>(w) * h * 4, 0);
    if (ppvBits) *ppvBits = bmp->dibBits.data();
    return reinterpret_cast<HBITMAP>(bmp);
}

HBITMAP CreateBitmap(int nWidth, int nHeight, UINT /*nPlanes*/, UINT /*nBitCount*/, const void* /*lpBits*/) {
    auto* bmp = new LinuxGdiObject();
    bmp->kind = LinuxGdiObject::Bitmap;
    bmp->width = std::max(1, nWidth);
    bmp->height = std::max(1, nHeight);
    bmp->dibBits.resize(static_cast<size_t>(bmp->width) * bmp->height * 4, 0);
    return reinterpret_cast<HBITMAP>(bmp);
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    auto* dc = reinterpret_cast<LinuxGdiObject*>(hdc);
    if (!dc) return nullptr;
    HGDIOBJ old = dc->selectedBitmap;
    dc->selectedBitmap = reinterpret_cast<HBITMAP>(h);
    return old;
}

BOOL DeleteObject(HGDIOBJ ho) {
    delete reinterpret_cast<LinuxGdiObject*>(ho);
    return TRUE;
}

HBRUSH CreateSolidBrush(COLORREF color) {
    auto* br = new LinuxGdiObject();
    br->kind = LinuxGdiObject::Brush;
    br->brushColor = color;
    return reinterpret_cast<HBRUSH>(br);
}

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

BOOL UpdateLayeredWindow(HWND hWnd, HDC /*hdcDst*/, POINT* pptDst, SIZE* psize,
                         HDC hdcSrc, POINT* pptSrc, COLORREF /*crKey*/,
                         BLENDFUNCTION* pblend, DWORD /*dwFlags*/) {
    if (!hWnd || !hdcSrc) return FALSE;
    auto* dcObj = reinterpret_cast<LinuxGdiObject*>(hdcSrc);
    auto* bmpObj = reinterpret_cast<LinuxGdiObject*>(dcObj->selectedBitmap);
    if (!bmpObj || bmpObj->dibBits.empty() || bmpObj->width <= 0 || bmpObj->height <= 0) {
        return FALSE;
    }

    int srcX = pptSrc ? std::max<int>(0, pptSrc->x) : 0;
    int srcY = pptSrc ? std::max<int>(0, pptSrc->y) : 0;
    int w = psize ? psize->cx : bmpObj->width;
    int h = psize ? psize->cy : bmpObj->height;
    if (srcX >= bmpObj->width || srcY >= bmpObj->height || w <= 0 || h <= 0) return FALSE;
    w = std::min(w, bmpObj->width - srcX);
    h = std::min(h, bmpObj->height - srcY);
    uint8_t constAlpha = pblend ? pblend->SourceConstantAlpha : 255;

    std::lock_guard<std::mutex> lock(g_winMtx);
    LinuxWindow* ov = GetWindowLocked(hWnd);
    if (!ov) return FALSE;

    int dstX = pptDst ? static_cast<int>(pptDst->x) : static_cast<int>(ov->rect.left);
    int dstY = pptDst ? static_cast<int>(pptDst->y) : static_cast<int>(ov->rect.top);
    ov->rect = {dstX, dstY, dstX + w, dstY + h};
    ov->overlayScreenRect = ov->rect;
    ov->overlayW = w;
    ov->overlayH = h;
    ov->overlayPixels.resize(static_cast<size_t>(w) * h * 4);

    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = bmpObj->dibBits.data() + (static_cast<size_t>(srcY + y) * bmpObj->width + srcX) * 4;
        uint8_t* drow = ov->overlayPixels.data() + static_cast<size_t>(y) * w * 4;
        if (constAlpha == 255) {
            std::memcpy(drow, srow, static_cast<size_t>(w) * 4);
        } else {
            for (int x = 0; x < w; ++x) {
                drow[x * 4 + 0] = static_cast<uint8_t>((static_cast<unsigned>(srow[x * 4 + 0]) * constAlpha + 127) / 255);
                drow[x * 4 + 1] = static_cast<uint8_t>((static_cast<unsigned>(srow[x * 4 + 1]) * constAlpha + 127) / 255);
                drow[x * 4 + 2] = static_cast<uint8_t>((static_cast<unsigned>(srow[x * 4 + 2]) * constAlpha + 127) / 255);
                drow[x * 4 + 3] = static_cast<uint8_t>((static_cast<unsigned>(srow[x * 4 + 3]) * constAlpha + 127) / 255);
            }
        }
    }

    if (ov->visible) {
        if (LinuxWindow* root = GetWindowLocked(g_mainHwnd)) {
            CompositeAndPresentWindow(root);
        }
    }
    return TRUE;
}

HRESULT DwmSetWindowAttribute(HWND /*hwnd*/, DWORD /*dwAttribute*/, LPCVOID /*pvAttribute*/, DWORD /*cbAttribute*/) { return S_OK; }
HRESULT DwmExtendFrameIntoClientArea(HWND /*hWnd*/, const MARGINS* /*pMarInset*/) { return S_OK; }
HRESULT DwmFlush() { return S_OK; }

// ============================================================================
// GlobalAlloc & Clipboard
// ============================================================================

HGLOBAL GlobalAlloc(UINT /*uFlags*/, SIZE_T dwBytes) {
    return reinterpret_cast<HGLOBAL>(new std::vector<uint8_t>(dwBytes, 0));
}

LPVOID GlobalLock(HGLOBAL hMem) {
    if (!hMem) return nullptr;
    auto* v = reinterpret_cast<std::vector<uint8_t>*>(hMem);
    return v->empty() ? nullptr : v->data();
}

BOOL GlobalUnlock(HGLOBAL /*hMem*/) { return TRUE; }

SIZE_T GlobalSize(HGLOBAL hMem) {
    if (!hMem) return 0;
    auto* v = reinterpret_cast<std::vector<uint8_t>*>(hMem);
    return v->size();
}

HGLOBAL GlobalFree(HGLOBAL hMem) {
    if (!hMem) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        if (g_lastClipboardHandle == hMem) g_lastClipboardHandle = nullptr;
    }
    delete reinterpret_cast<std::vector<uint8_t>*>(hMem);
    return nullptr;
}

BOOL OpenClipboard(HWND /*hWndNewOwner*/) { return TRUE; }
BOOL CloseClipboard() { return TRUE; }

BOOL EmptyClipboard() {
    std::lock_guard<std::mutex> lock(g_winMtx);
    g_clipboardText.clear();
    if (g_lastClipboardHandle) {
        delete reinterpret_cast<std::vector<uint8_t>*>(g_lastClipboardHandle);
        g_lastClipboardHandle = nullptr;
    }
    return TRUE;
}

BOOL IsClipboardFormatAvailable(UINT format) {
    if (format != CF_UNICODETEXT) return FALSE;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_HasClipboardText && sdl.SDL_HasClipboardText()) return TRUE;
    std::lock_guard<std::mutex> lock(g_winMtx);
    return !g_clipboardText.empty() ? TRUE : FALSE;
}

HANDLE GetClipboardData(UINT uFormat) {
    if (uFormat != CF_UNICODETEXT) return nullptr;
    std::string text;
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_GetClipboardText) {
        if (char* p = sdl.SDL_GetClipboardText()) {
            text = p;
            if (sdl.SDL_free) sdl.SDL_free(p);
        }
    }
    if (text.empty()) {
        std::lock_guard<std::mutex> lock(g_winMtx);
        text = g_clipboardText;
    }
    std::wstring wide = LinuxUtf8ToWide(text);

    std::lock_guard<std::mutex> lock(g_winMtx);
    if (g_lastClipboardHandle) {
        delete reinterpret_cast<std::vector<uint8_t>*>(g_lastClipboardHandle);
        g_lastClipboardHandle = nullptr;
    }
    auto* buf = new std::vector<uint8_t>((wide.size() + 1) * sizeof(wchar_t), 0);
    std::memcpy(buf->data(), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    HGLOBAL h = reinterpret_cast<HGLOBAL>(buf);
    g_lastClipboardHandle = h;
    return h;
}

HANDLE SetClipboardData(UINT uFormat, HANDLE hMem) {
    if (uFormat != CF_UNICODETEXT || !hMem) return nullptr;
    const wchar_t* wptr = static_cast<const wchar_t*>(GlobalLock(hMem));
    std::string utf8 = wptr ? LinuxWideToUtf8(wptr) : "";
    {
        std::lock_guard<std::mutex> lock(g_winMtx);
        g_clipboardText = utf8;
    }
    auto& sdl = Sdl2Api::Get();
    if (sdl.available && sdl.SDL_SetClipboardText) {
        sdl.SDL_SetClipboardText(utf8.c_str());
    }
    return hMem;
}

// ============================================================================
// Drag & Drop, ShellExecute & Native File Dialogs
// ============================================================================

UINT DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch) {
    auto* dd = reinterpret_cast<LinuxDropData*>(hDrop);
    if (!dd) return 0;
    if (iFile == 0xFFFFFFFF) return static_cast<UINT>(dd->files.size());
    if (iFile >= dd->files.size()) return 0;
    const std::wstring& path = dd->files[iFile];
    if (!lpszFile || cch == 0) return static_cast<UINT>(path.size());
    UINT n = std::min<UINT>(cch - 1, static_cast<UINT>(path.size()));
    std::wmemcpy(lpszFile, path.c_str(), n);
    lpszFile[n] = L'\0';
    return n;
}

void DragFinish(HDROP hDrop) {
    delete reinterpret_cast<LinuxDropData*>(hDrop);
}

void DragAcceptFiles(HWND hWnd, BOOL fAccept) {
    std::lock_guard<std::mutex> lock(g_winMtx);
    if (LinuxWindow* w = GetWindowLocked(hWnd)) w->acceptsDrop = (fAccept != FALSE);
}

HINSTANCE ShellExecuteW(HWND /*hwnd*/, LPCWSTR lpOperation, LPCWSTR lpFile,
                        LPCWSTR lpParameters, LPCWSTR lpDirectory, int /*nShowCmd*/) {
    if (!lpFile || !*lpFile) return reinterpret_cast<HINSTANCE>(2);
    std::string target = LinuxWinPathToPosix(lpFile);
    std::string workDir = (lpDirectory && *lpDirectory) ? LinuxWinPathToPosix(lpDirectory) : "";
    bool isExplore = (lpOperation && (_wcsicmp(lpOperation, L"explore") == 0 || _wcsicmp(lpOperation, L"open") == 0));

    if (lpParameters && *lpParameters) {
        std::vector<std::string> args;
        args.push_back(target);
        std::string paramsUtf8 = LinuxWideToUtf8(lpParameters);
        std::stringstream ss(paramsUtf8);
        std::string tok;
        while (ss >> tok) args.push_back(tok);
        if (LaunchDetachedProcess(args, workDir)) return reinterpret_cast<HINSTANCE>(33);
    }

    // If it's a URL or file opened via desktop handler
    if (isExplore || !lpOperation || access(target.c_str(), X_OK) != 0) {
        if (LaunchDetachedProcess({"xdg-open", target}, workDir)) {
            return reinterpret_cast<HINSTANCE>(33);
        }
    }

    if (LaunchDetachedProcess({target}, workDir)) {
        return reinterpret_cast<HINSTANCE>(33);
    }
    return reinterpret_cast<HINSTANCE>(2);
}

BOOL ShellExecuteExW(SHELLEXECUTEINFOW* pExecInfo) {
    if (!pExecInfo) return FALSE;
    HINSTANCE res = ShellExecuteW(pExecInfo->hwnd, pExecInfo->lpVerb, pExecInfo->lpFile,
                                  pExecInfo->lpParameters, pExecInfo->lpDirectory, pExecInfo->nShow);
    pExecInfo->hInstApp = res;
    return reinterpret_cast<uintptr_t>(res) > 32 ? TRUE : FALSE;
}

BOOL Shell_NotifyIconW(DWORD /*dwMessage*/, NOTIFYICONDATAW* /*lpData*/) { return TRUE; }
BOOL RegisterHotKey(HWND /*hWnd*/, int /*id*/, UINT /*fsModifiers*/, UINT /*vk*/) { return TRUE; }
BOOL UnregisterHotKey(HWND /*hWnd*/, int /*id*/) { return TRUE; }

HANDLE LoadImageW(HINSTANCE /*hInst*/, LPCWSTR /*name*/, UINT /*type*/, int /*cx*/, int /*cy*/, UINT /*fuLoad*/) { return nullptr; }
HICON CreateIconIndirect(ICONINFO* /*piconinfo*/) { return reinterpret_cast<HICON>(0x5002); }
BOOL DestroyIcon(HICON /*hIcon*/) { return TRUE; }
HMENU CreatePopupMenu() { return reinterpret_cast<HMENU>(0x6001); }
BOOL AppendMenuW(HMENU /*hMenu*/, UINT /*uFlags*/, UINT_PTR /*uIDNewItem*/, LPCWSTR /*lpNewItem*/) { return TRUE; }
BOOL TrackPopupMenu(HMENU /*hMenu*/, UINT /*uFlags*/, int /*x*/, int /*y*/, int /*nReserved*/, HWND /*hWnd*/, const RECT* /*prcRect*/) { return 0; }
BOOL DestroyMenu(HMENU /*hMenu*/) { return TRUE; }
int MessageBoxW(HWND /*hWnd*/, LPCWSTR /*lpText*/, LPCWSTR /*lpCaption*/, UINT /*uType*/) { return IDOK; }

HRESULT IShellItem::GetDisplayName(SIGDN /*sigdnName*/, LPWSTR* ppszName) {
    if (!ppszName) return E_POINTER;
    size_t len = m_path.size();
    WCHAR* p = static_cast<WCHAR*>(CoTaskMemAlloc((len + 1) * sizeof(WCHAR)));
    if (!p) return E_OUTOFMEMORY;
    std::memcpy(p, m_path.c_str(), (len + 1) * sizeof(WCHAR));
    *ppszName = p;
    return S_OK;
}

HRESULT IFileDialog::Show(HWND /*hwndOwner*/) {
    FileDialogSpec spec;
    spec.isSave = (m_options & FOS_OVERWRITEPROMPT) != 0;
    spec.pickFolders = (m_options & FOS_PICKFOLDERS) != 0;
    spec.multiSelect = (m_options & FOS_ALLOWMULTISELECT) != 0;
    spec.title = LinuxWideToUtf8(m_title);
    spec.initialDir = LinuxWinPathToPosix(m_initialFolder);
    spec.defaultExt = LinuxWideToUtf8(m_defaultExt);
    for (const auto& f : m_filters) {
        spec.filters.emplace_back(LinuxWideToUtf8(f.first), LinuxWideToUtf8(f.second));
    }

    auto selected = RunNativeFileDialog(spec);
    if (selected.empty()) return HRESULT_FROM_WIN32(ERROR_CANCELLED);

    m_resultPath = selected[0];
    if (auto* openDlg = dynamic_cast<IFileOpenDialog*>(this)) {
        openDlg->m_resultPaths = selected;
    }
    return S_OK;
}

static std::vector<std::pair<std::string, std::string>> ParseOfnFilters(LPCWSTR filter) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!filter) return out;
    const wchar_t* p = filter;
    while (*p) {
        std::wstring label = p;
        p += label.size() + 1;
        if (!*p) break;
        std::wstring pattern = p;
        p += pattern.size() + 1;
        out.emplace_back(LinuxWideToUtf8(label), LinuxWideToUtf8(pattern));
    }
    return out;
}

static BOOL RunOpenSaveDialog(LPOPENFILENAMEW lpofn, bool isSave) {
    if (!lpofn || !lpofn->lpstrFile || lpofn->nMaxFile == 0) return FALSE;
    FileDialogSpec spec;
    spec.isSave = isSave;
    spec.multiSelect = !isSave && ((lpofn->Flags & OFN_ALLOWMULTISELECT) != 0);
    if (lpofn->lpstrTitle) spec.title = LinuxWideToUtf8(lpofn->lpstrTitle);
    if (lpofn->lpstrInitialDir) spec.initialDir = LinuxWinPathToPosix(lpofn->lpstrInitialDir);
    if (lpofn->lpstrDefExt) spec.defaultExt = LinuxWideToUtf8(lpofn->lpstrDefExt);
    spec.filters = ParseOfnFilters(lpofn->lpstrFilter);

    auto selected = RunNativeFileDialog(spec);
    if (selected.empty()) return FALSE;

    if (selected.size() == 1 || !spec.multiSelect) {
        std::wstring res = selected[0];
        size_t n = std::min<size_t>(lpofn->nMaxFile - 1, res.size());
        std::wmemcpy(lpofn->lpstrFile, res.c_str(), n);
        lpofn->lpstrFile[n] = L'\0';
        size_t slash = res.find_last_of(L"/\\");
        lpofn->nFileOffset = (slash != std::wstring::npos && slash + 1 < n) ? static_cast<WORD>(slash + 1) : 0;
        return TRUE;
    }

    // Multiple selection OFN format: "<dir>\0<file1>\0<file2>\0\0"
    size_t pos = selected[0].find_last_of(L"/\\");
    std::wstring dir = (pos != std::wstring::npos) ? selected[0].substr(0, pos) : L"";
    DWORD written = 0;
    auto appendStr = [&](const std::wstring& s) -> bool {
        if (written + s.size() + 2 > lpofn->nMaxFile) return false;
        std::wmemcpy(lpofn->lpstrFile + written, s.c_str(), s.size());
        written += static_cast<DWORD>(s.size());
        lpofn->lpstrFile[written++] = L'\0';
        return true;
    };
    if (!appendStr(dir)) return FALSE;
    lpofn->nFileOffset = static_cast<WORD>(written);
    for (const auto& full : selected) {
        size_t p = full.find_last_of(L"/\\");
        std::wstring base = (p != std::wstring::npos) ? full.substr(p + 1) : full;
        if (!appendStr(base)) break;
    }
    lpofn->lpstrFile[written] = L'\0';
    return TRUE;
}

BOOL GetOpenFileNameW(LPOPENFILENAMEW lpofn) {
    return RunOpenSaveDialog(lpofn, false);
}

BOOL GetSaveFileNameW(LPOPENFILENAMEW lpofn) {
    return RunOpenSaveDialog(lpofn, true);
}

HIMC ImmGetContext(HWND /*hWnd*/) { return reinterpret_cast<HIMC>(0x7001); }
BOOL ImmSetCompositionWindow(HIMC /*hIMC*/, COMPOSITIONFORM* /*lpCompForm*/) { return TRUE; }
BOOL ImmReleaseContext(HWND /*hWnd*/, HIMC /*hIMC*/) { return TRUE; }
BOOL RegisterTouchWindow(HWND /*hwnd*/, ULONG /*ulFlags*/) { return TRUE; }
BOOL UnregisterTouchWindow(HWND /*hwnd*/) { return TRUE; }
BOOL GetTouchInputInfo(HTOUCHINPUT /*hTouchInput*/, UINT /*cInputs*/, PTOUCHINPUT /*pInputs*/, int /*cbSize*/) { return FALSE; }
BOOL CloseTouchInputHandle(HTOUCHINPUT /*hTouchInput*/) { return TRUE; }
